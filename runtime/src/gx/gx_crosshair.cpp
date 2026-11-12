#include "gx_crosshair.h"
#include "ssne/custom_crosshair.h"
#include "ssne/gx.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace ssne { namespace {
struct Payload {
    bool enabled=false,native_color=true;
    int size=32,opacity=100;
    std::shared_ptr<const CustomCrosshairImage> image;
    std::shared_ptr<gx::Texture> texture;
};
std::mutex mutex;
std::shared_ptr<const Payload> latest;
std::unordered_map<uint64_t,std::shared_ptr<const Payload>> tickets;
uint64_t next_ticket=0;
std::atomic<uint64_t> next_texture{0x4000000000000001ull};
std::atomic<bool> ready{false},guest_scope{false};
CustomCrosshairStats stats;
constexpr size_t max_tickets=32;
// Only the command processor accesses these. UI publication never touches a queued draw.
std::shared_ptr<const Payload> current;
bool prepared=false;
int texture_map=-1,texture_coord=-1;
std::vector<std::shared_ptr<const gx::Texture>> software_kept;

bool valid_image(const CustomCrosshairImage& i){return i.width&&i.height&&i.width<=1024&&i.height<=1024&&i.left<i.right&&i.top<i.bottom&&i.right<=i.width&&i.bottom<=i.height&&i.rgba.size()==size_t(i.width)*i.height*4;}
void emit(uint64_t ticket){
    gx_fifo_write(gx::kCrosshairOpcode,1);gx_fifo_write(gx::kCrosshairMagic,1);
    gx_fifo_write(uint32_t(ticket>>32),4);gx_fifo_write(uint32_t(ticket),4);
}
bool retain_software(const std::shared_ptr<gx::Texture>& texture){
    if(gx::gpu_enabled())return true;
    if(std::find_if(software_kept.begin(),software_kept.end(),[&](const auto& old){return old.get()==texture.get();})!=software_kept.end())return true;
    if(software_kept.size()>=32)return false;software_kept.push_back(texture);return true;
}
}
void set_custom_crosshair(const CustomCrosshairConfig& cfg){
    const int size=std::clamp(cfg.size,4,256),opacity=std::clamp(cfg.opacity,0,100);
    std::shared_ptr<const Payload> previous;
    {std::lock_guard lock(mutex);previous=latest;if(previous&&previous->enabled==cfg.enabled&&previous->size==size&&previous->opacity==opacity&&previous->image==cfg.image&&previous->native_color==cfg.native_color)return;}
    std::shared_ptr<Payload> next;
    try{
        next=std::make_shared<Payload>();next->enabled=cfg.enabled;next->size=size;next->opacity=opacity;next->image=cfg.image;next->native_color=cfg.native_color;
        if(cfg.enabled){
            if(!cfg.image||!valid_image(*cfg.image))throw std::invalid_argument("Invalid custom crosshair image");
            if(previous&&previous->image==cfg.image&&previous->opacity==opacity&&previous->texture)next->texture=previous->texture;
            else{
                auto texture=std::make_shared<gx::Texture>();const auto& image=*cfg.image;
                texture->w=int(image.width);texture->h=int(image.height);texture->tex_fmt=6;texture->id=next_texture.fetch_add(1);
                texture->rgba.resize(size_t(image.width)*image.height);bool nonzero=false;
                for(unsigned y=0;y<image.height;++y)for(unsigned x=0;x<image.width;++x){const size_t n=size_t(y)*image.width+x,k=n*4;
                    const auto alpha=image.rgba[k+3];if(x>=image.left&&x<image.right&&y>=image.top&&y<image.bottom&&alpha)nonzero=true;
                    const uint32_t a=(unsigned(alpha)*unsigned(opacity)+50)/100;
                    texture->rgba[n]=uint32_t(image.rgba[k])|(uint32_t(image.rgba[k+1])<<8)|(uint32_t(image.rgba[k+2])<<16)|(a<<24);
                }
                if(!nonzero)throw std::invalid_argument("Invalid custom crosshair image");next->texture=std::move(texture);
            }
        }else if(previous&&previous->image==cfg.image&&previous->opacity==opacity)next->texture=previous->texture;
    }catch(...){next.reset();}
    std::lock_guard lock(mutex);latest=std::move(next);const bool active=latest&&latest->enabled&&latest->texture;
    ready.store(active,std::memory_order_release);stats.ready=active;if(cfg.enabled&&!active)++stats.rejected;
    if(active){const auto& image=*latest->image;stats.texture_id=latest->texture->id;stats.width=image.width;stats.height=image.height;stats.left=image.left;stats.top=image.top;stats.right=image.right;stats.bottom=image.bottom;}
}
bool custom_crosshair_active(){return ready.load(std::memory_order_acquire);}
void gx_custom_crosshair_scope(bool on){
    if(!on&&!guest_scope.load(std::memory_order_relaxed))return;
    uint64_t ticket=0;
    {
        std::lock_guard lock(mutex);
        if(on&&latest&&latest->enabled&&latest->texture&&tickets.size()<max_tickets){
            if(++next_ticket==0)++next_ticket;ticket=next_ticket;tickets.emplace(ticket,latest);
        }else if(on&&latest&&latest->enabled)++stats.dropped;
        if(!ticket&&!guest_scope.load(std::memory_order_relaxed))return;
        guest_scope.store(ticket!=0,std::memory_order_relaxed);++stats.queued;
    }
    emit(ticket);
}
CustomCrosshairStats custom_crosshair_stats(){std::lock_guard lock(mutex);return stats;}

namespace gx {
void custom_crosshair_marker(uint64_t ticket){
    std::shared_ptr<const Payload> next;
    {
        std::lock_guard lock(mutex);++stats.parsed;
        if(ticket){const auto found=tickets.find(ticket);if(found!=tickets.end()){next=std::move(found->second);tickets.erase(found);}else ++stats.dropped;}
        stats.stream_active=bool(next);stats.last_replaced=false;
    }
    if(prepared||current!=next)g_cfg_dirty=true;prepared=false;current=std::move(next);
}
bool custom_crosshair_stream_active(){return bool(current);}
void custom_crosshair_finish_draw(){if(prepared){prepared=false;g_cfg_dirty=true;}}
void custom_crosshair_software_flush(){software_kept.clear();}
void custom_crosshair_reset_stream(){
    current.reset();prepared=false;texture_map=texture_coord=-1;g_cfg_dirty=true;
    std::lock_guard lock(mutex);tickets.clear();guest_scope.store(false,std::memory_order_relaxed);stats.stream_active=false;stats.last_replaced=false;++stats.resets;
}
void custom_crosshair_apply_cfg(DrawCfg& c){
    if(!prepared||!current||texture_map<0||texture_coord<0)return;
    c.tex[texture_map]=current->texture.get();c.wrap_s[texture_map]=c.wrap_t[texture_map]=0;
    c.linear[texture_map]=true;c.min_filter[texture_map]=4;
    c.su_s[texture_coord]=float(current->image->width);c.su_t[texture_coord]=float(current->image->height);
    // PNGs carry smooth alpha, unlike a console cutout. Keep zero-alpha depth
    // behavior while allowing all visible alpha values and normal alpha blending.
    c.alpha_cmp=(4u<<16)|(7u<<19);c.atest_always=false;
    c.blend=(c.blend&~(0x700u|0xe0u|3u|(1u<<11)))|1u|(4u<<8)|(5u<<5);
    c.blend_kind=BLEND_ALPHA;
}
bool custom_crosshair_prepare(uint8_t primitive,std::vector<Vtx>& vertices){
    custom_crosshair_finish_draw();if(!current)return false;
    std::lock_guard lock(mutex);++stats.tags;stats.last_replaced=false;stats.last_primitive=primitive;stats.last_native_color=current->native_color;
    auto fallback=[] {++stats.fallbacks;return false;};
    const bool quad=vertices.size()==4&&(primitive==PRIM_QUADS||primitive==PRIM_QUADS2||primitive==PRIM_TRISTRIP||primitive==PRIM_TRIFAN);
    const bool triangles=vertices.size()==6&&primitive==PRIM_TRIANGLES;
    if((!quad&&!triangles)||!(g.xf_reg[0x26]&1))return fallback();
    const auto cfg=build_cfg();stats.last_stages=uint32_t(cfg.stages);stats.last_texmap=stats.last_texcoord=-1;
    if(cfg.stages!=1||cfg.nind||!cfg.st[0].tex_enabled)return fallback();
    const auto map=cfg.st[0].texmap,coord=cfg.st[0].texcoord;stats.last_texmap=map;stats.last_texcoord=coord;stats.last_cenv=cfg.st[0].cenv;stats.last_aenv=cfg.st[0].aenv;
    if(map<0||map>=8||coord<0||coord>=cfg.ntex||!cfg.tex[map])return fallback();
    const double vx=std::abs(double(xfr_f(0x1a))),vy=std::abs(double(xfr_f(0x1b)));
    if(!(vx>1e-5&&vy>1e-5&&std::isfinite(vx)&&std::isfinite(vy)))return fallback();
    double u0=1e30,v0=1e30,u1=-1e30,v1=-1e30;
    std::array<double,6> u{},v{};
    for(size_t n=0;n<vertices.size();++n){const auto& p=vertices[n];const double q=p.tex[coord][2]!=0?p.tex[coord][2]:1.;u[n]=p.tex[coord][0]/q;v[n]=p.tex[coord][1]/q;
        if(!(p.w>1e-5f)||!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||!std::isfinite(p.w)||!std::isfinite(u[n])||!std::isfinite(v[n]))return fallback();
        u0=std::min(u0,u[n]);u1=std::max(u1,u[n]);v0=std::min(v0,v[n]);v1=std::max(v1,v[n]);
    }
    if(!(u1-u0>1e-7&&v1-v0>1e-7))return fallback();
    std::array<std::array<double,2>,4> point{};unsigned seen=0;
    for(size_t n=0;n<vertices.size();++n){u[n]=(u[n]-u0)/(u1-u0);v[n]=(v[n]-v0)/(v1-v0);
        if(std::min(std::abs(u[n]),std::abs(u[n]-1))>1e-3||std::min(std::abs(v[n]),std::abs(v[n]-1))>1e-3)return fallback();
        u[n]=u[n]>.5?1:0;v[n]=v[n]>.5?1:0;const auto k=unsigned(u[n])+2u*unsigned(v[n]);
        const double x=vertices[n].x/vertices[n].w,y=vertices[n].y/vertices[n].w;
        if((seen&(1u<<k))&&(std::abs(point[k][0]-x)>1e-4||std::abs(point[k][1]-y)>1e-4))return fallback();point[k]={x,y};seen|=1u<<k;
    }
    if(seen!=15)return fallback();
    const double ux=point[1][0]-point[0][0],uy=point[1][1]-point[0][1],tx=point[2][0]-point[0][0],ty=point[2][1]-point[0][1];
    if(std::abs(point[3][0]-(point[0][0]+ux+tx))>1e-4||std::abs(point[3][1]-(point[0][1]+uy+ty))>1e-4)return fallback();
    const double lu=std::hypot(ux*vx,uy*vy),lv=std::hypot(tx*vx,ty*vy);
    if(!(lu>1e-5&&lv>1e-5&&lu<4096&&lv<4096))return fallback();
    const auto& image=*current->image;const double iw=image.right-image.left,ih=image.bottom-image.top;
    const double scale=std::max(lu,lv)*double(current->size)/32.,su=scale*(iw/std::max(iw,ih))/lu,sv=scale*(ih/std::max(iw,ih))/lv;
    const double cx=(point[0][0]+point[1][0]+point[2][0]+point[3][0])*.25,cy=(point[0][1]+point[1][1]+point[2][1]+point[3][1])*.25;
    if(!retain_software(current->texture))return fallback();
    double left=1e30,right=-1e30,top=-1e30,bottom=1e30,new_left=1e30,new_right=-1e30,new_top=-1e30,new_bottom=1e30;
    for(const auto& p:point){left=std::min(left,p[0]);right=std::max(right,p[0]);top=std::max(top,p[1]);bottom=std::min(bottom,p[1]);}
    for(size_t n=0;n<vertices.size();++n){auto& p=vertices[n];const double x=cx+(u[n]-.5)*ux*su+(v[n]-.5)*tx*sv,y=cy+(u[n]-.5)*uy*su+(v[n]-.5)*ty*sv;
        // Health/target color follows the original reticle unless PNG-only
        // colors were selected. Both modes retain each native alpha/fade.
        if(!current->native_color)for(unsigned channel=0;channel<2;++channel)for(unsigned color=0;color<3;++color)p.color[channel][color]=255.f;
        p.x=float(x*p.w);p.y=float(y*p.w);p.tex[coord][0]=float((image.left+u[n]*iw)/image.width);p.tex[coord][1]=float((image.top+v[n]*ih)/image.height);p.tex[coord][2]=1;
        new_left=std::min(new_left,x);new_right=std::max(new_right,x);new_top=std::max(new_top,y);new_bottom=std::min(new_bottom,y);
    }
    texture_map=map;texture_coord=coord;prepared=true;g_cfg_dirty=true;
    ++stats.replacements;stats.last_replaced=true;stats.texture_id=current->texture->id;
    stats.original_left=float(left);stats.original_top=float(top);stats.original_right=float(right);stats.original_bottom=float(bottom);
    stats.drawn_left=float(new_left);stats.drawn_top=float(new_top);stats.drawn_right=float(new_right);stats.drawn_bottom=float(new_bottom);return true;
}
}
}
