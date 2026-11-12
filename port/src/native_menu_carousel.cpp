#include "native_menu_carousel.h"
#include "ssne/recomp_hooks.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace port { namespace {
constexpr uint16_t carousel=0x4b,thumbnail_id=0x4000;
struct Rect {
    float l=0,t=0,r=0,b=0;
    bool valid()const{return std::isfinite(l)&&std::isfinite(t)&&std::isfinite(r)&&std::isfinite(b)&&r>l&&b>t&&r-l<4096&&b-t<4096;}
};
Rect clip(Rect a,Rect b){return {std::max(a.l,b.l),std::max(a.t,b.t),std::min(a.r,b.r),std::min(a.b,b.b)};}
bool readable(uint32_t a,unsigned n){return a>=0x80000000u&&n<=ssne::guest_ram_size()&&a<=ssne::guest_ram_end()-n;}
uint32_t word(uint32_t a){return readable(a,4)?mem_read32(a):0;}
uint16_t half(uint32_t a){return readable(a,2)?mem_read16(a):0;}
float number(uint32_t a){return u2f(word(a));}
bool revision(){
    // These fields and callback layouts are specific to the owned PAL binary.
    static const bool valid=[] {
        if(word(0x80000000)!=0x47334250u)return false;
        constexpr std::array<std::pair<uint32_t,uint64_t>,6> expected{{
            {0x800f6c14,0x26f312d446cf8159ull},{0x800f31c8,0xe06797b4ec35e4b9ull},
            {0x800fe2cc,0x24f1f308b1281c71ull},{0x800fe9ac,0xa8038e07ad554e59ull},
            {0x800feaac,0x40b581c055638b12ull},{0x8010c8ac,0xb00f7bf968750709ull}}};
        for(auto [address,hash]:expected){
            if(!ssne_find_func(address))return false;
            uint64_t actual=14695981039346656037ull;
            for(unsigned i=0;i<32;++i){actual^=mem_read8(address+i);actual*=1099511628211ull;}
            if(actual!=hash)return false;
        }
        return true;
    }();
    return valid;
}
uint32_t binding(uint32_t gui,unsigned event,uint32_t function){
    // 800F6C14 retains {next+8,owner+10,event+14,id+18,callback+1C}.
    auto p=word(gui+0x94);std::set<uint32_t> seen;
    while(readable(p,0x20)&&seen.size()<2048&&seen.insert(p).second){
        if(half(p+0x18)==carousel&&word(p+0x14)==event&&word(p+0x1c)==function)return word(p+0x10);
        p=word(p+8);
    }
    return 0;
}
struct State {uint32_t gui=0,node=0,owner=0,entries=0,count=0,selected=0;bool category=false;};
bool state(uint32_t gui,State& out){
    if(!readable(gui,0xa8)||!revision())return false;
    auto root=word(gui+0x7c);std::set<uint32_t> parents;
    while(readable(root,0xac)&&word(root+0x24)&&parents.size()<32&&parents.insert(root).second)root=word(root+0x24);
    if(!root)root=word(gui+0x6c);
    if(!readable(root,0xac)||word(root+0x20)!=gui)return false;
    const auto definition=word(root+0x1c);
    if(!readable(definition,0x48)||half(definition+2)!=carousel||(word(definition+0xc)&0x72u))return false;
    bool linked=false;auto p=word(gui+0x6c);std::set<uint32_t> roots;
    while(readable(p,0xac)&&roots.size()<64&&roots.insert(p).second){if(p==root)linked=true;p=word(p+8);}
    if(!linked)return false;
    const auto owner=binding(gui,20,0x800fe2cc);
    if(!readable(owner,0x54)||binding(gui,21,0x800fe2cc)!=owner||binding(gui,22,0x800fe9ac)!=owner||!word(owner+0x20))return false;
    const auto count=word(owner+0x2c),selected=word(owner+0x30),entries=word(owner+0x28);
    if(!count||count>256||selected>=count||!readable(entries,(count+10)*12))return false;
    out={gui,root,owner,entries,count,selected,binding(gui,25,0x800feaac)==owner};return true;
}
std::string authored_level(const State& s,unsigned index){
    // Exact read-only table lookup performed by original 8010C8AC. Entries
    // are rebuilt by the game; never assume tutorial/campaign index ordering.
    const auto category=word(s.owner+0x48);
    if(category>=3||index>=s.count||index>=word(0x8021a770u+category*4))return {};
    const auto table=word(0x8023ac78u+category*4);
    if(!readable(table,(index+1)*12))return {};
    const auto text=word(table+index*12+8);std::string level;
    for(unsigned i=0;i<=64;++i){
        if(!readable(text+i,1))return {};
        const auto c=mem_read8(text+i);if(!c)return level;
        if(i==64||!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))return {};
        level+=char(c);
    }
    return {};
}
Rect sprite_rect(uint32_t wrapper){
    if(!readable(wrapper,0x18)||!word(wrapper+8))return {};
    const auto params=word(wrapper),position=word(params),size=word(params+8),alpha=word(params+0x18);
    if(!readable(params,0x28)||!readable(position,8)||!readable(size,8)||!readable(alpha,8)||number(alpha)<=.01f)return {};
    // 800F31C8 sends these current (interpolated) min/max points to the native
    // primitive. The descriptor's nominal 640x480 canvas is unrelated.
    const auto x=number(position),y=number(position+4),w=number(size),h=number(size+4);
    Rect result{x,y,x+w,y+h};return result.valid()?result:Rect{};
}
Rect title_rect(uint32_t root){
    auto node=word(root+0x34);std::set<uint32_t> seen;
    while(readable(node,0xc0)&&seen.size()<64&&seen.insert(node).second){
        if(half(word(node+0x1c)+2)==0x4c){
            const auto helper=word(node+0xb8),text=word(helper+0x48);
            if(!readable(helper,0x94)||!readable(text,0x6c)||word(text)!=0x8021cb58u)return {};
            float x=number(text+0x14),baseline=number(text+0x18);
            if(word(helper+0x34)){
                const auto position=word(word(helper+4));if(!readable(position,8))return {};
                x+=number(position)+number(helper+0x3c);baseline+=number(position+4)+number(helper+0x40)+number(helper+0x24);
            }
            Rect result{x+number(helper+0x20),baseline-number(helper+0x18),x+number(helper+0x20)+number(helper+0x14),baseline};
            return result.valid()?result:Rect{};
        }
        node=word(node+8);
    }
    return {};
}
void add(std::vector<NativeMenuMouseRegion>& out,const State& s,uint16_t id,unsigned event,Rect r,const std::string& name,uint32_t primitive=0){
    // The displayed image rejects outside clicks. Do not trim the filmstrip
    // to 4:3: its live native thumbnails can remain visible in widescreen.
    if(!r.valid())return;
    NativeMenuMouseRegion region;region.gui=s.gui;region.node=s.node;region.root=carousel;region.id=id;
    region.left=r.l;region.top=r.t;region.right=r.r;region.bottom=r.b;region.event=event;
    region.previous_event=20;region.next_event=21;region.primitive=primitive;region.control=name;out.push_back(std::move(region));
}
bool send(PPCContext& base,uint32_t node,unsigned event){
    if(!ssne_find_func(0x800fb130)||!readable(base.r[1]-0x400,0x400))return false;
    auto c=base;c.r[1]-=0x400;c.msr&=~0x8000u;mem_write32(c.r[1],base.r[1]);c.r[3]=node;c.r[4]=event;ssne_call(c,0x800fb130);return true;
}
}
std::vector<NativeMenuMouseRegion> native_carousel_mouse_regions(PPCContext&){
    std::vector<NativeMenuMouseRegion> out;auto gui=word(0x8028bfa4u);std::set<uint32_t> seen;
    while(readable(gui,0xa8)&&seen.size()<32&&seen.insert(gui).second){
        State s;if(state(gui,s)){
            for(unsigned i=0;i<s.count;++i){
                if(i==s.selected)continue;
                const auto wrapper=word(s.entries+i*12);const auto box=sprite_rect(wrapper);
                const auto name=i+1==s.selected?"carousel.previous":i==s.selected+1?"carousel.next":"carousel.thumbnail."+std::to_string(i);
                add(out,s,uint16_t(thumbnail_id+i),22,box,name,word(wrapper+8));
            }
            // The two big previews scroll inside the original native clip.
            // Only the preview actually displaying the current selection can
            // confirm; the neighbouring transition sliver is not a button.
            const auto first_frame=sprite_rect(word(s.entries+4));
            if(first_frame.valid()){
                const int first=(int(-40-int(first_frame.l))/80);
                const Rect preview_clip{number(0x80218890),number(0x80218890),number(0x80218894),number(0x80218894)};
                for(int i=0;i<2;++i){
                    const int index=(first+i)%int(s.count);
                    if(index==int(s.selected)){const auto wrapper=word(s.owner+0x34+i*4);add(out,s,carousel,22,clip(sprite_rect(wrapper),preview_clip),"carousel.confirm",word(wrapper+8));}
                }
            }
            if(s.category)add(out,s,0x4c,25,title_rect(s.node),"carousel.category");
        }
        gui=word(gui+8);
    }
    return out;
}
std::string native_carousel_mouse_diagnostics(){
    std::ostringstream out;out<<"{\"surfaces\":[";bool comma=false;
    auto gui=word(0x8028bfa4u);std::set<uint32_t> seen;
    while(readable(gui,0xa8)&&seen.size()<32&&seen.insert(gui).second){
        State s;if(state(gui,s)){
            if(comma)out<<',';comma=true;
            out<<"{\"gui\":"<<s.gui<<",\"node\":"<<s.node<<",\"selected\":"<<s.selected
               <<",\"count\":"<<s.count<<",\"category\":"<<word(s.owner+0x48)
               <<",\"category_enabled\":"<<(s.category?"true":"false")
               <<",\"selected_level\":\""<<authored_level(s,s.selected)<<"\",\"entries\":[";
            for(unsigned i=0;i<s.count;++i){if(i)out<<',';out<<"{\"index\":"<<i<<",\"level\":\""<<authored_level(s,i)<<"\"}";}
            out<<"]}";
        }
        gui=word(gui+8);
    }
    out<<"]}";return out.str();
}
bool native_carousel_mouse_event(PPCContext& c,uint32_t gui,uint32_t node,uint16_t root,uint16_t id,unsigned event){
    if(root!=carousel)return false;
    State s;if(!state(gui,s)||s.node!=node)return false;
    if(event==23)return send(c,node,23);
    // A wheel intent uses the same original left/right events as the pad.
    if(event==18||event==19)return send(c,node,event==18?20:21);
    if(event==20||event==21)return send(c,node,event);
    if(id==carousel&&event==22)return send(c,node,22);
    if(id==0x4c&&event==25&&s.category)return send(c,node,25);
    if(id<thumbnail_id||unsigned(id-thumbnail_id)>=s.count||event!=22)return false;
    const unsigned target=id-thumbnail_id;
    // Thumbnail clicks traverse the original selection callback, preserving
    // sounds, unlock checks, metadata, scores and the preview animation.
    for(unsigned steps=0;steps<s.count&&s.selected!=target;++steps){
        const auto previous=s.selected;
        if(!send(c,node,target<s.selected?20:21)||!state(gui,s)||s.node!=node||s.selected==previous)return false;
    }
    return s.selected==target;
}
}
