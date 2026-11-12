#include "../runtime/include/ssne/custom_crosshair.h"
#include "../runtime/src/gx/gx_crosshair.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace ssne::gx {
State g;Counters counters;
bool use_gpu=false;
DrawCfg fixture;
bool gpu_enabled(){return use_gpu;}
DrawCfg build_cfg(){auto cfg=fixture;custom_crosshair_apply_cfg(cfg);return cfg;}
}
namespace ssne {
std::vector<uint8_t> fifo;
void gx_fifo_write(uint32_t value,int bytes){for(int i=bytes-1;i>=0;--i)fifo.push_back(uint8_t(value>>(i*8)));}
}
using namespace ssne;using namespace ssne::gx;
void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
bool near(float a,float b){return std::abs(a-b)<1e-5f;}
void xf(unsigned n,float value){std::memcpy(&g.xf_reg[n],&value,4);}
uint64_t token(size_t offset){check(fifo.size()>=offset+10&&fifo[offset]==0xfe&&fifo[offset+1]==0x43,"private GX token framing");uint64_t result=0;for(unsigned i=0;i<8;++i)result=(result<<8)|fifo[offset+2+i];return result;}
uint64_t scope(bool on){const auto at=fifo.size();gx_custom_crosshair_scope(on);check(fifo.size()==at+10,"expected one ordered private scope packet");return token(at);}
std::shared_ptr<CustomCrosshairImage> image(uint8_t red=220){auto p=std::make_shared<CustomCrosshairImage>();p->width=p->height=8;p->left=2;p->top=5;p->right=4;p->bottom=7;p->rgba.resize(8*8*4);for(unsigned y=5;y<7;++y)for(unsigned x=2;x<4;++x){const auto n=(y*8+x)*4;p->rgba[n]=red;p->rgba[n+1]=40;p->rgba[n+2]=80;p->rgba[n+3]=200;}return p;}
std::vector<Vtx> quad(float cx=0,float cy=0){std::vector<Vtx> result(4);const float x[]={-.05f,.05f,.05f,-.05f},y[]={1.f/14,1.f/14,-1.f/14,-1.f/14},u[]={.1f,.2f,.2f,.1f},v[]={.4f,.4f,.5f,.5f};for(unsigned i=0;i<4;++i){auto& p=result[i];std::memset(&p,0,sizeof(p));p.x=cx+x[i];p.y=cy+y[i];p.z=-.5f;p.w=1;for(unsigned c=0;c<2;++c){p.color[c][0]=120;p.color[c][1]=80;p.color[c][2]=200;p.color[c][3]=160;}p.tex[0][0]=u[i];p.tex[0][1]=v[i];p.tex[0][2]=1;}return result;}
int main(){try{
    auto original=std::make_shared<Texture>();original->w=original->h=256;original->id=1;original->rgba.resize(256*256,~0u);
    fixture={};fixture.stages=1;fixture.st[0].tex_enabled=true;fixture.st[0].texmap=fixture.st[0].texcoord=0;fixture.ntex=1;fixture.tex[0]=original.get();fixture.su_s[0]=fixture.su_t[0]=256;fixture.st[0].cenv=0x88faf;fixture.st[0].aenv=0x8f2f0;fixture.write_mask=0xffffff;fixture.z_enable=false;fixture.blend=0x4ad;fixture.blend_kind=BLEND_ALPHA;
    g.xf_reg[0x26]=1;xf(0x1a,320);xf(0x1b,-224);const std::vector<uint8_t> guest_before(reinterpret_cast<const uint8_t*>(&g),reinterpret_cast<const uint8_t*>(&g)+sizeof(g));
    set_custom_crosshair({});check(!custom_crosshair_active(),"default config must be original");gx_custom_crosshair_scope(false);check(fifo.empty(),"default-off clear must not add FIFO bytes");auto cfg=fixture;custom_crosshair_apply_cfg(cfg);check(std::memcmp(&cfg,&fixture,sizeof(cfg))==0,"default-off material must remain byte-identical");
    CustomCrosshairConfig a;check(a.native_color,"native reticle health color must be the default");a.enabled=true;a.image=image();a.native_color=false;set_custom_crosshair(a);check(custom_crosshair_active(),"valid custom image should become ready");const auto first=scope(true);check(first!=0&&!custom_crosshair_stream_active(),"scope must wait for FIFO parser");
    auto b=a;b.image=image(12);b.native_color=true;set_custom_crosshair(b);custom_crosshair_marker(first);check(custom_crosshair_stream_active(),"ordered scope was not activated");
    auto v=quad();const auto saved=v;check(custom_crosshair_prepare(PRIM_QUADS,v),"valid original quad not replaced");cfg=fixture;custom_crosshair_apply_cfg(cfg);
    check(cfg.tex[0]!=original.get()&&(cfg.tex[0]->rgba[5*8+2]&255)==220,"queued scope picked latest UI image instead of ticket snapshot");
    check(cfg.st[0].cenv==fixture.st[0].cenv&&cfg.st[0].aenv==fixture.st[0].aenv&&cfg.write_mask==fixture.write_mask&&cfg.z_enable==fixture.z_enable,"replacement changed original tint/depth/write material");
    for(unsigned n=0;n<4;++n)check(near(v[n].x,saved[n].x)&&near(v[n].y,saved[n].y)&&v[n].z==saved[n].z&&v[n].w==saved[n].w&&v[n].color[0][3]==saved[n].color[0][3]&&v[n].color[1][3]==saved[n].color[1][3],"default32 changed original position/size/alpha/depth");
    for(const auto& point:v)for(unsigned channel=0;channel<2;++channel)for(unsigned component=0;component<3;++component)check(point.color[channel][component]==255.f,"custom PNG RGB must not inherit native green/yellow/black reticle tint");
    check(!custom_crosshair_stats().last_native_color,"queued PNG-only color choice was replaced by later UI health color");
    check(near(v[0].tex[0][0],.25f)&&near(v[0].tex[0][1],.625f)&&near(v[2].tex[0][0],.5f)&&near(v[2].tex[0][1],.875f),"trimmed UV bounds drift");
    check(cfg.su_s[0]==8&&cfg.su_t[0]==8&&cfg.wrap_s[0]==0&&cfg.linear[0]&&cfg.blend_kind==BLEND_ALPHA,"RGBA texture sampling/blending mismatch");custom_crosshair_finish_draw();cfg=fixture;custom_crosshair_apply_cfg(cfg);check(std::memcmp(&cfg,&fixture,sizeof(cfg))==0,"replacement material leaked beyond draw");custom_crosshair_marker(scope(false));custom_crosshair_software_flush();
    // The same image/size/opacity must still publish a changed color mode.
    set_custom_crosshair(a);auto tinted=a;tinted.native_color=true;set_custom_crosshair(tinted);const auto health_ticket=scope(true);set_custom_crosshair(a);custom_crosshair_marker(health_ticket);
    v=quad();const auto health_vertices=v;check(custom_crosshair_prepare(PRIM_QUADS,v),"native health-color quad replacement failed");
    for(size_t n=0;n<v.size();++n)check(std::memcmp(v[n].color,health_vertices[n].color,sizeof(v[n].color))==0,"health color mode must preserve both native RGBA channels exactly");
    check(custom_crosshair_stats().last_native_color,"queued health-color choice was replaced by later PNG-only UI setting");custom_crosshair_finish_draw();custom_crosshair_marker(scope(false));custom_crosshair_software_flush();
    a.size=14;a.opacity=50;set_custom_crosshair(a);custom_crosshair_marker(scope(true));v=quad(.4f,.2f);check(custom_crosshair_prepare(PRIM_QUADS,v),"scaled quad replacement failed");cfg=fixture;custom_crosshair_apply_cfg(cfg);check(near((v[0].x+v[2].x)*.5f,.4f)&&near((v[0].y+v[2].y)*.5f,.2f)&&near(v[1].x-v[0].x,.1f*14.f/32),"size must retain original view/weapon scale and center");check((cfg.tex[0]->rgba[5*8+2]>>24)==100&&v[0].color[0][3]==160,"opacity must multiply texture alpha while retaining original vertex fade");
    const std::weak_ptr<const Texture> kept=cfg.tex[0]->shared_from_this();custom_crosshair_finish_draw();set_custom_crosshair({});custom_crosshair_marker(scope(false));check(!kept.expired(),"software queued draw lost custom texture before flush");custom_crosshair_software_flush();check(kept.expired(),"software flush retained unused custom texture");
    a.opacity=100;a.size=32;set_custom_crosshair(a);custom_crosshair_marker(scope(true));v=quad();v[0].tex[0][0]=.15f;const auto malformed=v;check(!custom_crosshair_prepare(PRIM_QUADS,v)&&std::memcmp(v.data(),malformed.data(),v.size()*sizeof(Vtx))==0,"unsupported geometry must retain original vertices");cfg=fixture;custom_crosshair_apply_cfg(cfg);check(std::memcmp(&cfg,&fixture,sizeof(cfg))==0,"fallback geometry changed material");
    fixture.stages=2;v=quad();check(!custom_crosshair_prepare(PRIM_QUADS,v),"unexpected multi-stage material should fall back");fixture.stages=1;custom_crosshair_marker(scope(false));
    // A detached GPU frame uses the same shared texture ownership as frame.keep().
    use_gpu=true;custom_crosshair_marker(scope(true));v=quad();check(custom_crosshair_prepare(PRIM_QUADS,v),"GPU-shared CPU TL quad failed");cfg=fixture;custom_crosshair_apply_cfg(cfg);auto frame_keep=cfg.tex[0]->shared_from_this();const std::weak_ptr<const Texture> gpu_kept=frame_keep;custom_crosshair_finish_draw();set_custom_crosshair({});custom_crosshair_marker(scope(false));check(!gpu_kept.expired(),"GPU frame texture lifetime broken");frame_keep.reset();check(gpu_kept.expired(),"GPU frame texture leaked after release");use_gpu=false;
    // Save-state stream reset drops stale private tickets, not the local UI choice.
    set_custom_crosshair(a);const auto stale=scope(true);custom_crosshair_reset_stream();custom_crosshair_marker(stale);check(!custom_crosshair_stream_active()&&custom_crosshair_active(),"stale state ticket reactivated an unrelated draw");const auto newer=scope(true);check(newer>stale,"private ticket identities must not be reused after state load");custom_crosshair_marker(newer);set_custom_crosshair({});check(custom_crosshair_stream_active(),"UI disable raced an already-recorded draw");custom_crosshair_marker(scope(false));check(!custom_crosshair_stream_active(),"false scope did not clear after disable");
    auto invisible=a;auto empty=std::make_shared<CustomCrosshairImage>(*a.image);std::fill(empty->rgba.begin(),empty->rgba.end(),0);invisible.image=empty;set_custom_crosshair(invisible);check(!custom_crosshair_active(),"fully transparent runtime image must fall back");
    set_custom_crosshair(a);for(unsigned i=0;i<100;++i)gx_custom_crosshair_scope(true);check(custom_crosshair_stats().dropped>0,"pending config tickets must have a bounded fallback");custom_crosshair_reset_stream();set_custom_crosshair({});
    check(std::memcmp(guest_before.data(),&g,sizeof(g))==0,"custom crosshair mutated guest CP/XF/BP/TMEM state");const auto observed=custom_crosshair_stats();check(observed.replacements>=3&&observed.fallbacks>=2&&observed.rejected>=1,"renderer diagnostic counters missing actual outcomes");
    std::cout<<"Ordered GX ticket snapshots, original-quad scaling/UV/depth, PNG/native health RGB ticket snapshots and alpha/material isolation, software/GPU texture ownership, default-off bytes, bounded fallback and stale-state reset passed (native pipeline collaborators synthetic).\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
