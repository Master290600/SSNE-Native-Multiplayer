namespace native_render_list_adapter {
PPCFunc original=nullptr;
void draw(PPCContext& c){
    using namespace native_render_paths;
    static const bool enabled=[] {auto s=std::getenv("SSNE_NATIVE_RENDER_LIST");return !s || std::strcmp(s,"0");}();
    poll(c);
    if(!enabled || c.r[13]!=0x802E22A0u || !stack_ok(c)){
        ssne_call_original(c,0x80139688,original);return;
    }
    Frame frame(c,0xF0,23);
    const auto call=[&]<uint32_t Address>(uint32_t caller,std::initializer_list<uint32_t> args){
        invoke<Address>(c,caller,args);
    };
    ::ssne::native_render_list::draw(c,frame.sp,call,[&]{poll(c);});
}
void install(){
    if(mem_read32(0x80000000)!=0x47334250u || !ssne_find_func(0x80139688))return;
    uint64_t hash=14695981039346656037ull;
    for(unsigned i=0;i<936;++i){hash^=mem_read8(0x80139688+i);hash*=1099511628211ull;}
    if(hash!=0x2E5E48F782EFE178ull)return;
    original=ssne_find_func(0x80139688)->func;
    ssne_install_hook(0x80139688,draw);
    log_message("[ssne] native EU model render-list traversal installed (80139688)");
}
}
