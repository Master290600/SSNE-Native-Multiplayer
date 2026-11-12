#include "ssne/input_glyphs.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace ssne {
namespace {
std::shared_ptr<const InputGlyphConfig> configuration=std::make_shared<InputGlyphConfig>();
struct Template { int width, height; InputGlyphPatch patch; uint64_t hash; };
const Template templates[] = {
#include "input_glyph_templates.inc"
};
uint64_t region_hash(int width, const std::vector<uint32_t>& pixels, const InputGlyphPatch& r) {
    uint64_t hash = 14695981039346656037ull;
    for (int y=r.y;y<r.y+r.h;++y) for(int x=r.x;x<r.x+r.w;++x) {
        uint32_t p=pixels[size_t(y)*width+x];
        for(int c=0;c<3;++c) hash=(hash ^ ((p>>(8*c))&255))*1099511628211ull;
    }
    return hash;
}
// Five-column pixel letters keep labels readable inside the original 20–44 px glyph slots.
const char* letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-+<>?/";
const uint8_t font[][5] = {
 {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},{127,73,73,73,65},
 {127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},{65,65,127,65,65},{32,64,65,63,1},
 {127,8,20,34,65},{127,64,64,64,64},{127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},
 {127,9,9,9,6},{62,65,81,33,94},{127,9,25,41,70},{38,73,73,73,50},{1,1,127,1,1},
 {63,64,64,64,63},{31,32,64,32,31},{127,32,24,32,127},{99,20,8,20,99},{3,4,120,4,3},{97,81,73,69,67},
 {62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},{24,20,18,127,16},
 {39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},{54,73,73,73,54},{6,73,73,41,30},
 {8,8,8,8,8},{8,8,62,8,8},{8,20,34,65,0},{0,65,34,20,8},{2,1,81,9,6},{32,16,8,4,2}
};
void draw_icon(int width, int height, std::vector<uint32_t>& pixels, const InputGlyphPatch& r, const InputGlyph& icon) {
    auto put=[&](int x,int y,uint32_t color) {
        if(x>=0&&y>=0&&x<r.w&&y<r.h&&r.x+x>=0&&r.y+y>=0&&r.x+x<width&&r.y+y<height)
            pixels[size_t(r.y+y)*width+r.x+x]=color;
    };
    for(int y=0;y<r.h;++y) for(int x=0;x<r.w;++x) put(x,y,0);
    const bool mouse=icon.shape>=GlyphShape::MouseLeft&&icon.shape<=GlyphShape::WheelDown;
    const bool pad=icon.shape>=GlyphShape::Pad;
    int left=1, right=r.w-2, top=1, bottom=r.h-2;
    if(mouse) { int iw=std::min(r.w-2,std::max(12,r.h*2/3));left=(r.w-iw)/2;right=left+iw-1; }
    for(int y=top;y<=bottom;++y) for(int x=left;x<=right;++x) {
        bool edge=x==left||x==right||y==top||y==bottom;
        if((x==left||x==right)&&(y==top||y==bottom)) continue;
        if(pad) { float nx=(x-(left+right)*.5f)/((right-left)*.5f),ny=(y-(top+bottom)*.5f)/((bottom-top)*.5f);
            float radius=nx*nx+ny*ny; if(radius>1.f)continue;edge=radius>.78f; }
        uint32_t shade=uint32_t(32+18*(bottom-y)/std::max(1,bottom-top));
        put(x,y,edge?0xffdae4f0u:0xff000000u|(shade+7)<<16|(shade+3)<<8|shade);
    }
    if(mouse) {
        int mid=(left+right)/2, split=top+std::max(5,(bottom-top)/3);
        for(int y=top+2;y<split;++y) for(int x=left+2;x<right-1;++x) {
            bool active=icon.shape==GlyphShape::MouseLeft?x<mid:icon.shape==GlyphShape::MouseRight?x>mid:abs(x-mid)<=1;
            if(active)put(x,y,0xff61cbffu);
        }
        for(int y=top+1;y<=split;++y)put(mid,y,0xffdae4f0u);
        for(int x=left+1;x<right;++x)put(x,split,0xffdae4f0u);
        if(icon.shape==GlyphShape::WheelUp||icon.shape==GlyphShape::WheelDown) {
            int cy=(split+bottom)/2, dir=icon.shape==GlyphShape::WheelUp?-1:1;
            for(int i=-3;i<=3;++i)put(mid+i,cy+dir*(3-abs(i)),0xffffffffu);
        }
        return;
    }
    if(icon.shape>=GlyphShape::Cross) {
        int cx=r.w/2,cy=r.h/2,rad=std::max(3,std::min(r.w,r.h)/4);
        for(int y=-rad;y<=rad;++y) for(int x=-rad;x<=rad;++x) {
            bool mark=false;
            if(icon.shape==GlyphShape::Cross)mark=abs(x-y)<=1||abs(x+y)<=1;
            if(icon.shape==GlyphShape::Circle)mark=abs(x*x+y*y-rad*rad)<=rad*2;
            if(icon.shape==GlyphShape::Square)mark=abs(x)==rad||abs(y)==rad;
            if(icon.shape==GlyphShape::Triangle)mark=y==rad||abs(abs(x)-(y+rad)/2)<=1;
            if(mark)put(cx+x,cy+y,0xffffffffu);
        }
        return;
    }
    std::string label=icon.label.empty()?"-":icon.label;
    // Fit long key names without shrinking letters to fractional pixels.
    int available=std::max(5,r.w-4);
    int columns=label.size()<=1?5:label.size()==2?5:4;
    while(int(label.size())*(columns+1)-1>available && columns>3)--columns;
    int tw=int(label.size())*(columns+1)-1;
    if(tw>available){label.resize(std::max(1,(available+1)/(columns+1)));tw=int(label.size())*(columns+1)-1;}
    int scale=std::max(1,std::min(available/std::max(1,tw),(r.h-4)/7));
    int tx=(r.w-tw*scale)/2,ty=(r.h-7*scale)/2;
    for(size_t i=0;i<label.size();++i) {
        char c=char(std::toupper(static_cast<unsigned char>(label[i])));
        const char* found=std::strchr(letters,c); if(!found)continue;
        const auto& glyph=font[found-letters];
        for(int x=0;x<columns;++x)for(int y=0;y<7;++y)if(glyph[x*4/(columns-1)]&(1<<y))
            for(int sy=0;sy<scale;++sy)for(int sx=0;sx<scale;++sx)put(tx+int(i)*(columns+1)*scale+x*scale+sx,ty+y*scale+sy,0xffffffffu);
    }
}
}
void set_input_glyphs(InputGlyphConfig config) {
    auto old=std::atomic_load(&configuration);
    if(old->enabled==config.enabled&&old->buttons==config.buttons && old->menu_buttons==config.menu_buttons && old->separate_menu==config.separate_menu)return;
    config.revision=old->revision+1;
    std::atomic_store(&configuration,std::make_shared<const InputGlyphConfig>(std::move(config)));
}
std::shared_ptr<const InputGlyphConfig> input_glyph_config(){return std::atomic_load(&configuration);}
std::vector<InputGlyphPatch> find_input_glyphs(int w,int h,const std::vector<uint32_t>& pixels) {
    std::vector<InputGlyphPatch> result;
    if(pixels.size()!=size_t(w)*h)return result;
    for(const auto& t:templates)if(t.width==w&&t.height==h&&region_hash(w,pixels,t.patch)==t.hash)result.push_back(t.patch);
    return result;
}
void render_input_glyphs(int w,int h,std::vector<uint32_t>& pixels,const std::vector<InputGlyphPatch>& patches,const InputGlyphConfig& config){
    if(!config.enabled||pixels.size()!=size_t(w)*h)return;
    for(const auto& r:patches)if(r.button>=0&&r.button<GLYPH_COUNT)draw_icon(w,h,pixels,r,config.buttons[r.button]);
}
}
