// Included inside main.cpp's private namespace; all guest access stays on the
// guest frame thread. Geometry is resolved by native_menu_mouse's renderer path.
namespace native_info_mouse {
namespace {
constexpr uint16_t info_root=0xe3,tab_first=0x5000,body_id=0x5200;
constexpr uint16_t scroll_previous=0x5201,scroll_next=0x5202,page_previous=0x5203,page_next=0x5204;
bool readable(uint32_t a,unsigned n){return a>=0x80000000u&&n<=ssne::guest_ram_size()&&a<=ssne::guest_ram_end()-n;}
uint32_t word(uint32_t a){return readable(a,4)?mem_read32(a):0;}
uint16_t half(uint32_t a){return readable(a,2)?mem_read16(a):0;}
float number(uint32_t a){return u2f(word(a));}
bool revision(){
    if(word(0x80000000u)!=0x47334250u)return false;
    static const bool checked=[] {
        constexpr std::pair<uint32_t,uint64_t> signatures[]={
            {0x801019d0u,0x08478b35643406d1ull},{0x80102240u,0x057260b785a598ebull},
            {0x80102278u,0xa9f46edd0b888926ull},{0x801022ecu,0x7278f2f4c89ef2bdull},
            {0x801024b8u,0x5076a61fd41e0908ull}};
        for(const auto [address,expected]:signatures){
            if(!ssne_find_func(address))return false;
            uint64_t hash=14695981039346656037ull;
            for(unsigned i=0;i<32;++i){hash^=mem_read8(address+i);hash*=1099511628211ull;}
            if(hash!=expected)return false;
        }
        return ssne_find_func(0x800fb130u)!=nullptr;
    }();
    return checked;
}
uint32_t binding(uint32_t gui,unsigned event,uint32_t function){
    std::set<uint32_t> seen;
    for(auto p=word(gui+0x94);readable(p,0x20)&&seen.size()<2048&&seen.insert(p).second;p=word(p+8))
        if(half(p+0x18)==info_root&&word(p+0x14)==event&&word(p+0x1c)==function)return word(p+0x10);
    return 0;
}
struct State {uint32_t gui=0,node=0,owner=0,count=0,selected=0;std::vector<uint32_t> entries;};
bool owner_state(uint32_t gui,State& s,const char** rejection=nullptr){
    auto fail=[&](const char* why){if(rejection)*rejection=why;return false;};
    if(rejection)*rejection="accepted";
    if(!revision())return fail("unsupported_revision");
    if(!readable(gui,0xc8))return fail("invalid_gui");
    auto root=word(gui+0x7c);std::set<uint32_t> parents;
    while(readable(root,0xac)&&word(root+0x24)&&parents.size()<32&&parents.insert(root).second)root=word(root+0x24);
    if(!root)root=word(gui+0x6c);
    if(!readable(root,0xac)||word(root+0x20)!=gui)return fail("invalid_focused_root");
    const auto definition=word(root+0x1c);
    if(!readable(definition,0x44)||half(definition+2)!=info_root)return fail("focused_root_is_not_info");
    // Native 800F6A94 uses flag40 to retain the previous panel under an
    // overlay. Only hidden20 and closing02 suppress this live Info surface.
    const auto flags=word(definition+0xc);
    if(flags&0x20u)return fail("hidden_root");
    if(flags&0x02u)return fail("closing_root");
    bool linked=false;std::set<uint32_t> roots;
    for(auto p=word(gui+0x6c);readable(p,0xac)&&roots.size()<64&&roots.insert(p).second;p=word(p+8))if(p==root)linked=true;
    if(!linked)return fail("unlinked_root");
    const auto owner=binding(gui,20,0x80102278u);
    if(!readable(owner,0xa0)||word(owner+0x48)!=0x80219030u||word(owner+0x94)!=gui)return fail("invalid_callback_owner");
    if(!readable(word(owner+0x6c),0x10))return fail("closed_info_owner");
    if(binding(gui,21,0x80102278u)!=owner||binding(gui,28,0x80102240u)!=owner||binding(gui,29,0x80102240u)!=owner||binding(gui,23,0x801022ecu)!=owner)return fail("callback_owner_mismatch");
    s={gui,root,owner,word(owner+0x5c),word(owner+0x60),{}};return true;
}
bool state(uint32_t gui,State& s){
    if(!owner_state(gui,s)||s.count>512||(s.count&&s.selected>=s.count))return false;
    std::set<uint32_t> seen;
    auto p=word(s.owner+0x4c);
    while(p&&readable(p,0x3c)&&seen.size()<512&&seen.insert(p).second){s.entries.push_back(p);p=word(p+8);}
    return !p&&s.entries.size()==s.count;
}
uint32_t node(uint32_t gui,uint32_t root,uint16_t id){
    std::vector<uint32_t> todo{root};std::set<uint32_t> seen;
    while(!todo.empty()&&seen.size()<4096){const auto p=todo.back();todo.pop_back();
        if(!readable(p,0xac)||!seen.insert(p).second||word(p+0x20)!=gui)continue;
        const auto definition=word(p+0x1c);if(!readable(definition,0x44))continue;
        if(half(definition+2)==id)return p;
        if(p!=root&&word(p+8))todo.push_back(word(p+8));if(word(p+0x34))todo.push_back(word(p+0x34));
    }
    return 0;
}
bool visible_node(uint32_t p){const auto definition=word(p+0x1c);return readable(p,0xac)&&readable(definition,0x44)&&!(word(definition+0xc)&0x22u);}
uint32_t primitive(uint32_t wrapper){
    if(!readable(wrapper,0x18))return 0;
    const auto parameters=word(wrapper),alpha=word(parameters+0x18),object=word(wrapper+8);
    if(!readable(parameters,0x28)||!readable(alpha,8)||!std::isfinite(number(alpha))||!readable(object,0x48))return 0;
    // Original 800F2CC0 fades boundary arrows through their color pair, not
    // their shared panel fade. 800FBACC draws current RGBA alpha (+3), while
    // color+7 is only its target; use the value actually rendered this frame.
    const auto color=word(parameters+0x10);
    if(color&&!readable(color,4))return 0;
    const float opacity=number(alpha)*(color?float(mem_read8(color+3))/255.f:1.f);
    if(opacity<=.01f)return 0;
    return object;
}
void add(std::vector<port::NativeMenuMouseRegion>& out,const State& s,uint16_t id,unsigned event,uint32_t object,const std::string& name,unsigned previous=20,unsigned next=21){
    if(!object)return;
    port::NativeMenuMouseRegion r;r.gui=s.gui;r.node=s.node;r.primitive=object;r.root=info_root;r.id=id;r.event=event;r.previous_event=previous;r.next_event=next;r.control=name;out.push_back(std::move(r));
}
bool send(PPCContext& base,const State& s,unsigned event){
    if(!readable(base.r[1]-0x400,0x400)||!ssne_find_func(0x800fb130u))return false;
    static unsigned trace_count=0; // Guest-thread-owned, bounded per process.
    const auto flag=std::getenv("SSNE_NATIVE_INFO_MOUSE_TRACE");
    const bool trace=std::getenv("SSNE_TEST_HIDDEN")&&flag&&flag[0]=='1'&&!flag[1]&&trace_count<128;
    auto snapshot=[&](){
        const char* rejection="unknown";State observed;const bool accepted=owner_state(s.gui,observed,&rejection);
        const auto selected=word(s.owner+0x60);State live;const bool valid=state(s.gui,live);
        const auto entry=valid&&selected<live.entries.size()?live.entries[selected]:0;
        std::ostringstream out;out<<"{\"selected\":"<<selected<<",\"count\":"<<word(s.owner+0x5c)
            <<",\"open_renderer\":"<<word(s.owner+0x6c)<<",\"root_flags\":"<<word(word(s.node+0x1c)+0xc)
            <<",\"owner_state\":"<<(accepted?"true":"false")<<",\"reason\":\""<<rejection<<"\""
            <<",\"movie\":"<<(ssne::native_movie_presenting()?"true":"false")
            <<",\"selected_entry\":"<<entry<<",\"entry_vtable\":"<<word(entry+0x38)<<",\"entry_show\":"<<word(word(entry+0x38)+0x24)<<'}';
        return out.str();
    };
    const auto before=trace?snapshot():std::string{};
    auto c=base;c.r[1]-=0x400;c.msr&=~0x8000u;mem_write32(c.r[1],base.r[1]);c.r[3]=s.node;c.r[4]=event;ssne_call(c,0x800fb130u);
    if(trace){
        const auto after=snapshot();const auto serial=++trace_count;
        const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        std::ofstream log("native-info-mouse-events.jsonl",std::ios::app|std::ios::binary);
        log<<"{\"sequence\":"<<serial<<",\"monotonic_us\":"<<us<<",\"event\":"<<event<<",\"owner\":"<<s.owner
           <<",\"gui\":"<<s.gui<<",\"node\":"<<s.node<<",\"before\":"<<before<<",\"after\":"<<after<<"}\n";
    }
    return true;
}
}
// Proven ownership: native 801019D0 retains its player argument in +90 and GUI
// in +94. hle_authoritative_netricsa uses this same player +17C ownership check.
bool local_gui(uint32_t gui,unsigned slot){
    if(slot>=8||slot>=ssne::native_party_capacity())return false;
    State s;if(!owner_state(gui,s))return false;
    const auto actor=word(s.owner+0x90),registry=ssne::native_party_registry();
    return readable(actor,0x790)&&readable(registry,4*(slot+1))&&word(registry+4*slot)==actor&&word(actor+0x48)==ssne::game_address(0x80212268u)&&word(actor+0x17c)==slot;
}
std::vector<port::NativeMenuMouseRegion> regions(PPCContext&){
    std::vector<port::NativeMenuMouseRegion> out;std::set<uint32_t> guis;
    for(auto gui=word(0x8028bfa4u);readable(gui,0xc8)&&guis.size()<32&&guis.insert(gui).second;gui=word(gui+8)){
        State s;if(!state(gui,s))continue;
        const auto body=node(gui,s.node,0xe6);
        // Emit the full original text viewport first, under its native arrows.
        if(visible_node(body)){
            port::NativeMenuMouseRegion r;r.gui=gui;r.node=s.node;r.viewport_node=body;r.root=info_root;r.id=body_id;r.event=0;r.previous_event=28;r.next_event=29;r.control="info.body";out.push_back(std::move(r));
            add(out,s,scroll_previous,28,primitive(word(body+0xd8)),"info.scroll.previous",28,29);
            add(out,s,scroll_next,29,primitive(word(body+0xdc)),"info.scroll.next",28,29);
        }
        for(unsigned i=0;i<s.entries.size();++i)add(out,s,uint16_t(tab_first+i),22,primitive(word(s.entries[i]+0x30)),"info.tab."+std::to_string(i));
        for(unsigned i=0;i<2;++i){const auto arrow=node(gui,s.node,uint16_t(0xed+i));if(visible_node(arrow))add(out,s,i?page_next:page_previous,20+i,primitive(word(arrow+0xb4)),i?"info.page.next":"info.page.previous");}
    }
    return out;
}
bool event(PPCContext& c,uint32_t gui,uint32_t target,uint16_t root,uint16_t id,unsigned action){
    if(root!=info_root)return false;
    State s;if(!state(gui,s)||s.node!=target)return false;
    // Wheel directions are relative to the current selection, while a click
    // identifies the actual visible tab. Both reach original page callbacks.
    if(action==23)return send(c,s,23);
    const bool tab=id>=tab_first&&unsigned(id-tab_first)<s.count;
    if((tab||id==page_previous||id==page_next)&&(action==20||action==21))return send(c,s,action);
    if(tab&&action==22){
        const auto wanted=unsigned(id-tab_first),owner=s.owner;
        for(unsigned steps=0;steps<s.count&&s.selected!=wanted;++steps){
            const auto previous=s.selected;
            if(!send(c,s,wanted<s.selected?20:21)||!state(gui,s)||s.node!=target||s.owner!=owner||s.selected==previous)return false;
        }
        return s.selected==wanted;
    }
    if((id==body_id||id==scroll_previous||id==scroll_next)&&(action==28||action==29))return send(c,s,action);
    return false;
}
// Guest-thread copied diagnostics; window code never reads live guest memory.
std::string diagnostics(){
    std::ostringstream out;out<<"{\"surfaces\":[";bool comma=false;std::set<uint32_t> guis;
    for(auto gui=word(0x8028bfa4u);readable(gui,0xc8)&&guis.size()<32&&guis.insert(gui).second;gui=word(gui+8)){
        State s;if(!state(gui,s))continue;const auto actor=word(s.owner+0x90),body=node(gui,s.node,0xe6);auto scroll=number(body+0xbc);if(!std::isfinite(scroll))scroll=0;
        if(comma)out<<',';comma=true;out<<"{\"gui\":"<<gui<<",\"node\":"<<s.node<<",\"owner\":"<<s.owner<<",\"actor\":"<<actor<<",\"slot\":"<<(readable(actor,0x180)?word(actor+0x17c):999u)<<",\"count\":"<<s.count<<",\"selected\":"<<s.selected<<",\"tab_start\":"<<int32_t(word(s.owner+0x64))<<",\"scroll\":"<<scroll<<'}';
    }
    out<<"],\"linked_roots\":[";comma=false;guis.clear();
    for(auto gui=word(0x8028bfa4u);readable(gui,0xc8)&&guis.size()<32&&guis.insert(gui).second;gui=word(gui+8)){
        std::set<uint32_t> roots;
        for(auto root=word(gui+0x6c);readable(root,0xac)&&roots.size()<64&&roots.insert(root).second;root=word(root+8)){
            const auto definition=word(root+0x1c);if(!readable(definition,0x44)||half(definition+2)!=info_root)continue;
            State s;const char* rejection="unknown";const bool accepted=owner_state(gui,s,&rejection);
            auto focused=word(gui+0x7c);std::set<uint32_t> parents;
            while(readable(focused,0xac)&&word(focused+0x24)&&parents.size()<32&&parents.insert(focused).second)focused=word(focused+0x24);
            if(comma)out<<',';comma=true;
            out<<"{\"gui\":"<<gui<<",\"node\":"<<root<<",\"flags\":"<<word(definition+0xc)<<",\"focused_root\":"<<focused<<",\"focused_root_id\":"<<half(word(focused+0x1c)+2)<<",\"owner_state\":"<<(accepted?"true":"false")<<",\"reason\":\""<<rejection<<"\"}";
        }
    }
    out<<"]}";return out.str();
}
}
