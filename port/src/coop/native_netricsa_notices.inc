// Shared encyclopedia knowledge. No guest addresses, GUI handles, read flags or
// animation state cross the network. Existing local pages retain their read state.
struct NetricsaPageDescription {
    uint32_t kind=0,title=0,text=0,model_kind=0,model=0,parameter=0,attachments=0;
    std::array<uint32_t,3> position{};std::string image;
};
// All eight decorated enemy model classes in the original executable. Their
// attachment recipes and animation constants are executable data, resolved
// locally; neither recipe pointers nor live attachment handles go on the wire.
struct NetricsaModelClass {uint32_t table,recipe,phase;};
constexpr std::array<NetricsaModelClass,8> netricsa_decorated_models={{
 {0x801EA440,0x80276E24,0},{0x801EB990,0x801EBE70,0x801EB98C},
 {0x801EF300,0x801EFBA8,0x801EF2F8},{0x801F01F8,0x801F0A28,0x801F01F4},
 {0x801F24C0,0x801F29E0,0},{0x801F68A0,0x801F6D74,0x801F689C},
 {0x802241F8,0x80224A8C,0x802241EC},{0x80225440,0x80225CC4,0x80225434}}};
constexpr unsigned netricsa_page_limit=256,netricsa_wire_limit=16384;
bool netricsa_image_ok(const std::string& s){return !s.empty() && s.size()<=95 && s.starts_with("NETRISCA_") && std::all_of(s.begin(),s.end(),[](unsigned char c){return c=='_' || (c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9');});}
bool read_netricsa_page(uint32_t page,NetricsaPageDescription& d,std::string& error){
    if(!readable(page,0x3C)){error="Invalid shared Netricsa page";return false;}
    const auto table=mem_read32(page+0x38);
    d.kind=table==0x802192C8u?1:table==0x80219290u?2:table==0x80219300u?3:0;
    d.title=mem_read32(page+0x10);d.text=mem_read32(page+0x14);
    if(!d.kind){error="Unsupported shared Netricsa page class";return false;}
    if(d.kind==3){const auto name=mem_read32(page+0x20);if(!readable(name,96)){error="Invalid Netricsa image binding";return false;}
        for(unsigned i=0;i<96 && mem_read8(name+i);++i)d.image+=char(mem_read8(name+i));
        if(!netricsa_image_ok(d.image)){error="Invalid shared Netricsa image";return false;}
    }else{const auto object=mem_read32(page+0x18);d.parameter=d.kind==2?mem_read32(page+0x1C):0;
        if(!readable(object,0x24)){error="Invalid Netricsa model binding";return false;}
        const auto type=mem_read32(object+0x20);d.model_kind=type==0x80219580u?1:type==0x802195C8u?2:type==0x80219538u?3:type==0x80219610u?4:0;
        for(unsigned i=0;i<netricsa_decorated_models.size();++i)if(type==netricsa_decorated_models[i].table)d.model_kind=5+i;
        if(d.model_kind==3 || d.model_kind>=5){if(!readable(object,0x130)){error="Invalid decorated Netricsa model";return false;}for(unsigned i=0;i<netricsa_decorated_models.size();++i)if(mem_read32(object+0x24)==netricsa_decorated_models[i].recipe)d.attachments=i+1;}
        if(!d.model_kind || ((d.model_kind==3 || d.model_kind>=5) && !d.attachments)){error="Unsupported shared Netricsa model";return false;}
        d.model=mem_read32(object);for(unsigned i=0;i<3;++i)d.position[i]=mem_read32(object+8+4*i);

    }return true;
}
void pack_netricsa_page(const NetricsaPageDescription& d,std::vector<int64_t>& v){
    for(auto x:{d.kind,d.title,d.text,d.model_kind,d.model,d.position[0],d.position[1],d.position[2],d.parameter,uint32_t(d.image.size()),d.attachments})v.push_back(x);
    for(unsigned i=0;i<d.image.size();i+=4){uint32_t word=0;for(unsigned j=0;j<4 && i+j<d.image.size();++j)word|=uint32_t(uint8_t(d.image[i+j]))<<(24-8*j);v.push_back(word);}
}
bool unpack_netricsa_pages(const EntityState& e,std::vector<NetricsaPageDescription>& pages,std::string& error){
    const auto p=e.properties.find("netricsa_pages");const auto* v=p==e.properties.end()?nullptr:std::get_if<std::vector<int64_t>>(&p->second);
    auto fail=[&](){error="Invalid shared Netricsa catalog";return false;};
    if(!v || v->size()>netricsa_wire_limit)return fail();std::set<uint32_t> titles;
    for(size_t i=0;i<v->size();){if(v->size()-i<11 || pages.size()>=netricsa_page_limit)return fail();
        for(unsigned j=0;j<11;++j)if((*v)[i+j]<0 || (*v)[i+j]>UINT32_MAX)return fail();
        NetricsaPageDescription d;d.kind=uint32_t((*v)[i]);d.title=uint32_t((*v)[i+1]);d.text=uint32_t((*v)[i+2]);d.model_kind=uint32_t((*v)[i+3]);d.model=uint32_t((*v)[i+4]);
        for(unsigned j=0;j<3;++j)d.position[j]=uint32_t((*v)[i+5+j]);d.parameter=uint32_t((*v)[i+8]);const auto length=unsigned((*v)[i+9]);d.attachments=uint32_t((*v)[i+10]);i+=11;
        if(d.kind<1 || d.kind>3 || !d.title || d.title>65535 || !d.text || d.text>65535 || !titles.insert(d.title).second || length>95 || (length+3)/4>v->size()-i)return fail();
        for(unsigned j=0;j<length;j+=4){const auto word=(*v)[i++];if(word<0 || word>UINT32_MAX)return fail();for(unsigned k=0;k<4;++k){const auto c=uint8_t(uint64_t(word)>>(24-8*k));if(j+k<length)d.image+=char(c);else if(c)return fail();}}
        if(d.kind==3){if(!netricsa_image_ok(d.image) || d.model_kind || d.model || d.parameter || d.attachments || d.position!=std::array<uint32_t,3>{})return fail();}
        else {if(length || d.model_kind<1 || d.model_kind>12 || d.model>65535 || d.attachments>8 || ((d.model_kind==3 || d.model_kind>=5)!=bool(d.attachments)) || (d.model_kind>=5 && d.attachments!=d.model_kind-4))return fail();for(auto bits:d.position)if(!std::isfinite(u2f(bits)) || std::abs(u2f(bits))>10000.f)return fail();}
        pages.push_back(std::move(d));
    }return true;
}
bool capture_shared_netricsa(uint32_t actor,EntityState& e,std::string& error){
    std::vector<int64_t> packed;std::set<uint32_t> titles,actors;
    // Host first, followed by all party owners: entries discovered by either
    // player belong to the shared campaign, including a late joiner's history.
    for(unsigned slot=0;slot<=ssne::native_party_capacity();++slot){const auto source=slot<ssne::native_party_capacity()?mem_read32(ssne::native_party_registry()+slot*4):actor;
        if(!readable(source,0x790) || !actors.insert(source).second)continue;const auto info=mem_read32(source+0x720);
        if(!readable(info,0x98) || mem_read32(info+0x48)!=0x80219030u)continue;
        std::set<uint32_t> seen;for(auto page=mem_read32(info+0x4C);page;page=mem_read32(page+8)){
            if(!readable(page,0x3C) || !seen.insert(page).second || seen.size()>netricsa_page_limit){error="Invalid shared Netricsa list";return false;}
            if(!titles.insert(mem_read32(page+0x10)).second)continue;
            if(titles.size()>netricsa_page_limit){error="Netricsa catalog exceeds native bound";return false;}
            NetricsaPageDescription d;if(!read_netricsa_page(page,d,error))return false;pack_netricsa_page(d,packed);
        }
    }e.properties["netricsa_pages"]=std::move(packed);return true;
}
bool apply_shared_netricsa(PPCContext& context,uint32_t actor,const EntityState& e,std::string& error){
    std::vector<NetricsaPageDescription> pages;if(!unpack_netricsa_pages(e,pages,error))return false;
    const auto info=mem_read32(actor+0x720);std::set<uint32_t> titles;
    for(auto page=mem_read32(info+0x4C);page;page=mem_read32(page+8))titles.insert(mem_read32(page+0x10));
    auto invoke=[&](uint32_t fn,uint32_t r3,uint32_t r4=0,uint32_t r5=0,uint32_t r6=0){auto call=context;call.msr&=~0x8000u;call.r[1]-=0x300;mem_write32(call.r[1],context.r[1]);call.r[3]=r3;call.r[4]=r4;call.r[5]=r5;call.r[6]=r6;ssne_call(call,fn);return call.r[3];};
    auto allocate=[&](unsigned size){auto result=invoke(0x80115DCC,size);if(!result){result=invoke(0x80116084,mem_read32(context.r[13]-0x6DE0),size);if(result)mem_write32(context.r[13]-0x6DF0,mem_read32(context.r[13]-0x6DF0)-mem_read32(result-24));}if(!readable(result,size))ssne::fatal("Native Netricsa allocation failed");return result;};
    unsigned added=0;for(const auto& d:pages){if(titles.contains(d.title))continue;
        // Original native page/model layouts and constructors. The embedded
        // image string lives with its page, never in a temporary network buffer.
        const auto page=allocate(0x40+unsigned(d.image.size())+1);for(unsigned i=0;i<0x40;++i)mem_write8(page+i,0);
        mem_write32(page+0x38,d.kind==1?0x802192C8u:d.kind==2?0x80219290u:0x80219300u);mem_write32(page+0x10,d.title);mem_write32(page+0x14,d.text);
        if(d.kind==3){mem_write32(page+0x20,page+0x40);for(unsigned i=0;i<=d.image.size();++i)mem_write8(page+0x40+i,i<d.image.size()?uint8_t(d.image[i]):0);}
        else{const bool decorated=d.model_kind==3 || d.model_kind>=5;const auto size=decorated?0x130u:0x24u;const auto object=allocate(size);for(unsigned i=0;i<size;++i)mem_write8(object+i,0);
            const auto position=context.r[1]-0x180;for(unsigned i=0;i<3;++i)mem_write32(position+4*i,d.position[i]);
            invoke(d.model_kind==1?0x80104348:d.model_kind==2?0x80103F8C:decorated?0x801044DC:0x80103DC0,object,d.model,position,decorated?netricsa_decorated_models[d.attachments-1].recipe:0);
            if(d.model_kind>=5){const auto& type=netricsa_decorated_models[d.model_kind-5];mem_write32(object+0x20,type.table);if(type.phase)mem_write32(object+0x128,mem_read32(type.phase));}
            mem_write32(page+0x18,object);mem_write32(page+0x1C,d.parameter);
        }
        const auto tail=mem_read32(info+0x50);if(tail)mem_write32(tail+8,page);else mem_write32(info+0x4C,page);mem_write32(page+0xC,tail);mem_write32(info+0x50,page);mem_write32(info+0x60,mem_read32(info+0x5C));mem_write32(info+0x5C,mem_read32(info+0x5C)+1);titles.insert(d.title);++added;
    }
    if(added){if(mem_read32(info+0x6C))invoke(0x80102888,info);invoke(0x800B34A4,actor);ssne::log_message("[coop-netricsa-catalog] slot="+std::to_string(mem_read32(actor+0x17C))+" added="+std::to_string(added)+" total="+std::to_string(mem_read32(info+0x5C)));}
    return true;
}
