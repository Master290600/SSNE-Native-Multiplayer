#include "native_menu_mouse.h"
#include "ssne/recomp_hooks.h"
#include "ssne/native_party.h"
#include "ssne/audio.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

namespace port { namespace {
struct Rect {float l=0,t=0,r=0,b=0;bool valid()const{return std::isfinite(l)&&std::isfinite(t)&&std::isfinite(r)&&std::isfinite(b)&&r>l&&b>t;}bool contains(float x,float y)const{return x>=l&&x<r&&y>=t&&y<b;}};
Rect intersect(Rect a,Rect b){return {std::max(a.l,b.l),std::max(a.t,b.t),std::min(a.r,b.r),std::min(a.b,b.b)};}
struct Widget {
    uint32_t gui=0,node=0,definition=0,root_node=0;
    uint16_t root=0,id=0,type=0;
    Rect box{},track{};int option=-1;unsigned event=22,previous_event=0,next_event=0;
    bool selectable=false,allowed=false,custom=false;
    float value=0,minimum=0,maximum=0;
    std::string control;
};
std::mutex mutex;
std::deque<NativeMenuPointerEvent> events;
NativeMenuMouseSnapshot published;
std::string diagnostics="{\"active\":false,\"surfaces\":[]}";
NativeMenuMouseServices services;
std::atomic<float> display_wide{1};
bool installed=false;PPCFunc footer_original=nullptr;
uint64_t surface_hash=0,geometry_hash=0,last_sequence=0,frame=0;
uint64_t stale_events=0,cancel_events=0,activations=0,misses=0,rejected=0,focus_sounds=0;
PPCFunc sound_original=nullptr;
bool pointer_sound=false;
uint64_t sound_sequence=0;
Widget pressed,right_pressed;bool pressed_valid=false,dragging=false,right_valid=false;
uint32_t hovered=0,hovered_gui=0;uint16_t hovered_id=0;float pointer_x=0,pointer_y=0;
std::vector<Widget> widgets;
uint32_t wheel_gui=0,wheel_node=0;std::string wheel_control;float wheel_x_remainder=0,wheel_y_remainder=0;
void clear_wheel(){wheel_gui=wheel_node=0;wheel_control.clear();wheel_x_remainder=wheel_y_remainder=0;}
bool readable(uint32_t a,unsigned n){return a>=0x80000000u&&n<=ssne::guest_ram_size()&&a<=ssne::guest_ram_end()-n;}
uint32_t word(uint32_t a){return readable(a,4)?mem_read32(a):0;}
float number(uint32_t a){return u2f(word(a));}
uint16_t half(uint32_t a){return readable(a,2)?mem_read16(a):0;}
uint32_t call(PPCContext& base,uint32_t fn,uint32_t a=0,uint32_t b=0,uint32_t d=0,uint32_t e=0,float f=std::numeric_limits<float>::quiet_NaN()){
    if(!ssne_find_func(fn)||!readable(base.r[1]-0x400,0x400))return 0;
    auto c=base;c.r[1]-=0x400;c.msr&=~0x8000u;mem_write32(c.r[1],base.r[1]);c.r[3]=a;c.r[4]=b;c.r[5]=d;c.r[6]=e;
    if(std::isfinite(f))c.ps0[1]=c.ps1[1]=f;
    ssne_call(c,fn);return c.r[3];
}
// The original GUI owns its navigation cue and SFX volume. Focus helpers
// alone are silent; the controller navigation path also requests cue 0x1C7.
void play_focus_sound(PPCContext& c) {
    struct Source {bool previous=pointer_sound;Source(){pointer_sound=true;}~Source(){pointer_sound=previous;}} source;
    ++focus_sounds;call(c,0x800f9b74,0x1c7);
}
// Optional observation of the actual original GUI sound and decoded SFX voice.
// Production playback is entirely the game's normal wrapper/mixer path.
void trace_gui_sound(PPCContext& c) {
    const auto sound=c.r[3];ssne_call_original(c,0x800f9b74,sound_original);
    const auto handle=c.r[3];const auto sequence=0x500000000ull+(++sound_sequence);
    uint32_t record=0,parameter_block=0;
    if(sound==0x1c7&&int32_t(handle)>=0&&(handle&63u)<48) {
        const auto candidate=0x802d69c8u+(handle&63u)*192u,voice=word(candidate);
        if(word(candidate+0xac)==handle&&readable(voice,0x148)) {
            const auto slot=word(voice+0x18);
            if(slot<64) {record=candidate;parameter_block=0x802c18a0u+slot*236u;
                ssne::audio_track_weapon_voice(parameter_block,record,handle,sequence);}
        }
    }
    std::ofstream out("native-menu-audio-events.jsonl",std::ios::binary|std::ios::app);
    out<<"{\"sequence\":"<<sequence<<",\"frame\":"<<frame
       <<",\"source\":\""<<(pointer_sound?"mouse":"native")<<"\",\"sound_id\":"<<sound
       <<",\"handle\":"<<int32_t(handle)<<",\"record\":"<<record
       <<",\"parameter_block\":"<<parameter_block<<"}\n";
}
bool permitted(const Widget& w,unsigned event){return !services.allowed||services.allowed(w.gui,w.node,w.root,w.id,event);}
bool alive(const Widget& w){return readable(w.node,0xac)&&word(w.node+0x1c)==w.definition&&word(w.node+0x20)==w.gui&&(w.custom||half(w.definition+2)==w.id)&&!(word(w.definition+0xc)&0x22u);}
uint64_t mix(uint64_t h,uint64_t n){return (h^n)*1099511628211ull;}
Rect canvas_rect(uint32_t params){
    const auto p=word(params),s=word(params+8);
    if(!readable(p,8)||!readable(s,8))return {};
    const auto x=number(p),y=number(p+4),w=number(s),h=number(s+4);
    return {x,y,x+w,y+h};
}
// Read the matrix authored by original 8014CEA4, rather than inferring a
// multiplier from ScreenMode. Its native wide projection has half-width
// 426.6667; the standard projection has half-width 320. Runtime presentation
// subsequently centers the resulting NDC in the displayed wide image.
struct Projection {
    float half_width=320,half_height=240,xx=1.f/320,xy=0,xo=0,yx=0,yy=-1.f/240,yo=0;
    bool matrix=false;
} projection;
void read_projection(){
    projection={};const auto width=number(0x802dc690u),height=number(0x802dc694u);
    if(std::isfinite(width)&&width>=32&&width<=4096)projection.half_width=width;
    if(std::isfinite(height)&&height>=32&&height<=4096)projection.half_height=height;
    projection.xx=1/projection.half_width;projection.yy=-1/projection.half_height;
    constexpr uint32_t matrix=0x80292394u;
    const float xx=number(matrix),xy=number(matrix+4),xo=number(matrix+12),yx=number(matrix+16),yy=number(matrix+20),yo=number(matrix+28);
    if(std::isfinite(xx)&&std::isfinite(xy)&&std::isfinite(xo)&&std::isfinite(yx)&&std::isfinite(yy)&&std::isfinite(yo)&&std::abs(xx)>1e-6f&&std::abs(xx)<1&&std::abs(yy)>1e-6f&&std::abs(yy)<1&&std::abs(xy)<1&&std::abs(yx)<1&&std::abs(xo)<4&&std::abs(yo)<4){
        projection.xx=xx;projection.xy=xy;projection.xo=xo;projection.yx=yx;projection.yy=yy;projection.yo=yo;projection.matrix=true;
    }
}
struct Point {float x=0,y=0;};
Point anchor_point(Rect view,unsigned mode,Point offset,float scale){
    Point origin;const auto cx=(view.l+view.r)*.5f,cy=(view.t+view.b)*.5f;
    switch(mode){
    case 0:origin={view.l,view.t};break;case 1:origin={view.r,view.t};break;
    case 2:origin={view.l,view.b};break;case 3:origin={view.r,view.b};break;
    case 4:origin={cx,cy};break;case 5:origin={cx,view.t};break;
    case 6:origin={view.l,cy};break;case 7:origin={view.r,cy};break;
    case 8:origin={cx,view.b};break;
    case 9:case 10:{
        // The native safe-frame anchors use the original renderer constant,
        // independently of the selected screen or presentation aspect.
        const auto aspect=number(0x8021ca54u);
        if(!std::isfinite(aspect)||aspect<=0||aspect>4)return {std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::quiet_NaN()};
        const auto inset=((view.b-view.t)-(view.r-view.l)*aspect)*.5f;
        origin=mode==9?Point{view.l,view.t+inset}:Point{view.r,view.b-inset};break;
    }
    default:return {std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::quiet_NaN()};
    }
    return {std::fma(offset.x,scale,origin.x),std::fma(offset.y,scale,origin.y)};
}
Rect project(Rect world){
    if(!world.valid())return {};
    Rect out{1e30f,1e30f,-1e30f,-1e30f};
    for(const auto& p:{Point{world.l,world.t},Point{world.r,world.t},Point{world.l,world.b},Point{world.r,world.b}}){
        const float x=320.f*(projection.xx*p.x+projection.xy*p.y+projection.xo);
        const float y=-240.f*(projection.yx*p.x+projection.yy*p.y+projection.yo);
        out.l=std::min(out.l,x);out.r=std::max(out.r,x);out.t=std::min(out.t,y);out.b=std::max(out.b,y);
    }
    return out;
}
struct RenderView {Rect rect{},clip{-8192,-8192,8192,8192};float scale=1;bool valid=true;};
RenderView renderer_view(uint32_t renderer){
    RenderView view;view.rect={-projection.half_width,-projection.half_height,projection.half_width,projection.half_height};
    std::vector<uint32_t> chain;std::set<uint32_t> seen;
    while(renderer){
        if(!readable(renderer,0x44)||word(renderer)!=0x8021cab0u||chain.size()>=32||!seen.insert(renderer).second){view.valid=false;return view;}
        chain.push_back(renderer);renderer=word(renderer+0x14);
    }
    // 8011D2A8 stores the real parent in +14. Original 8011E5AC computes each
    // group's corners with the incoming scale, then applies its local scale
    // (+30) to the contents. This includes clipped/scrolling native lists.
    for(auto it=chain.rbegin();it!=chain.rend();++it){
        const auto group=*it;const auto flags=half(group+0x1e);const auto local_scale=number(group+0x30);
        if((flags&1)||!std::isfinite(local_scale)||local_scale<=0||local_scale>32){view.valid=false;return view;}
        const auto tl=anchor_point(view.rect,mem_read8(group+0x1c),{number(group+0x20),number(group+0x24)},view.scale);
        const auto br=anchor_point(view.rect,mem_read8(group+0x1d),{number(group+0x28),number(group+0x2c)},view.scale);
        view.rect={tl.x,tl.y,br.x,br.y};if(!view.rect.valid()){view.valid=false;return view;}
        if(flags&2){auto clip=project(view.rect);
            // The existing GPU path retains full-width scissors across the
            // complete wide output; partial native clips stay centered.
            if(clip.l<=-319.5f&&clip.r>=319.5f){const auto wide=display_wide.load(std::memory_order_relaxed);clip.l=-320*wide;clip.r=320*wide;}
            view.clip=intersect(view.clip,clip);
        }
        view.scale*=local_scale;
    }
    return view;
}
Rect local_rect(Rect local,unsigned anchor,const RenderView& view){
    if(!local.valid()||!view.valid)return {};
    const auto tl=anchor_point(view.rect,anchor,{local.l,local.t},view.scale),br=anchor_point(view.rect,anchor,{local.r,local.b},view.scale);
    return project({tl.x,tl.y,br.x,br.y});
}
// CcText retains the final local baseline. Native font metrics already include
// its authored font scale; renderer ancestry supplies viewport and animation
// scale, and the actual original orthographic matrix supplies NDC.
Rect text_rect(uint32_t helper,Rect* clip=nullptr){
    if(!readable(helper,0x94))return {};
    const auto text=word(helper+0x48);if(!readable(text,0x6c)||word(text)!=0x8021cb58u)return {};
    const auto view=renderer_view(word(text+0x10));if(!view.valid)return {};
    if(clip)*clip=intersect(*clip,view.clip);
    const auto x=number(text+0x14),baseline=number(text+0x18),width=number(helper+0x14),height=number(helper+0x18),align=number(helper+0x20);
    if(!(width>0&&width<4096&&height>0&&height<1024))return {};
    return local_rect({x+align,baseline-height,x+align+width,baseline},word(helper+0x10),view);
}
Rect glyph_rect(uint32_t object,Rect* clip=nullptr){
    if(!readable(object,0x48))return {};
    const auto view=renderer_view(word(object+0x10));if(!view.valid)return {};
    if(clip)*clip=intersect(*clip,view.clip);
    return local_rect({number(object+0x14),number(object+0x18),number(object+0x1c),number(object+0x20)},word(object+0x44),view);
}
unsigned step_event(const Widget&,int);
void add(Widget w,Rect box,Rect clip){
    w.track=box;w.box=intersect(box,clip);if(!w.box.valid())return;
    w.allowed=permitted(w,w.event);widgets.push_back(std::move(w));
}
void add_text(Widget w,uint32_t helper,Rect clip){const auto box=text_rect(helper,&clip);add(std::move(w),box,clip);}
void text_arrows(const Widget& original,uint32_t helper,Rect clip){
    if(!readable(helper,0x94))return;
    for(unsigned i=0;i<2;++i){auto w=original;w.option=-1;w.control=i?"adjust.next":"adjust.previous";w.event=step_event(w,i?1:-1);auto arrow_clip=clip;const auto box=glyph_rect(word(helper+0x4c+28*i),&arrow_clip);add(w,box,arrow_clip);}
}
void visit(PPCContext& c,uint32_t gui,uint32_t node,uint32_t root,Rect clip,std::set<uint32_t>& seen,unsigned depth){
    if(depth>32||seen.size()>=4096||!readable(node,0xac)||!seen.insert(node).second)return;
    const auto d=word(node+0x1c),params=word(node+0x14);
    if(!readable(d,0x44)||word(node+0x20)!=gui)return;
    const auto flags=word(d+0xc);if(flags&0x22u)return;

    if(!clip.valid())return;
    Widget w;w.gui=gui;w.node=node;w.definition=d;w.root_node=root;w.root=half(word(root+0x1c)+2);w.id=half(d+2);w.type=half(d);
    if(w.type==1||w.type==2||w.type==3||w.type==5)w.selectable=call(c,0x800fac6c,node)!=0;
    if(w.type==1)add_text(w,word(node+0xb8),clip);
    if(w.type==2){w.value=number(node+0xb8);w.minimum=number(node+0xbc);w.maximum=number(node+0xc0);const auto helper=word(node+0xc8);add_text(w,helper,clip);text_arrows(w,helper,clip);}
    if(w.type==3){
        w.value=float(int32_t(word(node+0xd0)));w.minimum=0;w.maximum=float(int32_t(word(node+0xd4))-1);
        auto option=word(node+0xc0);std::set<uint32_t> options;int index=0;
        while(readable(option,0x20)&&options.size()<256&&options.insert(option).second){
            auto o=w;const auto helper=word(option+0x14);const bool list=(flags&0x2000u)==0;o.option=list?index:-1;if(!list)o.control="selector.value";++index;add_text(o,helper,clip);text_arrows(w,helper,clip);option=word(option+8);
        }
    }
    if(w.type==5){const auto range=word(node+0xb0);if(readable(range,12)){w.value=number(range);w.minimum=number(range+4);w.maximum=number(range+8);auto track_clip=clip;const auto background=word(word(node+0xc4)+8);auto box=glyph_rect(background,&track_clip);if(!box.valid()){const auto view=renderer_view(word(node+0x2c));box=local_rect(canvas_rect(params),word(node+0x88),view);track_clip=intersect(track_clip,view.clip);}add(w,box,track_clip);}}
    if(w.type==7&&services.model_region&&services.model_region(gui,node,w.root,w.id)){
        const auto draw=word(node+0xbc),view=word(draw+8);
        if(readable(draw,12)&&view<16){
            const auto previous_owner=ssne::native_party_owner();const int owner=services.model_owner?services.model_owner(gui,node,w.root,w.id):-1;
            if(owner>=0&&owner<8)ssne::native_party_owner_set(owner);
            const auto engine=ssne::native_party_view_engine(word(0x802abaa0u));const auto slot=ssne::native_party_view_slot(view);
            ssne::native_party_owner_set(previous_owner);const auto base=engine+76u*slot;
            if(readable(base+0xe0,0x18)&&word(base+0xf0)==4&&word(base+0xf4)==4){
                Rect box=project({number(base+0xe0),number(base+0xe4),number(base+0xe8),number(base+0xec)});
                if(box.valid())for(unsigned side=0;side<2;++side){auto model=w;model.custom=model.selectable=true;model.event=18+side;model.control=side?"character.next":"character.previous";const float middle=(box.l+box.r)*.5f;add(model,side?Rect{middle,box.t,box.r,box.b}:Rect{box.l,box.t,middle,box.b},clip);}
            }
        }
    }
    auto child=word(node+0x34);std::set<uint32_t> siblings;
    while(child&&siblings.size()<1024&&siblings.insert(child).second){visit(c,gui,child,root,clip,seen,depth+1);child=word(child+8);}
}
void collect(PPCContext& c){
    read_projection();widgets.clear();bool has_surface=false;uint64_t surfaces=14695981039346656037ull;if(services.surface_epoch)surfaces=mix(surfaces,services.surface_epoch());
    auto gui=word(0x8028bfa4u);std::set<uint32_t> guis;
    while(readable(gui,0xa8)&&guis.size()<32&&guis.insert(gui).second){
        auto root=word(gui+0x7c);std::set<uint32_t> parents;
        while(root&&word(root+0x24)&&parents.size()<32&&parents.insert(root).second)root=word(root+0x24);
        if(!root)root=word(gui+0x6c);
        bool linked=false;auto active=word(gui+0x6c);std::set<uint32_t> roots;
        while(active&&roots.size()<64&&roots.insert(active).second){if(active==root)linked=true;active=word(active+8);}
        if(linked){
            const auto d=word(root+0x1c);if(readable(d,0x44)&&!(word(d+0xc)&0x20u))has_surface=true;
            if(readable(d,0x44)&&!(word(d+0xc)&0x22u)){
                surfaces=mix(mix(mix(surfaces,gui),root),half(d+2));std::set<uint32_t> seen;
                visit(c,gui,root,root,{-8192,-8192,8192,8192},seen,0);
                // These are the original GUI's six retained button prompts,
                // updated by 800F86FC from the focused definition's text IDs.
                // Results Continue and carousel category have no child widget.
                const auto focused=half(d+2)==0xbc?root:word(gui+0x7c),focused_definition=word(focused+0x1c),prompts=word(gui+0xc4);
                static constexpr const char* names[]={"footer.confirm","footer.back","footer.x","footer.y","footer.z","footer.start"};
                // Original 80239054 prompt order is A/B/X/Y/Z/Start. The
                // 80218408 input records place Start at event24 before X25.
                static constexpr unsigned prompt_events[]={22,23,25,26,27,24};
                if(readable(focused,0xac)&&readable(focused_definition,0x28)&&readable(prompts,48))for(unsigned i=0;i<6;++i){
                    if(!word(focused_definition+0x10+4*i))continue;
                    const auto helper=word(prompts+8*i),parameters=word(prompts+8*i+4),fade=word(parameters+0x18);
                    if(!readable(fade,4)||number(fade)<=.01f)continue;
                    Rect box=text_rect(helper);if(!box.valid())continue;
                    for(unsigned side=0;side<2;++side){const auto icon=word(helper+0x4c+28*side);if(readable(icon,0x24)){
                        Rect glyph=glyph_rect(icon);
                        if(glyph.valid())box={std::min(box.l,glyph.l),std::min(box.t,glyph.t),std::max(box.r,glyph.r),std::max(box.b,glyph.b)};
                    }}
                    Widget footer;footer.gui=gui;footer.node=focused;footer.definition=focused_definition;footer.root_node=root;footer.root=half(d+2);footer.id=half(focused_definition+2);footer.type=0;footer.event=prompt_events[i];footer.control=names[i];footer.selectable=true;
                    add(footer,box,{-8192,-8192,8192,8192});
                }
            }
        }
        gui=word(gui+8);
    }
    if(services.regions)for(const auto& r:services.regions(c)){
        if(!readable(r.node,0xac)||!readable(r.gui,0xa8))continue;
        Widget w;w.gui=r.gui;w.node=r.node;w.definition=word(r.node+0x1c);w.root=r.root;w.id=r.id;w.type=half(w.definition);w.event=r.event;w.previous_event=r.previous_event;w.next_event=r.next_event;w.custom=true;w.selectable=true;w.control=r.control;
        auto clip=Rect{-8192,-8192,8192,8192};Rect box;
        if(r.primitive)box=glyph_rect(r.primitive,&clip);
        else if(r.viewport_node){
            if(!readable(r.viewport_node,0x8c)||word(r.viewport_node+0x20)!=r.gui||!readable(word(r.viewport_node+0x1c),0x10)||(word(word(r.viewport_node+0x1c)+0xc)&0x22u))continue;
            const auto view=renderer_view(word(r.viewport_node+0x2c));clip=intersect(clip,view.clip);
            box=local_rect(canvas_rect(word(r.viewport_node+0x14)),word(r.viewport_node+0x88),view);
        }else box=project({r.left,r.top,r.right,r.bottom});
        add(w,box,clip);
    }
    uint64_t geometry=surfaces;for(float value:{projection.half_width,projection.half_height,projection.xx,projection.xy,projection.xo,projection.yx,projection.yy,projection.yo})geometry=mix(geometry,f2u(value));bool active=false;
    for(const auto& w:widgets){
        // Native footer prompts borrow the currently focused node. Hovering a
        // different row must not turn that focus proxy into a layout transition
        // and cancel a mouse press already captured on the live row.
        if(!w.control.starts_with("footer.")){geometry=mix(mix(mix(mix(geometry,w.node),w.definition),unsigned(w.option+1)),w.allowed&&w.selectable);geometry=mix(mix(mix(geometry,w.event),w.previous_event),w.next_event);for(unsigned char ch:w.control)geometry=mix(geometry,ch);}
        active|=w.allowed&&w.selectable;
    }
    std::lock_guard lock(mutex);
    if(surface_hash!=surfaces){surface_hash=surfaces;++published.surface_generation;pressed_valid=dragging=right_valid=false;hovered=0;clear_wheel();}
    if(geometry_hash!=geometry){geometry_hash=geometry;++published.geometry_generation;pressed_valid=dragging=right_valid=false;}
    published.active=active;published.has_surface=has_surface;
}
bool bound_event(const Widget& w,uint32_t node,unsigned event){
    const auto definition=word(node+0x1c);const auto id=half(definition+2);
    auto binding=word(w.gui+0x94);std::set<uint32_t> callbacks;
    while(readable(binding,0x20)&&callbacks.size()<4096&&callbacks.insert(binding).second){if(half(binding+0x18)==id&&word(binding+0x14)==event)return true;binding=word(binding+8);}
    const auto count=half(definition+8);const auto actions=word(definition+0x48);
    if(count<256&&readable(actions,unsigned(count)*12))for(unsigned i=0;i<count;++i)if(word(actions+12*i)==event)return true;
    return false;
}
bool back_available(const Widget& w){
    auto node=w.node;std::set<uint32_t> parents;
    while(readable(node,0xac)&&parents.size()<32&&parents.insert(node).second){
        const auto definition=word(node+0x1c);
        if(word(definition+0x14)||bound_event(w,node,23))return true;
        node=word(node+0x24);
    }
    return false;
}
Widget* hit(float x,float y){
    Widget* footer=nullptr;
    for(auto it=widgets.rbegin();it!=widgets.rend();++it)if(it->selectable&&it->allowed&&it->box.contains(x,y)){
        // Prompts borrow the focused control's identity and animate separately.
        // Keep its real body control stable under Down/Up when they overlap.
        if(!it->control.starts_with("footer."))return &*it;
        if(!footer)footer=&*it;
    }
    return footer;
}
bool dispatch(PPCContext& c,const Widget& w,unsigned event){
    if(!event)return false;
    if(!alive(w)||!permitted(w,event)||(!w.custom&&w.type!=0&&!call(c,0x800fac6c,w.node))){++rejected;return false;}
    if(services.intercept&&services.intercept(c,w.gui,w.node,w.root,w.id,event)){++activations;return true;}
    if(w.custom)return false;
    call(c,0x800fb130,w.node,event);++activations;return true;
}
void focus(PPCContext& c,const Widget& w){
    if(alive(w)&&permitted(w,22)&&(w.custom||w.type==0||call(c,0x800fac6c,w.node))&&word(w.gui+0x7c)!=w.node){
        const auto previous=word(w.gui+0x7c);
        if(w.root==0xbc&&w.node==w.root_node&&w.control=="footer.confirm"){call(c,0x800f6b6c,w.gui);call(c,0x800f6a94,w.gui,w.node);}
        else call(c,0x800fa5bc,w.node);
        const auto selected=word(w.gui+0x7c);
        if(w.event&&selected&&selected!=previous)play_focus_sound(c);
    }
    hovered=w.node;hovered_id=w.id;hovered_gui=w.gui;
}
unsigned step_event(const Widget& w,int direction){const auto flags=word(w.definition+0xc);if(w.type==2)return (flags&0x4000u?30:28)+(direction>0);if(w.type==5)return (flags&0x2000u?18:20)+(direction>0);return (flags&0x2000u?20:18)+(direction>0);}
bool set_slider(PPCContext& c,const Widget& w,float x,float y){
    if(!alive(w)||!permitted(w,14))return false;
    const auto range=word(w.node+0xb0);if(!readable(range,12))return false;
    const bool horizontal=(word(w.definition+0xc)&0x2000u)==0;
    const float fraction=std::clamp(horizontal?(x-w.track.l)/(w.track.r-w.track.l):(y-w.track.t)/(w.track.b-w.track.t),0.f,1.f);
    const auto low=number(range+4),high=number(range+8);if(!std::isfinite(low)||!std::isfinite(high)||high<low)return false;
    if(services.intercept&&services.intercept(c,w.gui,w.node,w.root,w.id,14))return true;
    call(c,0x800f2440,w.node,0,0,0,low+fraction*(high-low));
    const auto linked=half(w.node+0xac);if(linked){const auto peer=call(c,0x800f6030,w.gui,linked),vt=word(peer+0x5c);if(readable(vt,0x40))call(c,word(vt+0x3c),peer+int16_t(half(vt+0x38)),word(w.gui+0xa4));}
    call(c,0x800f6e3c,w.gui,w.id,14);++activations;return true;
}
void activate(PPCContext& c,const Widget& w){
    if(!w.event)return;
    if(!w.custom&&w.event!=22){dispatch(c,w,w.event);return;}
    if(w.type==3&&w.option<0&&!w.custom){dispatch(c,w,bound_event(w,w.node,22)?22:step_event(w,1));return;}
    if(w.type==5&&!w.custom){set_slider(c,w,pointer_x,pointer_y);return;}
    if(w.type==3&&w.option>=0&&!w.custom){
        const auto current=int32_t(word(w.node+0xd0));const int difference=w.option-current;
        if(std::abs(difference)>256)return;
        if(difference){for(int n=0;n<std::abs(difference);++n)if(!dispatch(c,w,step_event(w,difference>0?1:-1)))break;}
        else dispatch(c,w,22);
        return;
    }
    if(w.type==2&&!w.custom){dispatch(c,w,step_event(w,1));return;}
    dispatch(c,w,w.event);
}
bool scroll(PPCContext& c,const Widget& w,const NativeMenuPointerEvent& e){
    if(wheel_gui!=w.gui||wheel_node!=w.node||wheel_control!=w.control){clear_wheel();wheel_gui=w.gui;wheel_node=w.node;wheel_control=w.control;}
    const bool horizontal=e.wheel_x!=0;auto& remainder=horizontal?wheel_x_remainder:wheel_y_remainder;
    remainder+=std::clamp(horizontal?e.wheel_x:e.wheel_y,-32.f,32.f);
    const int steps=std::clamp(int(std::trunc(remainder)),-4,4);remainder-=float(steps);if(std::abs(remainder)>4)remainder=0;
    if(!steps)return false;
    int direction=steps>0?1:-1;if(!horizontal&&w.type!=2&&w.type!=5)direction=-direction;
    unsigned event=step_event(w,direction);
    if(w.custom&&(w.previous_event||w.next_event))event=direction<0?w.previous_event:w.next_event;
    else if(w.custom&&w.control.starts_with("carousel."))event=20+(direction>0);
    else if(w.type==1&&!w.custom)event=(word(word(w.root_node+0x1c)+0xc)&0x200u?20u:18u)+(direction>0);
    if(!event)return false;
    for(int n=0;n<std::abs(steps);++n){
        // A wheel notch is a discrete adjustment, not a held controller key.
        // Reset native key-repeat acceleration through the class's own handler.
        if(!w.custom&&w.type==2)call(c,0x800f0504,w.node,15);
        if(!w.custom&&w.type==5)call(c,0x800f2284,w.node,15);
        if(!dispatch(c,w,event))break;
    }
    return true;
}
// A local Connection child on BC may take native focus for highlighting, but
// the existing Results Continue prompt still belongs to BC. Preserve the native
// prompt renderer and its text/icon resources; only its descriptor input changes.
void footer_update(PPCContext& c){
    const auto gui=c.r[3];auto root=word(gui+0x7c);std::set<uint32_t> seen;
    while(root&&word(root+0x24)&&seen.size()<32&&seen.insert(root).second)root=word(root+0x24);
    const auto definition=word(root+0x1c);
    if(readable(definition,0x28)&&half(definition+2)==0xbc&&!(word(definition+0xc)&0x22u))c.r[4]=definition+0x10;
    ssne_call_original(c,0x800f86fcu,footer_original);
}
void describe(){
    const float wide=display_wide.load(std::memory_order_relaxed);auto normx=[&](float x){return .5f+x/(640.f*wide);};
    NativeMenuMouseSnapshot snapshot;{std::lock_guard lock(mutex);snapshot=published;}
    std::ostringstream out;out<<std::setprecision(8)<<"{\"active\":"<<(snapshot.active?"true":"false")<<",\"has_surface\":"<<(snapshot.has_surface?"true":"false")<<",\"surface_generation\":"<<snapshot.surface_generation<<",\"geometry_generation\":"<<snapshot.geometry_generation<<",\"frame\":"<<frame<<",\"projection_source\":\""<<(projection.matrix?"native_ortho_matrix":"native_half_extent_fallback")<<"\",\"native_half_extent\":["<<projection.half_width<<','<<projection.half_height<<"],\"projection\":["<<projection.xx<<','<<projection.xy<<','<<projection.xo<<','<<projection.yx<<','<<projection.yy<<','<<projection.yo<<"],\"wide\":"<<wide<<",\"last_sequence\":"<<last_sequence<<",\"hover\":{\"node\":"<<hovered<<",\"gui\":"<<hovered_gui<<",\"id\":"<<hovered_id<<"},\"dragging\":"<<(dragging?"true":"false")<<",\"stale_events\":"<<stale_events<<",\"cancel_events\":"<<cancel_events<<",\"activations\":"<<activations<<",\"misses\":"<<misses<<",\"focus_sounds\":"<<focus_sounds<<",\"rejected\":"<<rejected<<",\"surfaces\":[";
    uint32_t previous=0;uint16_t previous_root=0;bool first=true,first_widget=true;
    for(const auto& w:widgets){if(w.gui!=previous||w.root!=previous_root){if(!first)out<<"]}";if(!first)out<<',';first=false;first_widget=true;previous=w.gui;previous_root=w.root;out<<"{\"gui\":"<<w.gui<<",\"root\":"<<w.root<<",\"port\":"<<int16_t(half(w.gui+0x84))<<",\"widgets\":[";}
        if(!first_widget)out<<',';first_widget=false;
        out<<"{\"node\":"<<w.node<<",\"id\":"<<w.id<<",\"type\":"<<w.type<<",\"option\":"<<w.option<<",\"selectable\":"<<(w.selectable?"true":"false")<<",\"allowed\":"<<(w.allowed?"true":"false")<<",\"value\":"<<w.value<<",\"min\":"<<w.minimum<<",\"max\":"<<w.maximum<<",\"event\":"<<w.event<<",\"control\":\""<<w.control<<"\",\"axis\":\""<<(w.type==5?(word(w.definition+0xc)&0x2000u?"vertical":"horizontal"):"")<<"\",\"track_bounds\":["<<normx(w.track.l)<<','<<.5f+w.track.t/480.f<<','<<normx(w.track.r)<<','<<.5f+w.track.b/480.f<<"],\"bounds\":["<<normx(w.box.l)<<','<<.5f+w.box.t/480.f<<','<<normx(w.box.r)<<','<<.5f+w.box.b/480.f<<"]}";
    }
    if(!first)out<<"]}";out<<"]}";const auto state=out.str();{std::lock_guard lock(mutex);diagnostics=state;}
    if(std::getenv("SSNE_TEST_HIDDEN")&&(std::getenv("SSNE_DEV_NATIVE_MENU")||std::getenv("SSNE_NATIVE_MOUSE_COMMAND"))){
        std::ofstream file("native-menu-mouse-state.json.pending",std::ios::binary);file<<state;file.close();
        if(file){std::remove("native-menu-mouse-state.json");std::rename("native-menu-mouse-state.json.pending","native-menu-mouse-state.json");}
    }
}
}
void configure_native_menu_mouse(NativeMenuMouseServices s){services=std::move(s);}
void install_native_menu_mouse(){
    installed=word(0x80000000u)==0x47334250u&&ssne_find_func(0x800fb130)&&ssne_find_func(0x800fa5bc)&&ssne_find_func(0x800fac6c);
    if(!installed||footer_original)return;
    const auto original=ssne_find_func(0x800f86fcu);if(!original)return;
    uint64_t hash=14695981039346656037ull;for(unsigned i=0;i<0x200;++i)hash=mix(hash,mem_read8(0x800f86fcu+i));
    if(hash!=0xdfc4b05b8745fe09ull)return;
    footer_original=*ssne_hook_slot(0x800f86fcu);if(!footer_original)footer_original=original->func;
    ssne_install_hook(0x800f86fcu,footer_update);
    if(std::getenv("SSNE_TEST_HIDDEN")&&std::getenv("SSNE_TEST_MENU_HOVER_AUDIO")) {
        const auto sound=ssne_find_func(0x800f9b74);
        if(sound) {sound_original=*ssne_hook_slot(0x800f9b74);if(!sound_original)sound_original=sound->func;
            ssne_install_hook(0x800f9b74,trace_gui_sound);}
    }
}
void set_native_menu_mouse_display(float wide){if(std::isfinite(wide)&&wide>=1&&wide<=8)display_wide.store(wide,std::memory_order_relaxed);}
NativeMenuMouseSnapshot native_menu_mouse_snapshot(){std::lock_guard lock(mutex);return published;}
bool menu_mouse_wants_pointer(){return native_menu_mouse_snapshot().active;}
uint64_t menu_mouse_surface_generation(){return native_menu_mouse_snapshot().surface_generation;}
std::string native_menu_mouse_diagnostics(){std::lock_guard lock(mutex);return diagnostics;}
void queue_native_menu_pointer(const NativeMenuPointerEvent& event){
    if(!std::isfinite(event.u)||!std::isfinite(event.v)||!std::isfinite(event.wheel_x)||!std::isfinite(event.wheel_y)||!std::isfinite(event.display_wide)||event.display_wide<1||event.display_wide>8)return;
    std::lock_guard lock(mutex);
    if(event.kind==NativeMenuPointerEvent::Kind::Cancel)events.clear();
    if(event.kind==NativeMenuPointerEvent::Kind::Move&&!events.empty()&&events.back().kind==event.kind&&events.back().surface_generation==event.surface_generation&&events.back().geometry_generation==event.geometry_generation)events.back()=event;
    else {if(events.size()>=256){events.clear();NativeMenuPointerEvent cancel;cancel.kind=NativeMenuPointerEvent::Kind::Cancel;events.push_back(cancel);}events.push_back(event);}
}
void consume_native_menu_pointer(PPCContext& c){
    if(!installed)return;++frame;collect(c);
    for(unsigned count=0;count<64;++count){
        NativeMenuPointerEvent e;NativeMenuMouseSnapshot snapshot;{std::lock_guard lock(mutex);if(events.empty())break;e=events.front();events.pop_front();snapshot=published;}
        if(e.kind==NativeMenuPointerEvent::Kind::Cancel){pressed_valid=dragging=right_valid=false;hovered=0;clear_wheel();++cancel_events;continue;}
        if(e.sequence<=last_sequence){++stale_events;continue;}last_sequence=e.sequence;
        if(!snapshot.active||e.surface_generation!=snapshot.surface_generation||e.geometry_generation!=snapshot.geometry_generation){pressed_valid=dragging=right_valid=false;++stale_events;continue;}
        const float wide=e.display_wide;pointer_x=(e.u-.5f)*640.f*wide;pointer_y=(e.v-.5f)*480.f;
        auto target=e.inside?hit(pointer_x,pointer_y):nullptr;
        if(e.kind==NativeMenuPointerEvent::Kind::Move){
            if(dragging&&pressed_valid){set_slider(c,pressed,pointer_x,pointer_y);break;}
            if(target&&target->event)focus(c,*target);else {hovered=0;hovered_id=0;}
        }else if(e.kind==NativeMenuPointerEvent::Kind::Down){
            if(e.button==3){pressed_valid=dragging=right_valid=false;if(e.inside){
                // Back belongs to the current native focus, independent of the
                // pointer's position over text or blank original menu artwork.
                for(auto it=widgets.rbegin();it!=widgets.rend();++it)if(it->selectable&&it->allowed&&word(it->gui+0x7c)==it->node&&permitted(*it,23)&&back_available(*it)){right_pressed=*it;right_valid=true;break;}
                if(!right_valid)for(auto it=widgets.rbegin();it!=widgets.rend();++it)if(it->selectable&&it->allowed&&permitted(*it,23)&&back_available(*it)){right_pressed=*it;right_valid=true;break;}
            }continue;}
            if(e.button!=1)continue;
            if(!target||!target->event){pressed_valid=dragging=right_valid=false;++misses;continue;}
            pressed=*target;pressed_valid=true;dragging=target->type==5&&!target->custom;focus(c,*target);
            if(dragging){set_slider(c,pressed,pointer_x,pointer_y);break;}
        }else if(e.kind==NativeMenuPointerEvent::Kind::Up&&e.button==3){
            const auto saved=right_pressed;const bool back=right_valid&&e.inside&&word(saved.gui+0x7c)==saved.node&&back_available(saved);right_valid=false;
            if(back){dispatch(c,saved,23);break;}
        }else if(e.kind==NativeMenuPointerEvent::Kind::Up&&e.button==1){
            const bool same=pressed_valid&&target&&target->node==pressed.node&&target->option==pressed.option&&target->control==pressed.control;
            const auto saved=pressed;const bool was_dragging=dragging;pressed_valid=dragging=right_valid=false;
            if(was_dragging){set_slider(c,saved,pointer_x,pointer_y);break;}
            if(same){activate(c,saved);break;}
        }else if(e.kind==NativeMenuPointerEvent::Kind::Wheel&&target){
            focus(c,*target);if(scroll(c,*target,e))break;
        }
    }
    describe();
}
}
