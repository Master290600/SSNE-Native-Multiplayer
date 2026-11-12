#include "launcher_json.h"
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace launcher {
const JSONValue* JSONValue::find(std::string_view key)const{const auto p=object();if(!p)return nullptr;const auto it=p->find(key);return it==p->end()?nullptr:&it->second;}
bool JSONValue::integer(int64_t& out)const{if(const auto p=std::get_if<int64_t>(&value)){out=*p;return true;}return false;}
bool JSONValue::boolean(bool& out)const{if(const auto p=std::get_if<bool>(&value)){out=*p;return true;}return false;}
namespace {
bool utf8(std::string_view s){
    for(size_t i=0;i<s.size();){const auto c=uint8_t(s[i++]);if(c<0x80)continue;
        unsigned n;uint32_t cp,min;
        if(c>=0xc2&&c<=0xdf){n=1;cp=c&31;min=0x80;}
        else if(c>=0xe0&&c<=0xef){n=2;cp=c&15;min=0x800;}
        else if(c>=0xf0&&c<=0xf4){n=3;cp=c&7;min=0x10000;}else return false;
        if(n>s.size()-i)return false;
        for(unsigned j=0;j<n;++j){const auto x=uint8_t(s[i++]);if((x&0xc0)!=0x80)return false;cp=(cp<<6)|(x&63);}
        if(cp<min||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return false;
    }return true;
}
void append_utf8(std::string& s,uint32_t cp){
    if(cp<0x80)s+=char(cp);
    else if(cp<0x800){s+=char(0xc0|(cp>>6));s+=char(0x80|(cp&63));}
    else if(cp<0x10000){s+=char(0xe0|(cp>>12));s+=char(0x80|((cp>>6)&63));s+=char(0x80|(cp&63));}
    else{s+=char(0xf0|(cp>>18));s+=char(0x80|((cp>>12)&63));s+=char(0x80|((cp>>6)&63));s+=char(0x80|(cp&63));}
}
struct Reader {
    std::string_view in;JSONLimits limits;size_t at=0,nodes=0;
    [[noreturn]]void fail(const char* text)const{throw std::runtime_error("JSON byte "+std::to_string(at)+": "+text);}
    void space(){while(at<in.size()&&(in[at]==' '||in[at]=='\n'||in[at]=='\r'||in[at]=='\t'))++at;}
    bool take(char c){if(at<in.size()&&in[at]==c){++at;return true;}return false;}
    uint32_t hex4(){uint32_t n=0;for(unsigned i=0;i<4;++i){if(at==in.size())fail("short Unicode escape");const auto c=in[at++];unsigned d;if(c>='0'&&c<='9')d=c-'0';else if(c>='a'&&c<='f')d=c-'a'+10;else if(c>='A'&&c<='F')d=c-'A'+10;else fail("invalid Unicode escape");n=(n<<4)|d;}return n;}
    std::string string(){
        if(!take('"'))fail("expected string");std::string result;
        while(at<in.size()){unsigned char c=in[at++];if(c=='"')return result;if(c<0x20)fail("unescaped control character");
            if(c!='\\')result+=char(c);else{
                if(at==in.size())fail("short escape");const char e=in[at++];
                switch(e){case '"':case '\\':case '/':result+=e;break;case 'b':result+='\b';break;case 'f':result+='\f';break;case 'n':result+='\n';break;case 'r':result+='\r';break;case 't':result+='\t';break;
                case 'u':{auto cp=hex4();if(cp>=0xd800&&cp<=0xdbff){if(!take('\\')||!take('u'))fail("missing low surrogate");const auto low=hex4();if(low<0xdc00||low>0xdfff)fail("invalid low surrogate");cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);}else if(cp>=0xdc00&&cp<=0xdfff)fail("unexpected low surrogate");append_utf8(result,cp);break;}
                default:fail("invalid escape");}
            }
            if(result.size()>limits.max_string_bytes)fail("string limit exceeded");
        }fail("unterminated string");
    }
    JSONValue number(){
        const auto start=at;take('-');if(take('0')){if(at<in.size()&&in[at]>='0'&&in[at]<='9')fail("leading zero");}
        else{if(at==in.size()||in[at]<'1'||in[at]>'9')fail("invalid number");while(at<in.size()&&in[at]>='0'&&in[at]<='9')++at;}
        bool real=false;
        if(take('.')){real=true;const auto first=at;while(at<in.size()&&in[at]>='0'&&in[at]<='9')++at;if(first==at)fail("missing fraction");}
        if(at<in.size()&&(in[at]=='e'||in[at]=='E')){real=true;++at;if(at<in.size()&&(in[at]=='+'||in[at]=='-'))++at;const auto first=at;while(at<in.size()&&in[at]>='0'&&in[at]<='9')++at;if(first==at)fail("missing exponent");}
        if(at-start>128)fail("number limit exceeded");const auto text=in.substr(start,at-start);
        if(!real){int64_t n;const auto r=std::from_chars(text.data(),text.data()+text.size(),n);if(r.ec!=std::errc{}||r.ptr!=text.data()+text.size())fail("integer out of range");return n;}
        double d;std::istringstream stream{std::string(text)};stream.imbue(std::locale::classic());stream>>d;if(!stream||stream.peek()!=std::char_traits<char>::eof()||!std::isfinite(d))fail("number out of range");return d;
    }
    JSONValue value(unsigned depth){
        if(depth>limits.max_depth)fail("depth limit exceeded");if(++nodes>limits.max_nodes)fail("node limit exceeded");space();if(at==in.size())fail("missing value");
        if(in[at]=='"')return string();
        if(take('{')){JSONValue::Object result;space();if(take('}'))return result;
            do{space();const auto key=string();space();if(!take(':'))fail("expected colon");auto v=value(depth+1);if(!result.emplace(key,std::move(v)).second)fail("duplicate object key");space();if(take('}'))return result;}while(take(','));fail("expected object separator");}
        if(take('[')){JSONValue::Array result;space();if(take(']'))return result;
            do{result.push_back(value(depth+1));space();if(take(']'))return result;}while(take(','));fail("expected array separator");}
        for(auto literal:{std::string_view("true"),std::string_view("false"),std::string_view("null")})if(in.substr(at,literal.size())==literal){at+=literal.size();if(literal=="null")return nullptr;return literal=="true";}
        if(in[at]=='-'||(in[at]>='0'&&in[at]<='9'))return number();fail("invalid value");
    }
};
struct Writer {
    std::string out;bool pretty;size_t nodes=0;
    void line(unsigned d){if(pretty){out+='\n';out.append(d*2,' ');}}
    void string(std::string_view s){
        if(s.size()>16u*1024*1024)throw std::invalid_argument("JSON writer: string limit exceeded");if(!utf8(s))throw std::invalid_argument("JSON writer: invalid UTF-8");static constexpr char hex[]="0123456789abcdef";out+='"';
        for(unsigned char c:s){if(out.size()>16u*1024*1024)throw std::invalid_argument("JSON writer: byte limit exceeded");switch(c){case '"':out+="\\\"";break;case '\\':out+="\\\\";break;case '\b':out+="\\b";break;case '\f':out+="\\f";break;case '\n':out+="\\n";break;case '\r':out+="\\r";break;case '\t':out+="\\t";break;default:if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=char(c);}}
        out+='"';
    }
    void value(const JSONValue& v,unsigned depth){
        if(depth>32||++nodes>100000)throw std::invalid_argument("JSON writer: structure limit exceeded");
        if(std::holds_alternative<std::nullptr_t>(v.value))out+="null";
        else if(const auto p=std::get_if<bool>(&v.value))out+=*p?"true":"false";
        else if(const auto p=std::get_if<int64_t>(&v.value)){char b[32];const auto r=std::to_chars(b,b+sizeof(b),*p);out.append(b,r.ptr);}
        else if(const auto p=std::get_if<double>(&v.value)){if(!std::isfinite(*p))throw std::invalid_argument("JSON writer: non-finite number");char b[128];const auto r=std::to_chars(b,b+sizeof(b),*p,std::chars_format::general,std::numeric_limits<double>::max_digits10);if(r.ec!=std::errc{})throw std::invalid_argument("JSON writer: number formatting failed");out.append(b,r.ptr);}
        else if(const auto p=v.string())string(*p);
        else if(const auto p=v.array()){out+='[';bool comma=false;for(const auto& child:*p){if(comma)out+=',';line(depth+1);value(child,depth+1);comma=true;}if(comma)line(depth);out+=']';}
        else if(const auto p=v.object()){out+='{';bool comma=false;for(const auto& [key,child]:*p){if(comma)out+=',';line(depth+1);string(key);out+=pretty?": ":":";value(child,depth+1);comma=true;}if(comma)line(depth);out+='}';}
        if(out.size()>16u*1024*1024)throw std::invalid_argument("JSON writer: byte limit exceeded");
    }
};
}
bool parse_json(std::string_view input,JSONValue& out,std::string& error,JSONLimits limits){
    error.clear();if(input.size()>limits.max_bytes){error="JSON byte limit exceeded";return false;}if(!utf8(input)){error="JSON input is not valid UTF-8";return false;}
    try{Reader r{input,limits};auto result=r.value(0);r.space();if(r.at!=input.size())r.fail("trailing input");out=std::move(result);return true;}catch(const std::exception& e){error=e.what();return false;}
}
std::string write_json(const JSONValue& value,bool pretty){Writer w{{},pretty};w.value(value,0);return w.out;}
}
