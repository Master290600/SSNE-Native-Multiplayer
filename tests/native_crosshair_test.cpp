// Exercise the actual guest-hook functions with synthetic HUD/renderer memory.
// Original game bodies/signatures are never embedded in this fixture.
#include "../port/src/native_crosshair.cpp"
#include <array>
#include <iostream>
#include <map>
#include <stdexcept>

namespace ssne {
std::array<uint8_t,kRamSize> ram{};uint8_t* g_ram=ram.data();uint8_t g_locked_cache[kLockedCacheSize]{};
uint32_t guest_ram_size(){return kRamSize;}uint32_t guest_ram_end(){return 0x80000000u+kRamSize;}
uint32_t mmio_read(uint32_t,int){throw std::runtime_error("unexpected MMIO");}void mmio_write(uint32_t,uint32_t,int){throw std::runtime_error("unexpected MMIO");}
uint32_t native_party_registry(){return 0x81000000u;}unsigned native_party_capacity(){return 4;}
bool crosshair_on=false;std::vector<bool> markers;
NativeHudListObserver hud_list_observer=nullptr;
void set_native_hud_list_observer(NativeHudListObserver callback){hud_list_observer=callback;}
bool custom_crosshair_active(){return crosshair_on;}
void gx_custom_crosshair_scope(bool on){markers.push_back(on);}
}
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::map<uint32_t,PPCFunc> hooks;
unsigned update_calls=0,material_calls=0,group_calls=0;
bool replace_quad=false,throw_group=false,nested=false;
uint32_t next_quad=0;
constexpr uint32_t actor=0x81001000,hud=0x81100000,quad=0x81200000,material=0x81300000,parent=0x81400000;
void update(PPCContext& c){++update_calls;if(replace_quad)mem_write32(c.r[3]+0x4ec,next_quad);c.r[3]=0x1234;}
void bind(PPCContext& c){++material_calls;c.r[3]=0x4321;}
void resource(PPCContext& c,uint32_t q=quad,uint32_t m=material){c.lr=0x8011e90c;c.r[28]=q;c.r[3]=m;port::material_setup(c);}
void group(PPCContext& c){++group_calls;resource(c);if(throw_group)throw std::runtime_error("synthetic render failure");if(nested){nested=false;port::render_group(c);}c.r[3]=0x5678;}
// Synthetic HLE collaborator models a network-installed group hook replacing
// the offline wrapper. The actual registered port observer owns marker scope.
void network_list(PPCContext& c,bool local=true){
    if(!local)return;
    const auto callback=ssne::hud_list_observer;
    ssne::detail::NativeHudListScope scope(callback);
    group(c);
}
void node(uint32_t a,uint32_t h,uint32_t q,uint32_t m,uint32_t p,unsigned slot){
    mem_write32(0x81000000+slot*4,a);mem_write32(a+0x48,0x802121e8);mem_write32(a+0x374,h);
    mem_write32(h+0x14,p);mem_write32(h+0x4ec,q);mem_write32(h+0x4e8,m);
    mem_write32(q,0x8021cb58);mem_write32(q+8,m);mem_write32(q+0xc,0);mem_write32(q+0x10,p);
    mem_write32(m,0x8021d2a8);mem_write32(p,0x8021cab0);mem_write32(p+0xc,q);
}
void remember(PPCContext& c,uint32_t h=hud){c.r[3]=h;port::reticle_update(c);check(c.r[3]==0x1234,"update return registers changed");}
void clear_markers(){ssne::markers.clear();check(!port::scope_on,"scope leaked before next fixture");}
}
const SSNEFuncEntry g_ssne_funcs[]={{0x800bdc1c,update,"synthetic update"},{0x8012680c,bind,"synthetic material"},{0x8011e5ac,group,"synthetic group"}};
const size_t g_ssne_func_count=std::size(g_ssne_funcs);const uint32_t g_ssne_entry=0;const bool g_ssne_has_game_code=true;
const SSNEFuncEntry* ssne_find_func(uint32_t a){for(const auto& f:g_ssne_funcs)if(f.address==a)return &f;return nullptr;}
PPCFunc* ssne_hook_slot(uint32_t a){return &hooks[a];}
void ssne_install_hook(uint32_t a,PPCFunc f){hooks[a]=f;}

int main(){try{
    PPCContext c{};
    check(!port::install_native_crosshair()&&!port::native_crosshair_supported()&&hooks.empty(),"unsupported ID installed hooks");
    mem_write32(0x80000000,0x47334250);
    check(!port::install_native_crosshair()&&!port::native_crosshair_supported()&&hooks.empty(),"unverified PAL bodies installed hooks");
    // The full PAL code-hash gate remains strict. Actual hook transport below
    // uses only synthetic collaborators; positive ROM installation is native QA.
    port::original_update=update;port::original_material=bind;port::original_group=group;port::supported=true;
    node(actor,hud,quad,material,parent,0);remember(c);check(port::reticles.size()==1&&update_calls==1,"original update not called before capture");
    const std::vector<uint8_t> guest_before(ssne::ram.begin(),ssne::ram.end());
    port::render_group(c);check(ssne::markers.empty()&&material_calls==1&&group_calls==1&&c.r[3]==0x5678,"disabled mode changed original path");
    check(std::equal(ssne::ram.begin(),ssne::ram.end(),guest_before.begin()),"transport mutated guest HUD memory");
    ssne::crosshair_on=true;port::render_group(c);check(ssne::markers==std::vector<bool>({true,false})&&!port::scope_on&&port::group_depth==0,"exact reticle scope missing or leaked");clear_markers();
    // Same shared resource on another primitive is not a reticle.
    port::group_depth=1;c.lr=0x8011e90c;c.r[28]=quad+0x100;c.r[3]=material;port::material_setup(c);port::group_depth=0;
    check(ssne::markers.empty(),"shared HUD resource tagged an unrelated quad");
    c.lr=0x80000004;c.r[28]=quad;c.r[3]=material;port::material_setup(c);check(ssne::markers.empty(),"outside-group/non-GUI bind tagged a reticle");
    // Removal/reuse rejects stale identities even before the next frame prune.
    mem_write32(hud+0x4ec,0);port::render_group(c);check(ssne::markers.empty(),"deleted quad pointer tagged reused storage");
    port::native_crosshair_frame();check(port::reticles.empty(),"deleted identity not pruned");
    mem_write32(hud+0x4ec,quad);remember(c);mem_write32(parent+0xc,0);port::render_group(c);check(ssne::markers.empty(),"unlinked quad remained tagged");
    mem_write32(parent+0xc,quad);remember(c);
    const auto replacement=quad+0x200;mem_write32(replacement,0x8021cb58);mem_write32(replacement+8,material);mem_write32(replacement+0xc,0);mem_write32(replacement+0x10,parent);mem_write32(parent+0xc,replacement);
    next_quad=replacement;replace_quad=true;remember(c);replace_quad=false;
    port::render_group(c);check(ssne::markers.empty(),"old identity tagged after the original update replaced its quad");
    port::group_depth=1;resource(c,replacement,material);port::clear_scope();port::group_depth=0;
    check(ssne::markers==std::vector<bool>({true,false}),"new quad was not captured after original update");clear_markers();
    mem_write32(hud+0x4ec,quad);mem_write32(parent+0xc,quad);remember(c);mem_write32(actor+0x374,0);port::render_group(c);check(ssne::markers.empty(),"stale HUD ownership remained tagged");
    mem_write32(actor+0x374,hud);remember(c);
    throw_group=true;bool threw=false;try{port::render_group(c);}catch(const std::runtime_error&){threw=true;}throw_group=false;
    check(threw&&ssne::markers==std::vector<bool>({true,false})&&!port::scope_on&&port::group_depth==0,"exception leaked reticle scope");clear_markers();
    nested=true;port::render_group(c);check(ssne::markers==std::vector<bool>({true,false,true,false})&&!port::scope_on&&port::group_depth==0,"recursive groups leaked/inherited scope");clear_markers();
    node(actor+0x1000,hud+0x1000,quad+0x1000,material+0x1000,parent+0x1000,1);remember(c,hud+0x1000);check(port::reticles.size()==2,"second player reticle identity missing");
    port::group_depth=1;resource(c,quad+0x1000,material+0x1000);resource(c,quad,material);port::clear_scope();port::group_depth=0;
    check(ssne::markers==std::vector<bool>({true,false}),"separate active player reticles failed exact identity matching");clear_markers();
    // Authority installs a new HLE renderer after boot and restores the old
    // offline hook on leave. No reattach/chaining cycle is needed: both actual
    // entry paths use the same bounded scope helper around original drawing.
    ssne::set_native_hud_list_observer(port::observe_hud_list);
    ssne::crosshair_on=false;network_list(c);check(ssne::markers.empty()&&port::group_depth==0,"disabled HLE list changed original draw");
    ssne::crosshair_on=true;network_list(c,false);check(ssne::markers.empty()&&port::group_depth==0,"nonlocal skipped HLE list opened reticle scope");
    network_list(c);check(ssne::markers==std::vector<bool>({true,false})&&port::group_depth==0,"network HLE list failed without offline group hook");clear_markers();
    throw_group=true;threw=false;try{network_list(c);}catch(const std::runtime_error&){threw=true;}throw_group=false;
    check(threw&&ssne::markers==std::vector<bool>({true,false})&&!port::scope_on&&port::group_depth==0,"network HLE exception leaked scope");clear_markers();
    nested=true;network_list(c);check(ssne::markers==std::vector<bool>({true,false,true,false})&&port::group_depth==0,"nested HLE/offline scopes leaked");clear_markers();
    port::render_group(c);check(ssne::markers==std::vector<bool>({true,false})&&port::group_depth==0,"restored offline renderer lost original draw");clear_markers();
    network_list(c);check(ssne::markers==std::vector<bool>({true,false})&&port::group_depth==0,"rehost HLE observer lost scope");clear_markers();
    ssne::set_native_hud_list_observer(nullptr);resource(c);check(ssne::markers.empty(),"unregistered HLE observer tagged outside group");
    mem_write32(0x802db838,1);port::native_crosshair_frame();check(port::native_crosshair_language()==1,"original German language not published");
    mem_write32(0x802db838,9);port::native_crosshair_frame();check(port::native_crosshair_language()==0,"invalid native language not bounded");
    std::cout<<"Native reticle hooks: strict unsupported guards, disabled fallback, original calls, no HUD writes, exact identity, ownership/unlink/delete/reuse, nested/exception scope clearing, HLE authority observer/skip/restore/rehost, language and multiple players passed (PPC and renderer collaborators synthetic).\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
