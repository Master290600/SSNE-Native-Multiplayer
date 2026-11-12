// Runs only on the authoritative guest thread. Goals never write positions,
// inventory, damage or objective ownership: the original game handles these.
const bool automated=(server_->bot_mask()&(1u<<slot))!=0;
const bool cooperative=sample.mode==1;
auto& brain=game_bots_[slot];
if(brain.player!=controls[slot].id){brain={};brain.player=controls[slot].id;afk_bots_[slot]={};}
const unsigned skill=automated?server_->bot_skill(slot):2;
if(automated){
    if(bot_distance(p,brain.goal)>2 && bot_distance(p,brain.previous)<.25f)++brain.goal_age;else brain.goal_age=0;
    if(brain.pickup && brain.goal_age>192){brain.deferred[brain.pickup]=afk_decisions_[slot]+240;brain.pickup=0;brain.goal_age=0;brain.navigation={};}
    brain.previous=p;
    if(!cooperative){
        world.enemies.clear();
        for(unsigned other=0;other<capacity_;++other)if(other!=slot && actors[other] && controls[other].ready){
            const auto address=native_.local_address(actors[other]->id);
            if(mem_read32(address+0x328) || !(std::get<float>(actors[other]->properties.at("health"))>0))continue;
            const auto q=std::get<BotPoint>(actors[other]->properties.at("position"));auto aim=q;aim[1]+=.8f;
            if(bot_distance(p,q)>acquisition_range)continue;
            std::optional<ssne::NativeSegmentHit> hit;std::string why;
            if(ssne::native_world_segment(context,eye,aim,hit,why) && !hit)world.enemies.push_back({address,q,std::get<float>(actors[other]->properties.at("health")),1.6f});
        }
    }
    BotPoint goal=world.host.position;float best=0;EntityId pickup=0;
    const auto& owned=std::get<std::vector<int64_t>>(actor->properties.at("weapon_owned_counts"));
    const auto& ammo=std::get<std::vector<int64_t>>(actor->properties.at("ammo"));
    const auto& maximum=std::get<std::vector<int64_t>>(actor->properties.at("ammo_maximum"));
    bool low_ammo=false;
    for(unsigned weapon=0;weapon<owned.size() && weapon<12;++weapon)if(owned[weapon]){
        const auto instance=mem_read32(native_actor+0x4BC+weapon*24);
        if(instance<0x80000000u || instance>ssne::guest_ram_end()-0x1D4)continue;
        const auto config=mem_read32(instance+0x1D0);if(config<0x80000000u || config>ssne::guest_ram_end()-60)continue;
        const auto index=mem_read32(config+0x38);if(index<ammo.size() && index<maximum.size() && maximum[index]>0 && ammo[index]*4<maximum[index])low_ammo=true;
    }
    const float health=world.client.health,armor=std::get<float>(actor->properties.at("armor"));
    for(const auto& [id,item]:sample.entities)if(item.kind==EntityKind::Pickup){
        if(brain.deferred.contains(id) && brain.deferred[id]>afk_decisions_[slot])continue;
        const auto flags=item.properties.find("pickup_ownership");
        if(flags==item.properties.end())continue;
        const auto& collected=std::get<std::vector<int64_t>>(flags->second);if(slot>=collected.size() || collected[slot])continue;
        if(auto visible=item.properties.find("render_visible");visible!=item.properties.end() && !std::get<int64_t>(visible->second))continue;
        const auto q=std::get<BotPoint>(item.properties.at("position"));const auto distance=bot_distance(p,q);
        if(distance>65 || (cooperative && leader!=actor && bot_distance(q,world.host.position)>20))continue;
        const auto weapon=bot_weapon_pickup(item.definition);const bool has=weapon<owned.size() && owned[weapon]>0;
        auto value=bot_pickup_value(item.definition,health,armor,has,low_ammo);
        // Leave nearby first-aid to a more injured human teammate.
        if(cooperative && item.definition<=4)for(unsigned human=0;human<capacity_;++human)if(live_humans[human] && actors[human] && std::get<float>(actors[human]->properties.at("health"))<health && bot_distance(std::get<BotPoint>(actors[human]->properties.at("position")),q)<5)value=0;
        const auto score=value/(1.f+distance*.12f)*(brain.pickup==id?1.2f:1.f);
        if(score>best){best=score;goal=q;pickup=id;}
    }
    const ssne::BotActor* opponent=nullptr;float nearest_enemy=1e9f;
    for(const auto& enemy:world.enemies){const auto d=bot_distance(p,enemy.position);if(d<nearest_enemy){nearest_enemy=d;opponent=&enemy;}}
    if(!cooperative){
        if(opponent && best<25){
            goal=opponent->position;pickup=0;
            const float distance=std::hypot(p[0]-goal[0],p[2]-goal[2]);
            const unsigned selected=mem_read32(native_actor+0x5DC);const float preferred=selected==0?1.f:selected==7?5.f:selected==3?6.f:14.f;
            if(distance>.1f){const float angle=(slot%2?1.f:-1.f)*.35f;const float x=(p[0]-goal[0])/distance,z=(p[2]-goal[2])/distance;
                goal[0]+=preferred*(x*std::cos(angle)-z*std::sin(angle));goal[2]+=preferred*(x*std::sin(angle)+z*std::cos(angle));}
        }
        if(!opponent && !pickup){
            // Patrol actual map pickup anchors; phase offsets spread bots out.
            std::vector<BotPoint> anchors;for(const auto& [id,item]:sample.entities)if(item.kind==EntityKind::Pickup)anchors.push_back(std::get<BotPoint>(item.properties.at("position")));
            if(!anchors.empty())goal=anchors[(afk_decisions_[slot]/80+slot*7)%anchors.size()];
        }
        unsigned carrier=max_players;for(unsigned other=0;other<capacity_;++other)if(actors[other] && mem_read32(native_.local_address(actors[other]->id)+(sample.mode==3?0x694:0x6A4)))carrier=other;
        if(sample.mode==3){
            if(carrier<capacity_ && carrier!=slot){goal=std::get<BotPoint>(actors[carrier]->properties.at("position"));pickup=0;}
            else if(carrier==slot && opponent && best<35){goal=p;const float d=std::max(1.f,nearest_enemy);goal[0]+=(p[0]-opponent->position[0])*14/d;goal[2]+=(p[2]-opponent->position[2])*14/d;pickup=0;}
            else if(carrier==max_players)for(const auto& item:sample.unmapped)if(item.vtable==0x80206C80u){for(unsigned i=0;i<3;++i)goal[i]=u2f(mem_read32(item.address+0x20+i*4));pickup=0;break;}
        }else if(sample.mode==4 && carrier<capacity_){
            if(carrier==slot){
                float closest=1e9f;for(unsigned other=0;other<capacity_;++other)if(other!=slot && actors[other] && std::get<float>(actors[other]->properties.at("health"))>0){auto q=std::get<BotPoint>(actors[other]->properties.at("position"));const auto d=bot_distance(p,q);if(d<closest){closest=d;goal=q;}}
            }else{const auto q=std::get<BotPoint>(actors[carrier]->properties.at("position"));const auto d=bot_distance(p,q);if(d<25){goal=p;goal[0]+=(p[0]-q[0])*18/std::max(1.f,d);goal[2]+=(p[2]-q[2])*18/std::max(1.f,d);}}
            pickup=0;
        }
    }
    if(cooperative && !pickup && bot_distance(p,goal)<3.5f)goal=p;
    brain.pickup=pickup;brain.goal=goal;
    const auto radius=ssne::native_player_collision_radius(native_actor);
    if(radius && !swimming){
        auto above=goal,below=goal;above[1]+=1.f;below[1]-=3.f;std::optional<ssne::NativeSegmentHit> floor;std::string why;
        if(ssne::native_world_segment(context,above,below,floor,why) && floor && floor->normal[1]>=.7f)goal[1]=floor->position[1]+*radius+.02f;
    }
    auto probe=[&](const BotPoint& from,const BotPoint& wanted)->std::optional<BotPoint>{
        if(!radius)return {};
        auto above=wanted,below=wanted;above[1]+=1.f;below[1]-=2.f;
        std::optional<ssne::NativeSegmentHit> floor;std::string why;
        if(!ssne::native_world_segment(context,above,below,floor,why) || !floor || floor->normal[1]<.7f)return {};
        auto q=wanted;q[1]=floor->position[1]+*radius+.02f;if(std::abs(q[1]-from[1])>.6f)return {};
        ssne::NativeSphereResult sweep;
        if(!ssne::native_world_sphere(context,from,q,*radius,sweep,why) || bot_distance(sweep.position,q)>.3f)return {};
        // The midpoint prevents a valid landing from concealing a chasm.
        above=from;for(unsigned i=0;i<3;++i)above[i]=(from[i]+q[i])*.5f;below=above;above[1]+=.5f;below[1]-=2.f;
        if(!ssne::native_world_segment(context,above,below,floor,why) || !floor || floor->normal[1]<.7f || std::abs(floor->position[1]+*radius-q[1])>.6f)return {};
        return q;
    };
    if(swimming || bot_distance(p,goal)<2.f || route_clear(goal))world.host.position=goal;
    else world.host.position=brain.navigation.step(p,goal,probe,48);
    world.host.id=native_actor;world.host.health=health;
    if(log_ && afk_decisions_[slot]%16==0)log_("[game-bot-goal] slot="+std::to_string(slot)+" mode="+std::to_string(sample.mode)+" pickup="+std::to_string(pickup)+" goal="+std::to_string(goal[0])+","+std::to_string(goal[1])+","+std::to_string(goal[2])+" nodes="+std::to_string(brain.navigation.explored()));
}
