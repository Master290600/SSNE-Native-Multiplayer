// Host movement parameters and velocity are advisory for the local view.
constexpr char movement_key[]="Session.OnFootMotion";
constexpr unsigned motion_fields=10;
// PAL 800C09F0..800C0A1C bounds an automatic step by the player's
// sphere radius minus the original clearance (0.30 for the PAL player).
float walking_step_limit(float radius){
    return radius-u2f(mem_read32(0x802DA9DCu));
}

bool plausible_tuning(const MovementTuning& p){
    return p.ground_acceleration>0 && p.ground_acceleration<=1600 &&
           p.ground_decay>=.1f && p.ground_decay<=60 &&
           p.air_acceleration>0 && p.air_acceleration<=1600 &&
           p.air_decay>=.1f && p.air_decay<=60 &&
           p.jump_height>.1f && p.jump_height<=12 &&
           p.jump_duration>=.12f && p.jump_duration<=2;
}

float movement_parameter(uint32_t actor,unsigned field){
    const auto pointer=mem_read32(actor+field);
    return presentation_readable(pointer,0x28)?u2f(mem_read32(pointer+0x24)):0.f;
}

bool static_movement_contact(const ssne::NativeSegmentHit& hit){
    return (hit.source_kind==0 || hit.source_kind==4) && !(hit.flags&0x80000000u);
}

// A sphere can gradually climb the side of a barrier in several small
// sweeps. Measure the height of the entire tread above the approach
// floor, independently of the current (possibly lifted) sphere height.
bool walkable_rim(PPCContext& context,const ssne::NativeSegmentHit& contact,float max_walking_step){
    std::string error;
    const float flat=std::hypot(contact.normal[0],contact.normal[2]);
    if(flat<.01f)return false;
    auto floor_at=[&](float offset,float depth)->std::optional<float>{
        auto above=contact.position;
        above[0]+=offset*contact.normal[0]/flat;
        above[2]+=offset*contact.normal[2]/flat;
        above[1]+=.035f;
        auto below=above;below[1]-=depth+.035f;
        std::optional<ssne::NativeSegmentHit> floor;
        if(!ssne::native_world_segment(context,above,below,floor,error) || !floor ||
           !static_movement_contact(*floor) || floor->normal[1]<.725f)return {};
        // Compare both support planes at the actual edge X/Z.
        return floor->position[1]-(floor->normal[0]*(contact.position[0]-above[0])+
                                   floor->normal[2]*(contact.position[2]-above[2]))/floor->normal[1];
    };
    const auto upper=floor_at(.025f,.035f);
    if(!upper || std::abs(*upper-contact.position[1])>.025f)return false;
    const auto lower=floor_at(-.025f,max_walking_step+.035f);
    return lower && *upper-*lower>.008f && *upper-*lower<max_walking_step;
}

bool supported_walking_step(PPCContext& context,uint32_t actor,const MovementPoint& velocity,float radius){
    const float max_walking_step=walking_step_limit(radius);
    const float speed=std::hypot(velocity[0],velocity[2]);
    if(speed<.06f)return false;
    MovementPoint centre;
    for(unsigned i=0;i<3;++i)centre[i]=u2f(mem_read32(actor+0x20+4*i));
    auto support=[&](float distance)->std::optional<float>{
        auto from=centre;
        from[0]+=distance*velocity[0]/speed;
        from[2]+=distance*velocity[2]/speed;
        auto to=from;to[1]-=radius+max_walking_step+.04f;
        std::optional<ssne::NativeSegmentHit> hit;
        std::string error;
        if(!ssne::native_world_segment(context,from,to,hit,error) || !hit ||
           !static_movement_contact(*hit) || hit->normal[1]<.725f)return {};
        // Evaluate both planes at the same X/Z. Two points on a continuous
        // incline have equal plane heights even when the ray hits differ.
        return hit->position[1]-(hit->normal[0]*(centre[0]-hit->position[0])+
                                hit->normal[2]*(centre[2]-hit->position[2]))/hit->normal[1];
    };
    const auto behind=support(-radius),ahead=support(radius);
    if(behind && ahead){
        const float drop=*behind-*ahead;
        if(drop>.04f && drop<max_walking_step)return true;
    }

    // PAL may mark the capsule as sliding on either side of its step
    // animation. Test the actual rim in both travel directions, including
    // convex corners that the two longitudinal floor probes can miss.
    auto above=centre,below=centre;
    above[1]+=.04f;below[1]-=.06f;
    ssne::NativeSphereResult sweep;
    std::string error;
    if(!ssne::native_world_sphere(context,above,below,radius,sweep,error))return false;
    for(const auto& c:sweep.contacts){
        if(!static_movement_contact(c) || c.normal[1]>=0.f)continue;
        if(walkable_rim(context,c,max_walking_step))return true;
    }
    return false;
}

bool resting_on_landing(PPCContext& context,uint32_t actor,float radius){
    MovementPoint centre;
    for(unsigned i=0;i<3;++i)centre[i]=u2f(mem_read32(actor+0x20+4*i));
    // PAL's ground resolver offsets the support probe by 0.10 (800C0D74).
    // The 0.01 contact tolerance also covers its small separation at rest.
    const float contact_tolerance=u2f(mem_read32(0x80212F4Cu));
    const float clearance=u2f(mem_read32(0x80212F5Cu))+contact_tolerance;
    auto below=centre;
    below[1]-=radius+clearance+.001f;
    std::optional<ssne::NativeSegmentHit> floor;
    std::string error;
    return ssne::native_world_segment(context,centre,below,floor,error) && floor &&
           static_movement_contact(*floor) && floor->normal[1]>.999f &&
           floor->position[1]<=centre[1]-radius+contact_tolerance;
}

void capture_on_foot_motion(PPCContext& context,NativeWorld& native,const NativeWorldSample& sample,Properties& mission){
    std::vector<float> pack(max_players*motion_fields);
    for(const auto& [id,entity]:sample.entities){
        if(entity.kind!=EntityKind::Player || entity.owner.slot>=max_players)continue;
        const auto actor=native.local_address(id);
        if(presentation_readable(actor,0x790) && mem_read32(actor+0x340)==2){
            const auto controller=mem_read32(actor+0x344);
            if(!presentation_readable(controller,0x20) || mem_read32(controller)!=0x80210FB0u ||
               mem_read32(controller+4)>2 || mem_read32(actor+0x328) || mem_read32(actor+0x370) ||
               ssne::native_party_spawn_waiting(actor))continue;
            // PAL 800A9ED4 uses separate forward, sideways and upward swim
            // speeds and water drag. Carry them in the existing per-slot row.
            auto* row=pack.data()+entity.owner.slot*motion_fields;
            row[0]=mem_read32(controller+4)==2?4.f:3.f;
            for(unsigned axis=0;axis<3;++axis)row[axis+1]=u2f(mem_read32(actor+0x298+axis*4));
            row[4]=movement_parameter(actor,0x1F8);
            row[5]=movement_parameter(actor,0x1FC);
            row[6]=movement_parameter(actor,0x200);
            row[7]=u2f(mem_read32(0x802DA920u));
            continue;
        }
        const auto radius=ssne::native_player_collision_radius(actor);
        if(!radius || mem_read32(actor+0x340)!=1 ||
           mem_read32(actor+0x328) || mem_read32(actor+0x370) || mem_read32(actor+0x330) ||
           mem_read32(actor+0x704) || mem_read32(actor+0x6FC) ||
           ssne::native_party_spawn_waiting(actor))continue;
        const auto controller=mem_read32(actor+0x344);
        if(!presentation_readable(controller,0x38))continue;
        const uint32_t phase=mem_read32(controller+4);
        if(phase==2 || phase>3)continue;
        // PAL can mark one tick as sliding and clear ground contact when the
        // walking controller lifts the capsule onto a curb. Preserve this
        // supported step while leaving ordinary sliding under host control.
        const float step_height=u2f(mem_read32(actor+0x708));
        MovementPoint velocity;
        for(unsigned axis=0;axis<3;++axis)velocity[axis]=u2f(mem_read32(actor+0x298+axis*4));
        bool stepping=phase==0 && step_height>0.f && step_height<walking_step_limit(*radius);
        if(mem_read32(actor+0x6E4) && !stepping){
            if(phase!=0)continue;
            if(std::isfinite(velocity[1]) && velocity[1]<=0.f &&
               std::hypot(velocity[0],velocity[2])<.06f && resting_on_landing(context,actor,*radius)){
                // Gravity can accumulate while the capsule rests beside the
                // stair rail. Only the replicated visual motion uses zero Y.
                velocity[1]=0.f;
            }else if(!supported_walking_step(context,actor,velocity,*radius))continue;
            stepping=true;
        }
        if(!(movement_length(velocity)<160.f))continue;
        const bool grounded=phase==0 && (mem_read32(actor+0x6D0) || stepping);

        const auto powers=entity.properties.find("powerups");
        const auto* powerups=powers==entity.properties.end()?nullptr:std::get_if<std::vector<float>>(&powers->second);
        const bool skates=powerups && !powerups->empty() && (*powerups)[0]!=0.f;
        const auto combo=mem_read32(actor+0x3AC);
        float acceleration=movement_parameter(actor,skates?0x1E8:0x1E0);
        if(combo==4 || combo==5)acceleration*=movement_parameter(actor,0x204);
        const MovementTuning tune{acceleration,movement_parameter(actor,skates?0x1EC:0x1E4),
            movement_parameter(actor,0x1F0),movement_parameter(actor,0x1F4),
            movement_parameter(actor,0x20C),movement_parameter(actor,0x214)};
        if(!plausible_tuning(tune))continue;

        const auto offset=entity.owner.slot*motion_fields;
        pack[offset]=grounded?1.f:2.f;
        for(unsigned axis=0;axis<3;++axis)pack[offset+axis+1]=velocity[axis];
        pack[offset+4]=tune.ground_acceleration;
        pack[offset+5]=tune.ground_decay;
        pack[offset+6]=tune.air_acceleration;
        pack[offset+7]=tune.air_decay;
        pack[offset+8]=tune.jump_height;
        pack[offset+9]=tune.jump_duration;
    }
    mission[movement_key]=std::move(pack);
}

std::optional<MovementContact> local_static_step(PPCContext& context,const MovementPoint& from,
        const MovementPoint& desired,bool walking,float radius){
    const float max_walking_step=walking_step_limit(radius);
    std::string error;
    auto sweep=[&](const MovementPoint& start,const MovementPoint& target,ssne::NativeSphereResult& hit){
        if(!ssne::native_world_sphere(context,start,target,radius,hit,error))return false;
        for(const auto& contact:hit.contacts)
            if(contact.source_kind!=3 && !static_movement_contact(contact))return false;
        return movement_length(movement_difference(hit.position,start))<=
               movement_length(movement_difference(target,start))+.20f;
    };
    // PAL's segment queries can hit decorative stair treads below the actual
    // player ramp. Read support from the same swept sphere as locomotion.
    // Sphere contact normals point FROM the centre INTO the floor.
    auto support_height=[&](const ssne::NativeSphereResult& hit,const MovementPoint& at){
        std::optional<float> highest;
        for(const auto& c:hit.contacts){
            if(!static_movement_contact(c) || c.normal[1]>-.725f)continue;
            const float height=c.position[1]-(radius+c.normal[0]*(at[0]-c.position[0])+
                                              c.normal[2]*(at[2]-c.position[2]))/c.normal[1];
            if(!highest || height>*highest)highest=height;
        }
        return highest;
    };
    // A downward sweep slides along an incline. Use its contact plane to
    // settle vertically at the requested X/Z instead of adding downhill drift.
    auto settle=[&](const MovementPoint& at,float drop)->std::optional<MovementContact>{
        auto down=at;
        down[1]-=drop;
        ssne::NativeSphereResult probe;
        if(!sweep(at,down,probe))return {};
        if(walking){
            // A native downward sweep may first touch a convex rim, then
            // slide backwards onto the lower lawn. Use the first surface
            // reached on this vertical line, before the lateral slide.
            std::optional<float> first;
            for(const auto& c:probe.contacts){
                if(!static_movement_contact(c) || -radius*c.normal[1]<=radius-max_walking_step)continue;
                const float x=c.position[0]-radius*c.normal[0];
                const float z=c.position[2]-radius*c.normal[2];
                const float y=c.position[1]-radius*c.normal[1];
                if(std::hypot(x-at[0],z-at[2])>.009f || y<down[1]-.005f || y>at[1]+.005f)continue;
                if(c.normal[1]>-.725f && !walkable_rim(context,c,max_walking_step))continue;
                if(!first || y>*first)first=y;
            }
            if(first){down[1]=*first;return MovementContact{down,true};}
        }
        auto height=support_height(probe,at);
        if(!height || *height<down[1]-.005f || *height>at[1]+.04f)
            return MovementContact{at,false};
        down[1]=*height;
        if(std::abs(down[1]-at[1])<=.002f)return MovementContact{at,true};
        ssne::NativeSphereResult landing;
        if(!sweep(at,down,landing))return {};
        return MovementContact{landing.position,true};
    };

    ssne::NativeSphereResult motion;
    motion.position=from;
    if(desired!=from && !sweep(from,desired,motion))return {};
    const auto& corrected=motion.position;
    if(walking){
        // PAL first resolves the horizontal sweep, then the walking controller
        // settles the centre with a distinct vertical sphere cast. Lifting by
        // the maximum step height first can carry the view over a retaining
        // edge even while the host's sphere is still sliding along its side.
        const bool meets_rim=std::any_of(motion.contacts.begin(),motion.contacts.end(),[&](const auto& c){
            return static_movement_contact(c) && c.normal[1]<0.f &&
                   walkable_rim(context,c,max_walking_step);
        });
        if(meets_rim){
            std::optional<ssne::NativeSegmentHit> support;
            if(!ssne::native_ground_support(context,corrected,radius,support,error))return {};
            if(support)return MovementContact{support->position,true};
        }
    }
    if(!walking && desired[1]>from[1])return MovementContact{corrected,false};
    const auto floor=support_height(motion,corrected);
    if(floor && std::abs(corrected[1]-*floor)<.04f)return MovementContact{corrected,true};
    return settle(corrected,walking?max_walking_step:.045f);
}

MovementIntent on_foot_intent(const LocalControlSample& controls,float yaw){
    // The GameCube stick discards 15 counts, with full input at 72 after
    // clamping. Normalize diagonals to the same speed as a single direction.
    auto axis=[](uint8_t value){
        const int centred=int(value)-128;
        return std::copysign(float(std::clamp(std::abs(centred)-15,0,72))/72.f,float(centred));
    };
    float right=axis(controls.input.pad.stick_x),forward=axis(controls.input.pad.stick_y);
    const float length=std::max(1.f,std::hypot(right,forward));
    return {controls.sequence,controls.jump_pressed,right/length,forward/length,yaw};
}

// PAL's swim controller (800AAA90) queries the two points above the
// capsule against the level's medium BSP. The waterline varies per pool.
std::optional<int> local_medium(PPCContext& context,const MovementPoint& position){
    const auto level=mem_read32(0x802DACC0u);
    if(!presentation_readable(level,0x29C) || context.r[13]!=0x802E22A0u ||
       context.r[2]!=0x802F22A0u || context.r[1]<0x80001000u ||
       !presentation_readable(context.r[1]-0x600,0x604))return {};
    const auto manager=mem_read32(level+0x298);
    if(!presentation_readable(manager,0xB5C) || mem_read32(manager)!=0x8021E660u ||
       !ssne_find_func(0x8013BF44u))return {};
    const auto vertex=context.r[1]-0x60;
    std::array<uint8_t,0x600> saved;
    auto* scratch=ssne::host_ptr(context.r[1]-uint32_t(saved.size()));
    std::memcpy(saved.data(),scratch,saved.size());
    struct RestoreStack{
        uint8_t* scratch;const std::array<uint8_t,0x600>& saved;
        ~RestoreStack(){std::memcpy(scratch,saved.data(),saved.size());}
    } restore{scratch,saved};
    mem_write32(vertex,f2u(position[0]));
    mem_write32(vertex+4,f2u(position[1]));
    mem_write32(vertex+8,f2u(position[2]));
    PPCContext call=context;
    call.msr&=~0x8000u;
    call.r[1]-=0x100;
    mem_write32(call.r[1],context.r[1]);
    call.r[3]=manager;call.r[4]=vertex;call.r[5]=0;
    ssne_call(call,0x8013BF44u);
    if(!call.r[3])return -1;
    if(!presentation_readable(call.r[3],5))return {};
    return mem_read8(call.r[3]+4);
}

std::optional<bool> at_swim_surface(PPCContext& context,const MovementPoint& position){
    auto probe=position;
    probe[1]+=u2f(mem_read32(context.r[13]-0x797C));
    const auto top=local_medium(context,probe);
    if(!top)return {};
    if(*top!=0)return false;
    probe[1]=position[1]+u2f(mem_read32(context.r[13]-0x7978));
    const auto lower=local_medium(context,probe);
    if(!lower)return {};
    return *lower<=0;
}

std::optional<MovementContact> local_swim_step(PPCContext& context,const MovementPoint& from,
        const MovementPoint& to,float radius,std::optional<float> confirmed_surface){
    if(from==to)return MovementContact{from,false};
    ssne::NativeSphereResult water;
    std::string error;
    if(!ssne::native_world_sphere(context,from,to,radius,water,error))return {};
    auto& point=water.position;
    if(point[1]>from[1]){
        auto above_limit=[&]()->std::optional<bool>{
            // PAL can already be floating slightly above the point at which
            // the controller first entered its surface phase. Accept its
            // confirmed height only while the capsule centre is in local water.
            if(confirmed_surface && point[1]<=*confirmed_surface && local_medium(context,point)==1)
                return false;
            return at_swim_surface(context,point);
        };
        auto surface=above_limit();
        if(!surface)return {};
        if(*surface){
            float lo=from[1],hi=point[1];
            // Preserve the horizontal sweep while finding the local surface
            // at the destination X/Z, including along a sloping shore.
            for(unsigned i=0;i<12 && hi-lo>.002f;++i){
                point[1]=(lo+hi)*.5f;
                if(point[1]==lo || point[1]==hi)break;
                surface=above_limit();
                if(!surface)return {};
                if(*surface)hi=point[1];else lo=point[1];
            }
            point[1]=lo;
        }
    }
    return MovementContact{point,false};
}

std::optional<WeaponGaitInput> render_local_movement(PPCContext& context,LocalMovement& motion,const WorldState& world,
        const EntityState& own,EntityState& visual,uint32_t actor,const LocalControlSample& controls,
        double now,float yaw,float pitch){
    const auto meta=world.mission.find(movement_key);
    const auto* pack=meta==world.mission.end()?nullptr:std::get_if<std::vector<float>>(&meta->second);
    const auto position=own.properties.find("position");
    const auto frame_position=visual.properties.find("position");
    const auto* anchor_pos=position==own.properties.end()?nullptr:std::get_if<MovementPoint>(&position->second);
    const auto* shown=frame_position==visual.properties.end()?nullptr:std::get_if<MovementPoint>(&frame_position->second);
    const auto radius=ssne::native_player_collision_radius(actor);
    if(!pack || pack->size()!=max_players*motion_fields || !anchor_pos || !shown || !radius ||
       controls.epoch!=world.epoch || own.owner.slot>=max_players){
        motion.reset();
        return {};
    }
    const auto slot=own.owner.slot;
    const float* row=pack->data()+slot*motion_fields;
    const MovementTuning tune{row[4],row[5],row[6],row[7],row[8],row[9]};
    const MovementPoint velocity{row[1],row[2],row[3]};
    const bool swimming=row[0]==3.f || row[0]==4.f;
    const bool valid_swim=row[4]>0 && row[4]<=50 && row[5]>0 && row[5]<=50 &&
                          row[6]>=0 && row[6]<=100 && row[7]>.01f && row[7]<=60;
    if((swimming?!valid_swim:((row[0]!=1.f && row[0]!=2.f) || !plausible_tuning(tune))) ||
       !(movement_length(velocity)<160.f)){
        motion.reset();
        return {};
    }
    const MovementAnchor anchor{*anchor_pos,velocity,world.input_ack[slot],row[0]==1.f};
    // If polling stalls, stop accelerating on old input. Retain the local
    // capsule and its unacknowledged steps until fresh controls arrive.
    const bool fresh=now-controls.sampled_at<=.25;
    const auto intent=fresh?on_foot_intent(controls,yaw):MovementIntent{controls.sequence,0,0,0,yaw};
    SwimInput swim;
    swim.capsule_diameter=2.f * (*radius);
    {
        swim.surface=row[0]==4.f;
        if(swim.surface && (intent.right<0 || pitch<.52359879f))pitch=0;
        const float forward=(swimming?row[4]:movement_parameter(actor,0x1F8))*intent.forward;
        const float side=(swimming?row[5]:movement_parameter(actor,0x1FC))*intent.right;
        const float upwards=swimming?row[6]:movement_parameter(actor,0x200);
        const float flat=forward*std::cos(pitch);
        const bool ascend=fresh && ((controls.input.pad.buttons&ssne::PAD_L) || controls.input.pad.trigger_l>=128);
        swim.acceleration={flat*std::sin(yaw)+side*std::cos(yaw),
                           -forward*std::sin(pitch)+(ascend && !swim.surface?upwards:0.f),
                           flat*std::cos(yaw)-side*std::sin(yaw)};
        swim.drag=swimming?row[7]:u2f(mem_read32(0x802DA920u));
    }
    const auto predicted=motion.sample(now,anchor,intent,tune,
        [&](const MovementPoint& from,const MovementPoint& to,bool walking)->std::optional<MovementContact>{
            if(swimming || motion.entering_water()){
                const auto surface=swim.surface?std::optional<float>{anchor.position[1]}:std::nullopt;
                return local_swim_step(context,from,to,*radius,surface);
            }
            auto step=local_static_step(context,from,to,walking,*radius);
            if(step && step->position!=from)
                step->entered_water=local_medium(context,step->position)==1;
            return step;
        },swimming?&swim:nullptr,&swim);
    const WeaponGaitInput gait{std::hypot(intent.right,intent.forward),
                               motion.falling(!anchor.grounded && velocity[1]<0.f)};
    if(!predicted)return gait;

    const auto offset=movement_difference(*predicted,*shown);
    for(const char* prefix:{"camera_row","weapon_model0_row","weapon_model1_row"}){
        PresentationMatrix matrix;
        if(!presentation_matrix(visual.properties,prefix,matrix))continue;
        for(unsigned axis=0;axis<3;++axis)matrix[axis*4+3]+=offset[axis];
        put_presentation_matrix(visual.properties,prefix,matrix);
    }
    return gait;
}

