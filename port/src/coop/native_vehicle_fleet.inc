// PAL vehicle wrappers own their car, physics and renderer proxies. Allocate
// through the same heap/actor manager as the level loader. Keep private config
// data in the wrapper allocation, so ordinary map teardown owns its lifetime.
using VehiclePoint=std::array<float,3>;
struct FleetVehicle {
    unsigned reference=0,parent=0;
    VehiclePoint parking{};
};
bool party_vehicle(uint32_t object){
    return readable(object,0xA8) && mem_read32(object+0x48)==0x80209BB8u &&
        mem_read32(object+0x58)==object+0x94;
}
VehiclePoint vehicle_point(uint32_t address){
    return {u2f(mem_read32(address)),u2f(mem_read32(address+4)),u2f(mem_read32(address+8))};
}
bool vehicle_reference_space(unsigned needed){
    const auto count=mem_read32(0x802DB7C8),capacity=mem_read32(0x802DB7CC);
    return capacity<=32768 && count<=capacity && needed<=capacity-count &&
        readable(mem_read32(0x802DB7C4),capacity*8);
}
bool vehicle_reference_taken(unsigned reference){
    const auto table=mem_read32(0x802DB7C4),count=mem_read32(0x802DB7C8);
    unsigned lo=0,hi=count;
    while(lo<hi){
        const unsigned mid=lo+(hi-lo)/2;
        if(mem_read16(table+8*mid)<reference)lo=mid+1;else hi=mid;
    }
    return lo<count && mem_read16(table+8*lo)==reference;
}
uint32_t spawn_party_vehicle(PPCContext& context,const FleetVehicle& spec,uint32_t source,bool visible){
    if(!context_ok(context) || !vehicle_reference_space(1))return 0;
    const auto config=mem_read32(source+0x58),heap=mem_read32(context.r[13]-0x6DE0);
    const auto manager=mem_read32(0x802ABAAC);
    // The original allocator accepts a heap index (the level heap is 0).
    if(!readable(config,16) || !readable(manager,16))return 0;
    StackScratch scratch(context);auto call=context;call.msr&=~0x8000u;
    call.r[1]-=0x200;mem_write32(call.r[1],context.r[1]);
    call.r[3]=heap;call.r[4]=0xA8;ssne_call(call,0x80116084);
    const auto object=call.r[3];if(!readable(object,0xA8))return 0;
    mem_write32(context.r[13]-0x6DF0,mem_read32(context.r[13]-0x6DF0)-mem_read32(object-0x18));
    const auto owned_config=object+0x94;
    mem_write32(owned_config,mem_read32(config));
    // The map owns the Use relay. Its original one-shot/count rules apply
    // even when the first player boards an additional vehicle. A second
    // wrapper must not send the original vehicle's destruction notification.
    mem_write32(owned_config+4,mem_read32(config+4));
    mem_write16(owned_config+8,visible?1:0);mem_write16(owned_config+10,0);
    mem_write32(owned_config+12,UINT32_MAX);
    mem_write32(object+0xA4,spec.parent);
    call.r[3]=object;call.r[4]=owned_config;ssne_call(call,0x8008514C);
    const auto coordinates=call.r[1]+0x40;
    for(unsigned i=0;i<3;++i)mem_write32(coordinates+i*4,f2u(spec.parking[i]));
    call.r[3]=object;call.r[4]=coordinates;ssne_call(call,0x8009C300);
    mem_write32(object+0x1C,mem_read32(object+0x1C)|2);
    for(unsigned i=0;i<3;++i)mem_write32(object+0x2C+i*4,mem_read32(source+0x70+i*4));
    call.r[3]=manager;call.r[4]=object;ssne_call(call,0x801505F8);
    call.r[3]=object;call.r[4]=spec.reference;ssne_call(call,0x8007A244);
    // Dynamic wrappers join after the map's link-resolution pass. This is
    // the original resolver, including subscription to target destruction.
    call.r[3]=object;ssne_call(call,0x80085500u);
    if(native_script_reference_address(spec.reference)!=object || !mem_read32(object+0x60))
        ssne::fatal("Native cooperative vehicle failed to register");
    return object;
}
std::optional<VehiclePoint> parking_position(PPCContext& c,uint32_t source,const std::vector<VehiclePoint>& occupied){
    const auto origin=vehicle_point(source+0x64);
    unsigned rays=0,floor_hits=0;
    auto ground=[&](VehiclePoint p,float above,float below)->std::optional<float>{
        auto end=p;p[1]+=above;end[1]-=below;
        std::optional<ssne::NativeSegmentHit> hit;std::string error;
        ++rays;if(!ssne::native_world_segment(c,p,end,hit,error) || !hit || hit->normal[1]<.8f)return {};
        ++floor_hits;
        return hit->position[1];
    };
    const bool submarine=mem_read32(mem_read32(source+0x58))==3;
    auto base=submarine?std::optional<float>(origin[1]):ground(origin,1.f,4.f);
    if(!base)base=ground(origin,1.f,64.f);
    auto report=[&](){
        static unsigned emitted=0;
        if(getenv("SSNE_TEST_HIDDEN") && emitted++<10)ssne::log_message("[coop-vehicle-parking] source="+std::to_string(mem_read32(source+0x50))+" rays="+std::to_string(rays)+" supported="+std::to_string(floor_hits));
    };
    if(!base){report();return {};}
    constexpr float spacing=6.5f;
    for(unsigned ring=1;ring<=3;++ring)for(unsigned i=0;i<8;++i){
        const float angle=float(i)*.7853981634f;
        VehiclePoint p{origin[0]+ring*spacing*std::cos(angle),origin[1],origin[2]+ring*spacing*std::sin(angle)};
        if(std::any_of(occupied.begin(),occupied.end(),[&](const auto& v){
            return std::abs(p[1]-v[1])<4 && std::hypot(p[0]-v[0],p[2]-v[2])<spacing-.2f;
        }))continue;
        if(submarine){
            auto side=origin;
            side[0]+=4.f*std::cos(angle);side[2]+=4.f*std::sin(angle);
            ssne::NativeSphereResult sweep;std::string error;
            ++rays;
            if(ssne::native_world_sphere(c,side,p,1.8f,sweep,error) && sweep.contacts.empty() &&
               std::hypot(sweep.position[0]-p[0],sweep.position[2]-p[2])<.1f &&
               std::abs(sweep.position[1]-p[1])<.1f)return p;
            continue;
        }
        p[1]=std::clamp(origin[1],*base,*base+2.f);
        const auto floor=ground(p,2.f,5.f);if(!floor || std::abs(*floor-*base)>2.f)continue;
        auto start=origin,end=p;
        // The map vehicle itself has a collider. Check the route starting
        // at its outer edge, then check the entire destination footprint.
        start[0]+=3.5f*std::cos(angle);start[2]+=3.5f*std::sin(angle);
        start[1]=*base+1.1f;end[1]=*floor+1.1f;
        std::optional<ssne::NativeSegmentHit> hit;std::string error;
        if(!ssne::native_world_segment(c,start,end,hit,error) || hit)continue;
        bool clear=true;
        for(float x:{-1.7f,1.7f})for(float z:{-1.7f,1.7f}){
            VehiclePoint corner{p[0]+x,*floor,p[2]+z};
            const auto support=ground(corner,3.f,1.f);
            if(!support || std::abs(*support-*floor)>.65f){clear=false;continue;}
            corner[1]=*support+1.1f;
            if(!ssne::native_world_segment(c,end,corner,hit,error) || hit)clear=false;
        }
        if(clear){p[1]=*floor+std::clamp(origin[1]-*base,.05f,2.f);return p;}
    }
    report();return {};
}
const std::vector<int64_t>* fleet_data(const Properties& p){
    const auto it=p.find("Session.Vehicles.Fleet");
    return it==p.end()?nullptr:std::get_if<std::vector<int64_t>>(&it->second);
}
bool read_fleet(const Properties& p,std::vector<FleetVehicle>& out,std::string& error){
    const auto* data=fleet_data(p);
    if(!data && !p.contains("Session.Vehicles.Fleet"))return true;
    if(!data || data->size()>35 || data->size()%5){error="Invalid cooperative vehicle roster";return false;}
    std::set<unsigned> ids;unsigned missing=0;
    for(size_t k=0;k<data->size();k+=5){
        const auto* row=data->data()+k;
        if(row[0]<32761 || row[0]>32767 || row[1]<=0 || row[1]>32767 || !ids.insert(unsigned(row[0])).second){error="Invalid cooperative vehicle reference";return false;}
        const auto parent=native_script_reference_address(row[1]);
        if(!parent || !readable(*parent,0x94) || mem_read32(*parent+0x48)!=0x80209BB8u || party_vehicle(*parent)){error="Cooperative vehicle has no map vehicle";return false;}
        const auto origin=vehicle_point(*parent+0x64);FleetVehicle spec{unsigned(row[0]),unsigned(row[1])};
        for(unsigned i=0;i<3;++i){
            if(row[i+2]<0 || uint64_t(row[i+2])>UINT32_MAX){error="Invalid cooperative vehicle parking point";return false;}
            const auto x=u2f(uint32_t(row[i+2]));
            if(!std::isfinite(x) || std::abs(x-origin[i])>(i==1?66.f:20.f)){error="Cooperative vehicle parking point is outside its map location";return false;}
            spec.parking[i]=x;
        }
        const auto local=native_script_reference_address(spec.reference);
        if(!local){
            if(!vehicle_reference_space(0) || vehicle_reference_taken(spec.reference)){error="Cooperative vehicle reference is occupied";return false;}
            ++missing;
        }
        else if(!party_vehicle(*local) || mem_read32(*local+0xA4)!=spec.parent || vehicle_point(*local+0x64)!=spec.parking){error="Conflicting cooperative vehicle binding";return false;}
        out.push_back(spec);
    }
    if(missing && !vehicle_reference_space(missing)){error="No free level references for cooperative vehicles";return false;}
    return true;
}
