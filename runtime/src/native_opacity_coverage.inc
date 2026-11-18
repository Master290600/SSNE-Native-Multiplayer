// The original type-2 opacity layer replaces alpha. Preserve texture coverage
// before that layer, then multiply it by the authored opacity-table fade.
// This adapts only the pinned PAL layer setup; GX TEV semantics stay unchanged.
namespace native_opacity_coverage {
constexpr uint32_t alpha_address=0x80123954u,layer_address=0x80123D0Cu;
constexpr uint32_t stage_address=0x802DC61Cu,color_modes=0x802442B4u,alpha_modes=0x80244334u;
PPCFunc previous=nullptr;
bool installed=false;
thread_local bool active=false;
bool range(uint32_t p,uint64_t n){return p>=0x80000000u && n<=guest_ram_size() && uint64_t(p)+n<=guest_ram_end();}
bool eligible(const PPCContext& c){
    if(active || c.lr!=0x80124DF4u || c.r[3]!=1001 || c.r[13]!=0x802E22A0u ||
       (c.r[1]&7) || !range(c.r[1]-8,0x78) || mem_read32(0x80000000u)!=0x47334250u)return false;
    // This return address is the shared alpha-mode call in cases 100..102.
    if(mem_read32(0x80124DF0u)!=0x4BFFEB65u || mem_read32(layer_address)!=0x9421FF98u ||
       mem_read32(alpha_address)!=0x9421FFF8u)return false;
    const auto material=c.r[26],ordinal=c.r[25],stage=mem_read32(stage_address);
    if(!range(material,0x30) || c.r[23]!=material+12 || (ordinal!=4 && ordinal!=8 && ordinal!=12) ||
       stage<1 || stage>15 || c.r[29]!=stage)return false;
    const auto base=mem_read32(material+12),opacity=mem_read32(material+12+ordinal);
    if(!range(base,0x14) || !range(opacity,0x14) || base==opacity || mem_read32(opacity+4)!=2)return false;
    if(mem_read32(alpha_modes)!=2000 || mem_read32(color_modes+stage*4)!=2007)return false;
    for(uint32_t i=1;i<stage;++i)if(mem_read32(alpha_modes+i*4)!=1002)return false;
    return true;
}
void alpha(PPCContext& c){
    if(!eligible(c)){ssne_call_original(c,alpha_address,previous);return;}
    // The original helper reads the current stage from SDA. Use a private CPU
    // context for the extra write and always restore the selector, including an
    // exceptional exit. Its mode cache intentionally records the emitted state.
    struct Restore {
        uint32_t stage;bool was_active;
        Restore():stage(mem_read32(stage_address)),was_active(active){active=true;}
        ~Restore(){mem_write32(stage_address,stage);active=was_active;}
    } restore;
    auto coverage=c;coverage.r[3]=1001;
    coverage.msr&=~0x8000u; // no IRQ/reschedule while the shared selector is temporary
    mem_write32(stage_address,0);
    ssne_call_original(coverage,alpha_address,previous);
    mem_write32(stage_address,restore.stage);
    c.r[3]=2001;
    ssne_call_original(c,alpha_address,previous);
}
bool fingerprint(uint32_t a,uint32_t n,uint64_t expected){
    if(!range(a,n))return false;
    uint64_t h=14695981039346656037ull;
    for(uint32_t i=0;i<n;++i){h^=mem_read8(a+i);h*=1099511628211ull;}
    return h==expected;
}
void install(){
    if(installed || mem_read32(0x80000000u)!=0x47334250u)return;
    const auto entry=ssne_find_func(alpha_address);
    if(!entry || !ssne_find_func(layer_address) ||
       !fingerprint(alpha_address,844,0xCC1EAA1BD396FB9Cull) ||
       !fingerprint(layer_address,10684,0xDE72322195D12991ull))return;
    const auto hook=*ssne_hook_slot(alpha_address);previous=hook?hook:entry->func;
    ssne_install_hook(alpha_address,alpha);installed=true;
    log_message("[ssne] native opacity layers preserve base texture coverage");
}
}
