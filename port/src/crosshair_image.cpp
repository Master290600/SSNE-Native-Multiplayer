#include "crosshair_image.h"
#include "gc/decompress.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace port { namespace {
namespace fs=std::filesystem;
[[noreturn]] void fail(const char* text){throw std::runtime_error(text);}
void need(bool ok,const char* text){if(!ok)fail(text);}
uint32_t be32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
unsigned be16(const uint8_t* p){return unsigned(p[0])<<8|p[1];}
uint32_t crc32(std::span<const uint8_t> bytes){
    static const auto table=[] {std::array<uint32_t,256> result{};for(unsigned i=0;i<256;++i){uint32_t value=i;for(unsigned n=0;n<8;++n)value=(value>>1)^((value&1)?0xedb88320u:0);result[i]=value;}return result;}();
    uint32_t value=~0u;for(const auto b:bytes)value=table[(value^b)&255]^(value>>8);return ~value;
}
uint32_t adler32(std::span<const uint8_t> bytes){uint32_t a=1,b=0;for(const auto v:bytes){a=(a+v)%65521;b=(b+a)%65521;}return b<<16|a;}
std::string utf8(const fs::path& path){const auto s=path.u8string();return {s.begin(),s.end()};}
std::string lower_ascii(std::string s){for(char& c:s)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return s;}
bool utf8_valid(std::string_view text){
    for(size_t i=0;i<text.size();){const auto first=uint8_t(text[i++]);if(first<0x80)continue;unsigned count=0;uint32_t code=0,minimum=0;
        if(first>=0xc2&&first<=0xdf){count=1;code=first&31;minimum=0x80;}
        else if(first>=0xe0&&first<=0xef){count=2;code=first&15;minimum=0x800;}
        else if(first>=0xf0&&first<=0xf4){count=3;code=first&7;minimum=0x10000;}else return false;
        if(count>text.size()-i)return false;for(unsigned n=0;n<count;++n){const auto c=uint8_t(text[i++]);if((c&0xc0)!=0x80)return false;code=(code<<6)|(c&63);}
        if(code<minimum||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return false;
    }return true;
}
struct PNG {
    unsigned width=0,height=0,depth=0,color=0,channels=0,interlace=0;
    bool header=false,data=false,data_ended=false,end=false,has_palette=false,has_alpha=false;
    std::vector<uint8_t> palette,alpha,compressed;
};
PNG parse(std::span<const uint8_t> bytes){
    constexpr uint8_t signature[]={137,80,78,71,13,10,26,10};
    need(bytes.size()>=8&&bytes.size()<=kCrosshairMaxFileBytes,"PNG file is empty or exceeds 4 MiB.");
    need(std::equal(std::begin(signature),std::end(signature),bytes.begin()),"Not a PNG image.");PNG p;size_t at=8;
    while(at<bytes.size()){
        need(bytes.size()-at>=12,"Truncated PNG chunk.");const auto length=be32(bytes.data()+at);need(length<=bytes.size()-at-12,"Truncated PNG payload.");
        const auto type=bytes.subspan(at+4,4),value=bytes.subspan(at+8,length);for(const auto c:type)need((c>='A'&&c<='Z')||(c>='a'&&c<='z'),"Invalid PNG chunk type.");
        need(!(type[2]&32),"Invalid PNG reserved chunk flag.");need(crc32(bytes.subspan(at+4,size_t(length)+4))==be32(bytes.data()+at+8+length),"PNG checksum mismatch.");
        const std::string name(reinterpret_cast<const char*>(type.data()),4);at+=size_t(length)+12;
        need(p.header||name=="IHDR","PNG header must be first.");if(p.data&&name!="IDAT")p.data_ended=true;
        if(name=="IHDR"){
            need(!p.header&&length==13,"Invalid or duplicate PNG header.");p.width=be32(value.data());p.height=be32(value.data()+4);p.depth=value[8];p.color=value[9];p.interlace=value[12];
            need(p.width&&p.height&&p.width<=kCrosshairMaxDimension&&p.height<=kCrosshairMaxDimension,"PNG dimensions must be 1 to 1024 pixels.");
            need(value[10]==0&&value[11]==0&&p.interlace<=1,"Unsupported PNG compression, filter or interlace.");
            const bool low=p.depth==1||p.depth==2||p.depth==4,wide=p.depth==8||p.depth==16;
            need((p.color==0&&(low||wide))||(p.color==2&&wide)||(p.color==3&&(low||p.depth==8))||((p.color==4||p.color==6)&&wide),"Unsupported PNG color type or bit depth.");
            p.channels=p.color==0||p.color==3?1:p.color==2?3:p.color==4?2:4;p.header=true;
        }else if(name=="PLTE"){
            need(!p.has_palette&&!p.data&&p.color!=0&&p.color!=4&&length&&length<=768&&length%3==0,"Invalid PNG palette.");
            need(p.color!=3||length/3<=(1u<<p.depth),"PNG palette exceeds indexed bit depth.");p.palette.assign(value.begin(),value.end());p.has_palette=true;
        }else if(name=="tRNS"){
            need(!p.has_alpha&&!p.data,"Invalid or duplicate PNG transparency.");const auto max=(1u<<p.depth)-1;
            if(p.color==0)need(length==2&&be16(value.data())<=max,"Invalid grayscale transparency.");
            else if(p.color==2)need(length==6&&be16(value.data())<=max&&be16(value.data()+2)<=max&&be16(value.data()+4)<=max,"Invalid RGB transparency.");
            else need(p.color==3&&p.has_palette&&length&&length<=p.palette.size()/3,"Invalid palette transparency.");
            p.alpha.assign(value.begin(),value.end());p.has_alpha=true;
        }else if(name=="IDAT"){
            need(!p.data_ended&&(p.color!=3||p.has_palette),"PNG data order or palette is invalid.");p.compressed.insert(p.compressed.end(),value.begin(),value.end());p.data=true;
        }else if(name=="IEND"){need(p.data&&length==0,"Invalid PNG end marker.");p.end=true;break;}
        else need((type[0]&32)!=0,"Unsupported critical PNG chunk.");
    }
    need(p.header&&p.data&&p.end&&at==bytes.size(),"Incomplete PNG or trailing data.");need(p.compressed.size()>=6,"Truncated PNG zlib stream.");
    need((p.compressed[0]&15)==8&&(p.compressed[0]>>4)<=7&&((p.compressed[0]<<8)|p.compressed[1])%31==0&&!(p.compressed[1]&32),"Invalid PNG zlib header.");return p;
}
unsigned paeth(unsigned a,unsigned b,unsigned c){const int prediction=int(a)+int(b)-int(c),pa=std::abs(prediction-int(a)),pb=std::abs(prediction-int(b)),pc=std::abs(prediction-int(c));return pa<=pb&&pa<=pc?a:pb<=pc?b:c;}
unsigned sample(const std::vector<uint8_t>& row,unsigned pixel,unsigned channel,const PNG& p){
    const size_t index=size_t(pixel)*p.channels+channel;if(p.depth==16)return be16(row.data()+index*2);if(p.depth==8)return row[index];const auto bit=index*p.depth;return(row[bit/8]>>(8-p.depth-(bit%8)))&((1u<<p.depth)-1);
}
uint8_t byte_sample(unsigned value,unsigned depth){return uint8_t(depth==16?value>>8:depth==8?value:value*255/((1u<<depth)-1));}
struct Pass {unsigned x=0,y=0,dx=1,dy=1,width=0,height=0;size_t row_bytes=0;};
std::vector<Pass> passes(const PNG& p){
    constexpr unsigned xs[]={0,4,0,2,0,1,0},ys[]={0,0,4,0,2,0,1},dx[]={8,8,4,4,2,2,1},dy[]={8,8,8,4,4,2,2};std::vector<Pass> result;
    for(unsigned i=0;i<(p.interlace?7u:1u);++i){Pass q;if(p.interlace){q.x=xs[i];q.y=ys[i];q.dx=dx[i];q.dy=dy[i];}q.width=p.width>q.x?(p.width-q.x+q.dx-1)/q.dx:0;q.height=p.height>q.y?(p.height-q.y+q.dy-1)/q.dy:0;if(!q.width||!q.height)continue;q.row_bytes=(size_t(q.width)*p.channels*p.depth+7)/8;result.push_back(q);}return result;
}
void pixel(const PNG& p,const std::vector<uint8_t>& row,unsigned x,unsigned target_x,unsigned target_y,CrosshairImage& image){
    uint8_t r=0,g=0,b=0,a=255;const auto v=sample(row,x,0,p);
    if(p.color==0||p.color==4){r=g=b=byte_sample(v,p.depth);if(p.color==4)a=byte_sample(sample(row,x,1,p),p.depth);else if(p.has_alpha&&v==be16(p.alpha.data()))a=0;}
    else if(p.color==3){need(v<p.palette.size()/3,"PNG palette index is out of range.");r=p.palette[v*3];g=p.palette[v*3+1];b=p.palette[v*3+2];if(v<p.alpha.size())a=p.alpha[v];}
    else {const auto green=sample(row,x,1,p),blue=sample(row,x,2,p);r=byte_sample(v,p.depth);g=byte_sample(green,p.depth);b=byte_sample(blue,p.depth);if(p.color==6)a=byte_sample(sample(row,x,3,p),p.depth);else if(p.has_alpha&&v==be16(p.alpha.data())&&green==be16(p.alpha.data()+2)&&blue==be16(p.alpha.data()+4))a=0;}
    const auto offset=(size_t(target_y)*p.width+target_x)*4;image.rgba[offset]=r;image.rgba[offset+1]=g;image.rgba[offset+2]=b;image.rgba[offset+3]=a;
    if(a){image.visible.left=std::min(image.visible.left,target_x);image.visible.top=std::min(image.visible.top,target_y);image.visible.right=std::max(image.visible.right,target_x+1);image.visible.bottom=std::max(image.visible.bottom,target_y+1);}
}
CrosshairImage decode(std::span<const uint8_t> bytes){
    const auto p=parse(bytes);const auto plan=passes(p);size_t total=0;for(const auto& q:plan)total+=(q.row_bytes+1)*q.height;
    need(total<=size_t(kCrosshairMaxDimension)*kCrosshairMaxDimension*8+kCrosshairMaxDimension*8,"PNG decoded image is too large.");std::vector<uint8_t> raw(total);std::string error;
    need(gc::decomp::inflate(p.compressed.data(),p.compressed.size()-4,raw.data(),raw.size(),true,&error),"PNG compressed data is invalid.");need(adler32(raw)==be32(p.compressed.data()+p.compressed.size()-4),"PNG zlib checksum mismatch.");
    CrosshairImage image;image.width=p.width;image.height=p.height;image.rgba.resize(size_t(p.width)*p.height*4);image.visible={p.width,p.height,0,0};size_t at=0;const auto bpp=std::max(1u,(p.channels*p.depth+7)/8);
    for(const auto& q:plan){std::vector<uint8_t> previous(q.row_bytes),row(q.row_bytes);for(unsigned y=0;y<q.height;++y){const auto filter=raw[at++];need(filter<=4,"Unknown PNG scanline filter.");
            for(size_t n=0;n<q.row_bytes;++n){const unsigned left=n>=bpp?row[n-bpp]:0,up=previous[n],diagonal=n>=bpp?previous[n-bpp]:0;const auto predicted=filter==0?0:filter==1?left:filter==2?up:filter==3?(left+up)/2:paeth(left,up,diagonal);row[n]=uint8_t(raw[at++]+predicted);}
            for(unsigned x=0;x<q.width;++x)pixel(p,row,x,q.x+x*q.dx,q.y+y*q.dy,image);previous.swap(row);
        }}
    need(image.visible.right>image.visible.left&&image.visible.bottom>image.visible.top,"PNG is fully transparent; the original crosshair will be kept.");return image;
}
}
bool decode_crosshair_png(std::span<const uint8_t> bytes,CrosshairImage& image,std::string& error){try{auto next=decode(bytes);image=std::move(next);error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}}
bool load_crosshair_png(const fs::path& path,CrosshairImage& image,std::string& error){try{
    std::error_code ec;const auto status=fs::symlink_status(path,ec);need(!ec&&fs::is_regular_file(status)&&!fs::is_symlink(status),"Crosshair PNG is missing or is not a regular file.");const auto length=fs::file_size(path,ec);need(!ec&&length>=8&&length<=kCrosshairMaxFileBytes,"PNG file is empty or exceeds 4 MiB.");std::ifstream file(path,std::ios::binary);need(bool(file),"Cannot open crosshair PNG.");std::vector<uint8_t> bytes(size_t(length),0);file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));need(bool(file)&&file.peek()==std::char_traits<char>::eof(),"Cannot read the complete crosshair PNG.");return decode_crosshair_png(bytes,image,error);
}catch(const std::exception& e){error=e.what();return false;}}
bool safe_crosshair_filename(std::string_view name){
    if(name.empty()||name.size()>255||!utf8_valid(name)||name.back()=='.'||name.back()==' ')return false;
    for(const unsigned char c:name)if(c<32||c==127||c=='/'||c=='\\'||c==':'||c=='<'||c=='>'||c=='"'||c=='|'||c=='?'||c=='*')return false;
    const auto lower=lower_ascii(std::string(name));if(lower.size()<5||!lower.ends_with(".png"))return false;const auto first_dot=lower.find('.');auto stem=lower.substr(0,first_dot);while(!stem.empty()&&stem.back()==' ')stem.pop_back();
    if(stem=="con"||stem=="prn"||stem=="aux"||stem=="nul"||stem=="conin$"||stem=="conout$"||stem=="clock$"||(stem.size()==4&&(stem.starts_with("com")||stem.starts_with("lpt"))&&stem[3]>='1'&&stem[3]<='9'))return false;return true;
}
std::vector<std::string> list_crosshair_pngs(const fs::path& directory,std::string& error){try{
    std::vector<std::string> names;std::error_code ec;if(!fs::exists(directory,ec)&&!ec){error.clear();return names;}need(!ec&&fs::is_directory(directory,ec),"Cannot read the crosshairs folder.");size_t visited=0;
    for(const auto& entry:fs::directory_iterator(directory)){need(++visited<=4096,"Too many files in the crosshairs folder.");const auto status=entry.symlink_status();if(!fs::is_regular_file(status)||fs::is_symlink(status))continue;const auto name=utf8(entry.path().filename());if(safe_crosshair_filename(name))names.push_back(name);}
    std::sort(names.begin(),names.end(),[](const auto& a,const auto& b){const auto x=lower_ascii(a),y=lower_ascii(b);return x==y?a<b:x<y;});if(names.size()>kCrosshairMaxChoices)names.resize(kCrosshairMaxChoices);error.clear();return names;
}catch(const std::exception& e){error=e.what();return {};}}
}
