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
            const auto q=std::get<BotPoint>(actors[other]->properties.at("position"));const auto aim=q;
            if(bot_distance(p,q)>acquisition_range)continue;
            std::optional<ssne::NativeSegmentHit> hit;std::string why;
            if(ssne::native_world_segment(context,eye,aim,hit,why) && !hit)world.enemies.push_back({address,q,std::get<float>(actors[other]->properties.at("health")),0.f});
        }
    }
    // Regroup with a distant human before engaging remote enemies. Nearby
    // threats still permit self-defence while ordinary navigation continues.
    if(cooperative && leader!=actor && nearest>18.f)std::erase_if(world.enemies,[&](const auto& enemy){return bot_distance(p,enemy.position)>8.f;});
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
        if(distance>65)continue;
        const auto weapon=bot_weapon_pickup(item.definition);const bool has=weapon<owned.size() && owned[weapon]>0;
        auto value=bot_pickup_value(item.definition,health,armor,has,low_ammo);
        const bool urgent=(item.definition<=4 && health<40.f) || (weapon<12 && !has) || (low_ammo && item.definition>=17 && item.definition<=45);
        if(cooperative && leader!=actor && !bot_coop_pickup_allowed(nearest,distance,bot_distance(q,world.host.position),urgent))continue;
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
    bool goal_supported=swimming;
    if(radius && !swimming){
        auto above=goal,below=goal;above[1]+=1.f;below[1]-=3.f;std::optional<ssne::NativeSegmentHit> floor;std::string why;
        if(ssne::native_world_segment(context,above,below,floor,why) && floor && floor->normal[1]>=.7f){goal[1]=floor->position[1]+*radius/std::max(.725f,floor->normal[1])+.02f;goal_supported=true;}
    }
    std::array<unsigned,6> probe_stats{};float probe_rise=0.f;
    auto probe=[&](const BotPoint& from,const BotPoint& wanted)->std::optional<BotPoint>{
        if(!radius || bot_distance(from,wanted)>8.f){++probe_stats[0];return {};}
        auto above=wanted,below=wanted;above[1]+=1.f;below[1]-=2.f;
        std::optional<ssne::NativeSegmentHit> floor;std::string why;
        if(!ssne::native_world_segment(context,above,below,floor,why) || !floor || floor->normal[1]<.7f){++probe_stats[1];return {};}
        auto q=wanted;q[1]=floor->position[1]+*radius/std::max(.725f,floor->normal[1])+.02f;probe_rise=std::max(probe_rise,std::abs(q[1]-from[1]));
        const float distance=std::hypot(q[0]-from[0],q[2]-from[2]);
        ssne::NativeSphereResult sweep;
        bool fast=std::abs(q[1]-from[1])<=.6f && ssne::native_world_sphere(context,from,q,*radius,sweep,why) && bot_distance(sweep.position,q)<=.3f;
        // Flat, clear edges keep their cheap sweep and support samples.
        if(fast){
            const unsigned steps=std::max(1u,unsigned(std::ceil(distance/.5f)));float previous=from[1]-*radius;
            for(unsigned step=1;step<=steps;++step){
                const float t=float(step)/steps;for(unsigned i=0;i<3;++i)above[i]=from[i]+t*(q[i]-from[i]);below=above;above[1]+=.5f;below[1]-=2.f;
                if(!ssne::native_world_segment(context,above,below,floor,why) || !floor || floor->normal[1]<.725f || above[1]-.5f-floor->position[1]>*radius+.5f || floor->position[1]-previous>.6f || floor->position[1]-previous<-.5f){fast=false;break;}
                previous=floor->position[1];
            }
            if(fast){++probe_stats[5];return q;}
        }
        // A coarse search edge may cross several original small stairs. Follow
        // their actual floor heights, with at most32 short segments per edge.
        const unsigned steps=std::max(1u,unsigned(std::ceil(distance/.25f)));auto current=from;
        const float step_limit=std::min(.6f,*radius-.3f);
        for(unsigned step=1;step<=steps;++step){
            const float t=float(step)/steps;BotPoint next;
            for(unsigned i=0;i<3;++i)next[i]=from[i]+t*(q[i]-from[i]);above=below=next;above[1]+=.75f;below[1]-=2.f;
            if(!ssne::native_world_segment(context,above,below,floor,why) || !floor || floor->normal[1]<.725f){++probe_stats[1];return {};}
            next[1]=floor->position[1]+*radius/std::max(.725f,floor->normal[1])+.02f;const float rise=next[1]-current[1];
            if(rise>step_limit+.02f || rise<-.5f){++probe_stats[2];return {};}
            const bool queried=walk_sweep(current,next,*radius,sweep,why);
            bool resolved_step=false;
            if(queried && bot_distance(sweep.position,next)>.15f){
                const float dx=sweep.position[0]-current[0],dz=sweep.position[2]-current[2],wx=next[0]-current[0],wz=next[2]-current[2];
                const float travel=std::hypot(dx,dz),wanted_travel=std::hypot(wx,wz);
                if(travel>.025f && travel<=wanted_travel+.05f && dx*wx+dz*wz>0.f && std::abs(sweep.position[1]-current[1])<=step_limit){
                    auto support_above=sweep.position,support_below=sweep.position;support_above[1]+=.5f;support_below[1]-=2.f;
                    std::optional<ssne::NativeSegmentHit> support;
                    resolved_step=ssne::native_world_segment(context,support_above,support_below,support,why) && support && support->normal[1]>=.725f && sweep.position[1]-support->position[1]>=*radius-.05f && sweep.position[1]-support->position[1]<=*radius+.6f;
                }
            }
            if(!queried || (bot_distance(sweep.position,next)>.15f && !resolved_step)){
                ++probe_stats[3];
                if(log_ && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES") && afk_decisions_[slot]%8==0 && probe_stats[3]<=2){
                    auto point=[](const BotPoint& v){return std::to_string(v[0])+","+std::to_string(v[1])+","+std::to_string(v[2]);};
                    log_("[game-bot-probe-reject] slot="+std::to_string(slot)+" from="+point(current)+" wanted="+point(next)+" corrected="+point(sweep.position)+" normal_y="+std::to_string(floor->normal[1])+" rise="+std::to_string(rise)+" radius="+std::to_string(*radius));
                }
                return {};
            }
            current=resolved_step?sweep.position:next;
        }
        ++probe_stats[5];return current;
    };
    if(goal_supported && (swimming || bot_distance(p,goal)<2.f || route_clear(goal)))world.host.position=goal;
    else world.host.position=brain.navigation.step(p,goal,probe,48,goal_supported);
    world.host.id=native_actor;world.host.health=health;
    if(log_ && env_requested("SSNE_TEST_HIDDEN") && env_requested("SSNE_COOP_TRACE_LIFETIMES") && afk_decisions_[slot]%8==0){
        const auto nav=brain.navigation.state();auto point=[](const BotPoint& v){return std::to_string(v[0])+","+std::to_string(v[1])+","+std::to_string(v[2]);};
        log_("[game-bot-navigation] slot="+std::to_string(slot)+" position="+point(p)+" goal="+point(goal)+" waypoint="+point(world.host.position)+" nodes="+std::to_string(nav.nodes)+" open="+std::to_string(nav.open)+" path="+std::to_string(nav.path)+" cursor="+std::to_string(nav.cursor)+" best="+std::to_string(nav.best)+" tip="+std::to_string(nav.tip)+" age="+std::to_string(nav.age)+" restarts="+std::to_string(nav.restarts)+" solved="+std::to_string(nav.solved)+" root="+point(nav.root)+" next="+point(nav.next)+" frontier="+point(nav.frontier)+" probe_guard="+std::to_string(probe_stats[0])+" probe_floor="+std::to_string(probe_stats[1])+" probe_height="+std::to_string(probe_stats[2])+" probe_sweep="+std::to_string(probe_stats[3])+" probe_support="+std::to_string(probe_stats[4])+" probe_ok="+std::to_string(probe_stats[5])+" max_rise="+std::to_string(probe_rise));
    }
    if(log_ && afk_decisions_[slot]%16==0)log_("[game-bot-goal] slot="+std::to_string(slot)+" mode="+std::to_string(sample.mode)+" pickup="+std::to_string(pickup)+" goal="+std::to_string(goal[0])+","+std::to_string(goal[1])+","+std::to_string(goal[2])+" nodes="+std::to_string(brain.navigation.explored()));
}
