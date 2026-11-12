#include "native_configuration.h"
#include "ssne/ppc_context.h"
#include "ssne/recomp_hooks.h"
#include <set>
#include <optional>
#include <cmath>
#include <algorithm>
namespace port::coop {
namespace {
bool readable(uint32_t address,unsigned n){return address>=0x80000000u && n<=ssne::guest_ram_size() && address<=ssne::guest_ram_end()-n;}
std::optional<std::string> cstring(uint32_t address,unsigned limit) {
    if(!readable(address,1))return std::nullopt;
    std::string result;
    for(unsigned i=0;i<=limit;++i) {
        if(!readable(address+i,1))return std::nullopt;
        auto c=mem_read8(address+i);if(!c)return result;if(c<32 || c==127 || i==limit)return std::nullopt;result+=char(c);
    }
    return std::nullopt;
}
std::optional<std::string> native_string(uint32_t object,unsigned limit) {
    if(!readable(object,12))return std::nullopt;
    const auto table=mem_read32(object);if(!readable(table,0x20))return std::nullopt;
    auto method=mem_read32(table+0x1C);auto receiver=object+int16_t(mem_read16(table+0x18));
    if(!readable(method,8) || mem_read32(method+4)!=0x4E800020u)return std::nullopt;
    auto instruction=mem_read32(method);uint32_t text=0;
    if((instruction&0xFFFF0000u)==0x80630000u){auto pointer=receiver+int16_t(instruction);if(!readable(pointer,4))return std::nullopt;text=mem_read32(pointer);}
    else if((instruction&0xFFFF0000u)==0x38630000u)text=receiver+int16_t(instruction);
    else return std::nullopt;
    return cstring(text,limit);
}
std::optional<unsigned> type(uint32_t variable) {
    if(!readable(variable,0x28))return std::nullopt;
    const auto table=mem_read32(variable+0x14);if(!readable(table,0x10))return std::nullopt;
    const auto method=mem_read32(table+0xC);if(!readable(method,8) || mem_read32(method+4)!=0x4E800020u)return std::nullopt;
    const auto instruction=mem_read32(method);if((instruction&0xFFFF0000u)!=0x38600000u)return std::nullopt;
    auto value=unsigned(instruction&0xFFFF);return value<=4?std::optional<unsigned>(value):std::nullopt;
}
struct Visitor {
    NativeConfigurationSample sample;
    std::string scope;
    std::set<uint32_t> roots,nodes;
    std::map<std::string,std::pair<uint32_t,unsigned>> bindings;
    void tree(uint32_t root,const std::string& path,unsigned depth) {
        if(depth>32 || !readable(root,4) || !roots.insert(root).second){sample.error="Invalid or cyclic native configuration stash";return;}
        auto node=mem_read32(root);
        while(node && sample.error.empty()) {
            if(!readable(node,0x28) || !nodes.insert(node).second || nodes.size()>16384){sample.error="Invalid or cyclic native configuration node";return;}
            auto name=native_string(node+8,128);auto kind=type(node);
            if(!name || !kind || name->empty()){++sample.unsupported;node=mem_read32(node);continue;}
            auto key=path.empty()?*name:path+"."+*name;
            if(key.size()>128){++sample.unsupported;node=mem_read32(node);continue;}
            // A scoped query only needs its ancestors and the requested branch.
            // In particular, per-frame HideWeapons replication must not read
            // every player's settings/string values before discarding them.
            const bool inside=scope.empty() || key==scope || key.starts_with(scope+".");
            const bool ancestor=!inside && scope.starts_with(key+".");
            if(!inside && !ancestor){node=mem_read32(node);continue;}
            bindings[key]={node,*kind};
            switch(*kind) {
            case 0:{float value=u2f(mem_read32(node+0x24));if(std::isfinite(value))sample.values[key]=value;else ++sample.unsupported;break;}
            case 1:sample.values[key]=int64_t(int32_t(mem_read32(node+0x24)));break;
            case 2:sample.values[key]=int64_t(mem_read32(node+0x1C)!=0);break;
            case 3:{auto value=native_string(node+0x24,512);if(value)sample.values[key]=*value;else ++sample.unsupported;break;}
            case 4:tree(mem_read32(node+0x18),key,depth+1);break;
            }
            node=mem_read32(node);
        }
    }
};
}
std::optional<std::string> read_native_text(uint32_t object,unsigned limit){return native_string(object,limit);}
uint32_t native_global_configuration(){return mem_read32(mem_read32(0x80000000u)==0x47334250u?0x802ABB60u:0x802AB460u);}
NativeConfigurationSample read_native_configuration(uint32_t root,const std::string& prefix) {
    Visitor visitor;visitor.scope=prefix;visitor.tree(root,{},0);
    if(!prefix.empty())for(auto it=visitor.sample.values.begin();it!=visitor.sample.values.end();) {
        if(it->first!=prefix && !it->first.starts_with(prefix+"."))it=visitor.sample.values.erase(it);else ++it;
    }
    return std::move(visitor.sample);
}
static bool configure_native(PPCContext& context,uint32_t root,const Properties& values,const std::string& prefix,std::string& error,bool commit) {
    if(prefix.empty()){error="Native configuration write scope is empty";return false;}
    Visitor visitor;visitor.scope=prefix;visitor.tree(root,{},0);if(!visitor.sample.error.empty()){error=visitor.sample.error;return false;}
    struct Scalar {uint32_t address,value;};
    struct Text {uint32_t object;std::string value;};
    std::vector<Scalar> scalars;std::vector<Text> strings;
    for(const auto& [key,value]:values) {
        if(key!=prefix && !key.starts_with(prefix+".")){error="Native configuration field leaves authorized scope";return false;}
        auto found=visitor.bindings.find(key);if(found==visitor.bindings.end()){error="Native configuration field is missing: "+key;return false;}
        const auto [node,kind]=found->second;
        if(kind==0) {
            auto f=std::get_if<float>(&value);if(!f || !std::isfinite(*f) || *f<u2f(mem_read32(node+0x18)) || *f>u2f(mem_read32(node+0x1C))){error="Invalid native float/range: "+key;return false;}
            scalars.push_back({node+0x24,f2u(*f)});
        } else if(kind==1) {
            auto n=std::get_if<int64_t>(&value);if(!n || *n<int32_t(mem_read32(node+0x18)) || *n>int32_t(mem_read32(node+0x1C))){error="Invalid native integer/range: "+key;return false;}
            scalars.push_back({node+0x24,uint32_t(*n)});
        } else if(kind==2) {
            auto n=std::get_if<int64_t>(&value);if(!n || (*n!=0 && *n!=1)){error="Invalid native flag: "+key;return false;}scalars.push_back({node+0x1C,uint32_t(*n)});
        } else if(kind==3) {
            auto text=std::get_if<std::string>(&value);
            if(!text || text->size()>512 || text->find('\0')!=std::string::npos || std::any_of(text->begin(),text->end(),[](unsigned char c){return c<32 || c==127;})){error="Invalid native string: "+key;return false;}
            if(!native_string(node+0x24,512)){error="Native string storage cannot be read";return false;}strings.push_back({node+0x24,*text});
        } else {error="Native stash cannot be assigned as a scalar";return false;}
    }
    const bool pal=mem_read32(0x80000000u)==0x47334250u;
    const auto assign=pal?0x80159DB8u:0x80159E00u;
    if(!strings.empty() && (!ssne_find_func(assign) || !readable(context.r[1]-0x800,0x800))){error="Native string assignment context unavailable";return false;}
    if(!commit)return true;
    std::vector<uint8_t> stack;
    if(!strings.empty()) {
        stack.resize(0x800);for(unsigned i=0;i<stack.size();++i)stack[i]=mem_read8(context.r[1]-0x800+i);
        try {
            for(const auto& text:strings) {
                PPCContext call=context;call.msr&=~0x8000u;call.r[1]-=0x100;mem_write32(call.r[1],context.r[1]);
                auto data=context.r[1]-0x500;for(unsigned i=0;i<=text.value.size();++i)mem_write8(data+i,i<text.value.size()?uint8_t(text.value[i]):0);
                call.r[3]=text.object;call.r[4]=data;ssne_call(call,assign);
            }
        } catch(...) {for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x800+i,stack[i]);throw;}
        for(unsigned i=0;i<stack.size();++i)mem_write8(context.r[1]-0x800+i,stack[i]);
    }
    for(const auto& scalar:scalars)mem_write32(scalar.address,scalar.value);
    return true;
}
bool validate_native_configuration(PPCContext& context,uint32_t root,const Properties& values,const std::string& prefix,std::string& error){return configure_native(context,root,values,prefix,error,false);}
bool write_native_configuration(PPCContext& context,uint32_t root,const Properties& values,const std::string& prefix,std::string& error){return configure_native(context,root,values,prefix,error,true);}
}
