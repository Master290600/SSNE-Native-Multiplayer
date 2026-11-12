// PAL cAnimControl owns every completion callback that captures its address.
// Shared/independent model instances can retain the same callback even after
// Stop() releases the controller's current model. Retire that exact capture
// while the controller is still alive, before its original destructor runs.
namespace native_animation_lifetime {
PPCFunc destructor_original=nullptr,setter_original=nullptr;
constexpr uint32_t destructor_address=0x800039E8u,setter_address=0x801286A8u;
constexpr uint32_t completion_callback=0x80003774u;
bool range(uint32_t p,uint64_t bytes){return p>=0x80000000u && uint64_t(p)+bytes<=guest_ram_end();}
std::vector<uint32_t> captured_models(uint32_t control){
    std::vector<uint32_t> matches;
    const auto manager=mem_read32(0x802ABAA4u);
    if(!range(manager,0x18) || mem_read32(manager)!=0x8021EB70u)return matches;
    std::set<uint32_t> seen;auto model=mem_read32(manager+0x14);
    while(model){
        // Validate the whole authoritative list before mutating any callback.
        if(!range(model,0xB0) || seen.size()>=32768 || !seen.insert(model).second || mem_read32(model)!=0x8021D668u || mem_read32(model+8)!=manager)return {};
        if(mem_read32(model+0x6C)==completion_callback && mem_read32(model+0x70)==control)matches.push_back(model);
        model=mem_read32(model+4);
    }
    return matches;
}
void destroy(PPCContext& context){
    const auto control=context.r[3];const auto models=captured_models(control);
    for(const auto model:models){
        auto call=context;call.r[3]=model;call.r[4]=call.r[5]=call.r[6]=0;
        // The verified setter is a 24-byte leaf; no guest stack or unrelated
        // model clock, pose, resource ownership or reference count is changed.
        ssne_call_original(call,setter_address,setter_original);
        static unsigned traced=0;
        if(std::getenv("SSNE_TEST_HIDDEN") && std::getenv("SSNE_TEST_ANIMATION_LIFETIME_TRACE") && traced<64){
            ++traced;log_message("[native-animation-callback-retired] control="+std::to_string(control)+" model="+std::to_string(model)+" resource="+std::to_string(mem_read32(model+0xC))+" current="+std::to_string(range(control,0x2C)?mem_read32(control+0x28):0));
        }
    }
    ssne_call_original(context,destructor_address,destructor_original);
}
uint64_t signature(uint32_t address,unsigned size){uint64_t h=14695981039346656037ull;for(unsigned i=0;i<size;++i){h^=mem_read8(address+i);h*=1099511628211ull;}return h;}
void install(){
    if(destructor_original || mem_read32(0x80000000u)!=0x47334250u)return;
    const auto dtor=ssne_find_func(destructor_address),setter=ssne_find_func(setter_address);
    if(!dtor || !setter || signature(destructor_address,32)!=0x5a316a46a0212f5full || signature(setter_address,24)!=0x763a7c1bd9f36744ull)return;
    destructor_original=dtor->func;setter_original=setter->func;ssne_install_hook(destructor_address,destroy);
    log_message("[ssne] native PAL animation callback ownership installed (800039E8)");
}
}
