#include "client_presentation.h"
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
using namespace port::coop;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::vector<float> pose(unsigned count,float bias){
    std::vector<float> v(count*12);for(unsigned i=0;i<v.size();++i)v[i]=(float((i*37)%601)-300.f)/17.f+bias;
    if(!v.empty())v[0]=std::bit_cast<float>(uint32_t(0x80000000));
    if(v.size()>1)v[1]=std::bit_cast<float>(uint32_t(1));return v;
}
std::vector<float> reference(const std::vector<float>& a,const std::vector<float>& b,float alpha){
    auto out=a;for(unsigned i=0;i<out.size();++i)out[i]=a[i]+(b[i]-a[i])*alpha;return out;
}
bool bits_equal(const std::vector<float>& a,const std::vector<float>& b){
    if(a.size()!=b.size())return false;
    for(unsigned i=0;i<a.size();++i)if(std::bit_cast<uint32_t>(a[i])!=std::bit_cast<uint32_t>(b[i]))return false;return true;
}
const std::vector<float>& vector(const Property& p){return std::get<std::vector<float>>(p);}
void vector_regressions(){
    for(unsigned count:{0u,1u,3u,32u,256u})for(float alpha:{0.f,.03125f,.35f,.5f,1.f}){
        const Property a=pose(count,-.2f),b=pose(count,.8f);const auto saved_a=a,saved_b=b;
        const auto expected=reference(vector(a),vector(b),alpha);
        Property out=pose(count,91.f);const auto* storage=vector(out).data();
        interpolate_property(out,a,b,alpha,"enemy_bone_matrices");
        require(bits_equal(vector(out),expected),"Vector interpolation differs by float bits");
        if(presentation_vector_reuse_enabled() && count)require(vector(out).data()==storage,"Equal-size render pose did not reuse storage");
        require(bits_equal(vector(a),vector(saved_a)) && bits_equal(vector(b),vector(saved_b)),"Interpolation mutated immutable input poses");
        out=std::string("different output type");interpolate_property(out,a,b,alpha,"enemy_bone_matrices");
        require(bits_equal(vector(out),expected),"Different output type did not retain reference fallback");
        out=std::vector<float>(count*12+7,100.f);interpolate_property(out,a,b,alpha,"enemy_bone_matrices");
        require(bits_equal(vector(out),expected),"Different output size did not retain reference values");
        auto left=a,right=b;interpolate_property(left,left,right,alpha,"enemy_bone_matrices");
        require(bits_equal(vector(left),expected) && bits_equal(vector(right),vector(b)),"Left-input alias changed interpolation");
        left=a;right=b;interpolate_property(right,left,right,alpha,"enemy_bone_matrices");
        require(bits_equal(vector(right),expected) && bits_equal(vector(left),vector(a)),"Right-input alias changed interpolation");
        left=a;const auto same=reference(vector(a),vector(a),alpha);interpolate_property(left,left,left,alpha,"enemy_bone_matrices");
        require(bits_equal(vector(left),same),"Both-input alias changed interpolation");
    }
    Property a=pose(3,0),b=pose(4,1),out=std::string("unchanged");
    interpolate_property(out,a,b,.5f,"enemy_bone_matrices");require(out==Property(std::string("unchanged")),"Unequal input lengths mutated output");
    const auto saved=a;interpolate_property(a,a,b,.5f,"enemy_bone_matrices");require(bits_equal(vector(a),vector(saved)),"Unequal-length alias was mutated");
    b=1.f;interpolate_property(out,a,b,.5f,"enemy_bone_matrices");require(out==Property(std::string("unchanged")),"Unequal input types mutated output");
}
std::shared_ptr<WorldState> world(float time,float x,float bias){
    auto w=std::make_shared<WorldState>();w->epoch=1;w->revision=1;w->level="Rlevel1_2";w->mission["Session.Motion.Time"]=time;
    EntityState player;player.id=1;player.kind=EntityKind::Player;player.resource="Sam";player.owner={0,1};
    player.properties["position"]=std::array<float,3>{0,0,0};player.properties["health"]=100.f;player.properties["movement_mode"]=int64_t(0);
    w->entities.emplace(1,player);
    EntityState enemy;enemy.id=2;enemy.kind=EntityKind::Enemy;enemy.definition=1;enemy.resource="Rocketeer";
    enemy.properties["position"]=std::array<float,3>{x,0,0};enemy.properties["health"]=100.f;
    enemy.properties["render_visible"]=int64_t(1);enemy.properties["render_row0"]=std::array<float,4>{x,x+1,x+2,x+3};
    enemy.properties["enemy_bone_count"]=int64_t(3);enemy.properties["enemy_bone_layout"]=int64_t(71);enemy.properties["enemy_bone_matrices"]=pose(3,bias);
    enemy.properties["unused_network_inventory"]=std::vector<int64_t>{1,2,3,4};w->entities.emplace(2,enemy);return w;
}
void frame_regressions(){
    {
        // Successive PAL Werebull positions recorded during a charge in Rlevel1_3.
        const std::array<float,3> first{-148.631f,3.30809f,1398.73f}, next{-149.959f,3.30809f,1394.94f};
        auto start=world(0,0,0),end=world(.1f,0,0);
        auto& from=start->entities.at(2);auto& to=end->entities.at(2);
        from.definition=to.definition=5;from.resource=to.resource="Werebull";
        from.properties["position"]=first;to.properties["position"]=next;
        ClientPresentation charge;
        charge.push(start,100.,{0,1});charge.push(end,100.1,{0,1});
        std::map<EntityId,EntityState> frame;
        charge.sample_into(100.02,frame);
        const auto rendered=std::get<std::array<float,3>>(frame.at(2).properties.at("position"));
        require(rendered[2]<first[2] && rendered[2]>next[2],"A normal Werebull charge snapped to the last snapshot");
        auto teleported=to;teleported.properties["position"]=std::array<float,3>{0,0,0};
        require(!presentation_continuous(from,teleported,.1),"A teleported Werebull retained its old charge path");
    }
    auto a=world(0,0,-.2f),b=world(.05f,1,.8f);b->revision=2;
    const auto a_entities=a->entities,b_entities=b->entities;const auto a_mission=a->mission,b_mission=b->mission;
    ClientPresentation p;require(p.push(a,100,{0,1}),"First sample did not establish epoch");require(!p.push(b,100.05,{0,1}),"Continuous second sample reset interpolation");
    bool had_intermediate=false;
    for(double now:{100.001,100.004,100.01,100.02,100.04,100.07}){
        const auto sampled=p.sample(now);require(sampled.size()==2,"Render sampling lost an entity");
        const auto& e=sampled.at(2);const float alpha=std::get<std::array<float,3>>(e.properties.at("position"))[0];
        require(alpha>=0 && alpha<=1,"Render alpha left its endpoints");had_intermediate|=alpha>0 && alpha<1;
        auto expected=presentation_fields(b->entities.at(2));
        expected.properties["position"]=std::array<float,3>{alpha,0,0};
        auto row=std::get<std::array<float,4>>(a->entities.at(2).properties.at("render_row0"));const auto end=std::get<std::array<float,4>>(b->entities.at(2).properties.at("render_row0"));
        for(unsigned i=0;i<4;++i)row[i]=row[i]+(end[i]-row[i])*alpha;expected.properties["render_row0"]=row;
        // When the cursor reaches the newest snapshot, sample has no span and
        // adopts that snapshot directly; recomputing alpha=1 can round differently.
        expected.properties["enemy_bone_matrices"]=alpha==1.f?vector(b->entities.at(2).properties.at("enemy_bone_matrices")):
            reference(vector(a->entities.at(2).properties.at("enemy_bone_matrices")),vector(b->entities.at(2).properties.at("enemy_bone_matrices")),alpha);
        require(e==expected,"Complete render property set differs from expected interpolation");
        require(bits_equal(vector(e.properties.at("enemy_bone_matrices")),vector(expected.properties.at("enemy_bone_matrices"))),"Complete frame pose differs by float bits");
        require(sampled.at(1)==presentation_fields(b->entities.at(1)),"Unchanged owner presentation drifted");
    }
    require(had_intermediate,"Frame test never exercised interpolation");
    require(a->entities==a_entities && b->entities==b_entities && a->mission==a_mission && b->mission==b_mission,"Sampling modified retained authoritative snapshots");
    auto reset=world(1,3,5);reset->epoch=2;require(p.push(reset,200,{0,1}),"Epoch change did not cut interpolation");
    require(p.sample(200.001).at(2)==presentation_fields(reset->entities.at(2)),"Old-epoch pose leaked into new level");
    auto layout=world(1.05f,3.5f,8);layout->epoch=2;layout->entities.at(2).properties["enemy_bone_count"]=int64_t(4);layout->entities.at(2).properties["enemy_bone_layout"]=int64_t(72);layout->entities.at(2).properties["enemy_bone_matrices"]=pose(4,8);
    require(!p.push(layout,200.05,{0,1}),"Other model layout reset the owner buffer");const auto layout_frame=p.sample(200.002);
    require(bits_equal(vector(layout_frame.at(2).properties.at("enemy_bone_matrices")),vector(layout->entities.at(2).properties.at("enemy_bone_matrices"))),"Layout change blended incompatible skeletons");
    auto retired=std::make_shared<WorldState>(*layout);retired->mission["Session.Motion.Time"]=1.10f;retired->entities.erase(2);auto newcomer=layout->entities.at(2);newcomer.id=3;retired->entities.emplace(3,newcomer);
    p.push(retired,200.10,{0,1});const auto retired_frame=p.sample(200.003);
    require(!retired_frame.contains(2) && retired_frame.at(3)==presentation_fields(newcomer),"Presentation resurrected retired identity or blended a new lifetime");
    auto teleport=std::make_shared<WorldState>(*retired);teleport->mission["Session.Motion.Time"]=1.15f;teleport->entities.at(3).properties["position"]=std::array<float,3>{100,0,0};
    ClientPresentation teleport_buffer;teleport_buffer.push(retired,300,{0,1});teleport_buffer.push(teleport,300.05,{0,1});
    require(teleport_buffer.sample(300.001).at(3)==presentation_fields(teleport->entities.at(3)),"Teleport did not snap to current pose");
    auto model=std::make_shared<WorldState>(*retired);model->mission["Session.Motion.Time"]=1.15f;model->entities.at(3).resource="DifferentModel";model->entities.at(3).properties["enemy_bone_matrices"]=pose(4,19);
    ClientPresentation model_buffer;model_buffer.push(retired,400,{0,1});model_buffer.push(model,400.05,{0,1});
    require(model_buffer.sample(400.001).at(3)==presentation_fields(model->entities.at(3)),"Resource identity change interpolated an old model pose");
}
bool property_bits(const Property& a,const Property& b){
    if(a.index()!=b.index())return false;
    if(const auto* x=std::get_if<float>(&a))return std::bit_cast<uint32_t>(*x)==std::bit_cast<uint32_t>(std::get<float>(b));
    if(const auto* x=std::get_if<std::array<float,3>>(&a)){const auto& y=std::get<std::array<float,3>>(b);for(unsigned i=0;i<3;++i)if(std::bit_cast<uint32_t>((*x)[i])!=std::bit_cast<uint32_t>(y[i]))return false;return true;}
    if(const auto* x=std::get_if<std::array<float,4>>(&a)){const auto& y=std::get<std::array<float,4>>(b);for(unsigned i=0;i<4;++i)if(std::bit_cast<uint32_t>((*x)[i])!=std::bit_cast<uint32_t>(y[i]))return false;return true;}
    if(const auto* x=std::get_if<std::vector<float>>(&a))return bits_equal(*x,std::get<std::vector<float>>(b));return a==b;
}
bool properties_bits(const Properties& a,const Properties& b){
    if(a.size()!=b.size())return false;auto x=a.begin(),y=b.begin();
    for(;x!=a.end();++x,++y)if(x->first!=y->first || !property_bits(x->second,y->second))return false;return true;
}
bool entities_bits(const std::map<EntityId,EntityState>& a,const std::map<EntityId,EntityState>& b){
    if(a.size()!=b.size())return false;auto x=a.begin(),y=b.begin();
    for(;x!=a.end();++x,++y){const auto& e=x->second;const auto& f=y->second;
        if(x->first!=y->first || e.id!=f.id || e.kind!=f.kind || e.definition!=f.definition || e.resource!=f.resource || e.owner!=f.owner || !properties_bits(e.properties,f.properties))return false;
    }return true;
}
std::shared_ptr<WorldState> rich_world(float time,float phase){
    auto w=world(time,phase,-.2f+phase);w->revision=uint64_t(time*1000)+1;
    auto& local=w->entities.at(1);local.properties["position"]=std::array<float,3>{phase*.1f,0,0};
    local.properties["camera_mode"]=int64_t(0);local.properties["camera_frozen"]=int64_t(0);
    const std::array<float,3> angles{.1f+phase*.01f,.02f,.01f};local.properties["view_angles"]=angles;
    auto camera=native_camera_rotation(angles);camera[3]=phase*.1f;camera[7]=1.7f;camera[11]=phase*.2f;put_presentation_matrix(local.properties,"camera_row",camera);
    local.properties["weapon_model_slot"]=int64_t(0);local.properties["weapon_model_visibility"]=std::vector<int64_t>{1,0};
    for(unsigned hand=0;hand<2;++hand){
        const auto prefix="weapon_model"+std::to_string(hand);auto gun=matrix_identity();gun[3]=hand?.3f:-.3f;gun[7]=-.4f;gun[11]=.8f;
        put_presentation_matrix(local.properties,prefix+"_row",matrix_product(camera,gun));
        local.properties[prefix+"_bone_count"]=int64_t(3);local.properties[prefix+"_bone_layout"]=int64_t(200+hand);local.properties[prefix+"_bone_matrices"]=pose(3,phase);
    }
    for(unsigned part=0;part<4;++part){const auto prefix="body"+std::to_string(part)+"_bone_";local.properties[prefix+"count"]=int64_t(3);local.properties[prefix+"layout"]=int64_t(300+part);local.properties[prefix+"matrices"]=pose(3,phase+part);}
    auto remote=local;remote.id=3;remote.owner={1,1};remote.properties["position"]=std::array<float,3>{phase*.2f,0,2};w->entities.emplace(3,remote);
    auto& enemy=w->entities.at(2);enemy.properties["enemy_rotation"]=std::array<float,3>{3.12f+phase,0,-3.12f-phase};
    EntityState pickup;pickup.id=5;pickup.kind=EntityKind::Pickup;pickup.resource="Health";pickup.properties["position"]=std::array<float,3>{1,0,0};pickup.properties["pickup_glow_pose"]=pose(1,phase);w->entities.emplace(5,pickup);
    auto corpse=enemy;corpse.id=6;corpse.kind=EntityKind::Physics;corpse.definition=3;corpse.properties["corpse_mode"]=int64_t(1);corpse.properties["corpse_active"]=int64_t(1);w->entities.emplace(6,corpse);
    auto projectile=enemy;projectile.id=7;projectile.kind=EntityKind::Projectile;projectile.definition=1;projectile.resource="Rocket";projectile.properties.erase("enemy_bone_count");projectile.properties.erase("enemy_bone_layout");projectile.properties.erase("enemy_bone_matrices");w->entities.emplace(7,projectile);
    EntityState door;door.id=8;door.kind=EntityKind::Door;door.properties["position"]=std::array<float,3>{3,0,0};w->entities.emplace(8,door);
    std::vector<int64_t> tags(20);tags[0]=100;tags[1]=1;tags[6]=2;w->mission["Session.Vehicles.Tags"]=tags;
    auto values=pose(11,phase);values.resize(128);values[0]=phase;values[1]=0;values[2]=1;values[3]=3.12f+phase;values[4]=0;values[5]=0;values[6]=100+phase;values[7]=200+phase;w->mission["Session.Vehicles.Values"]=values;return w;
}
struct CacheComparison {
    ClientPresentation reference,cached;
    std::map<EntityId,EntityState> output;Properties vehicles,reference_vehicles;
    std::vector<std::shared_ptr<WorldState>> sources;std::vector<WorldState> originals;
    unsigned frames=0;
    void push(const std::shared_ptr<WorldState>& w,double received,PlayerId owner={0,1}){
        sources.push_back(w);originals.push_back(*w);
        require(reference.push(w,received,owner)==cached.push(w,received,owner),"Cached and reference push/reset disagree");
    }
    void check(double now,const char* label){
        const auto expected=reference.sample(now,&reference_vehicles);cached.sample_into(now,output,&vehicles);++frames;
        if(!entities_bits(output,expected))throw std::runtime_error(std::string(label)+": cached entity/pose float bits differ, frame="+std::to_string(frames));
        if(!properties_bits(vehicles,reference_vehicles))throw std::runtime_error(std::string(label)+": cached vehicle float bits differ");
        for(unsigned i=0;i<sources.size();++i)require(entities_bits(sources[i]->entities,originals[i].entities) && properties_bits(sources[i]->mission,originals[i].mission),"Cached render modified a retained immutable snapshot");
    }
    void disturb_output(){
        if(output.contains(1)){
            follow_local_camera(output.at(1),sources.back()->entities.at(1),{.2,-.1},-1.f,1.f);
            output.at(1).properties["camera_row1"]=9.f;output.at(1).properties["unused_extra"]=std::vector<float>{3,4};output.at(1).resource="damaged scratch metadata";
        }
        output.erase(2);EntityState extra;extra.id=999;output.emplace(999,extra);
        vehicles["unexpected"]=int64_t(1);vehicles["Session.Vehicles.Values"]=std::vector<float>{0};
    }
};
void cached_frame_regressions(){
    CacheComparison h;h.push(rich_world(0,0),100);h.check(100.01,"single snapshot");h.disturb_output();h.check(100.03,"single-snapshot output reset");h.check(100.049,"before next arrival");
    for(unsigned step=1;step<=12;++step){
        auto w=rich_world(step*.05f,step*.1f);
        if(step==2)w->entities.at(1).properties["weapon_model_slot"]=int64_t(1);
        if(step==3)w->entities.at(1).properties["weapon_model_visibility"]=std::vector<int64_t>{0,-1};
        if(step==4)w->entities.at(2).properties["enemy_bone_layout"]=int64_t(99);
        if(step==5)w->entities.at(2).properties["health"]=0.f;
        if(step==6)w->entities.at(1).properties["camera_frozen"]=int64_t(1);
        if(step==7)w->entities.at(1).properties["movement_mode"]=int64_t(3);
        if(step>=9)w->epoch=2;
        if(step==10){w->entities.erase(2);w->entities.at(5).properties["pickup_glow_pose"]=std::vector<float>{};}
        if(step==11){auto e=w->entities.at(2);w->entities.erase(2);e.id=9;w->entities.emplace(9,e);}
        if(step==12){auto& tags=std::get<std::vector<int64_t>>(w->mission.at("Session.Vehicles.Tags"));tags[0]=101;}
        const auto received=100+step*.05;h.push(w,received);
        for(double offset:{.001,.004,.009,.019,.035,.049}){h.check(received+offset,"transition sequence");h.disturb_output();}
    }
    // An intermediate absent glow remains a discrete edge even when both
    // interpolation endpoints and the newest snapshot have matching vectors.
    CacheComparison glow;glow.push(rich_world(0,0),500);glow.check(500.149,"glow baseline");
    glow.push(rich_world(.05f,.1f),500.15);auto absent=rich_world(.1f,.2f);absent->entities.at(5).properties["pickup_glow_pose"]=std::vector<float>{};glow.push(absent,500.15);glow.push(rich_world(.15f,.3f),500.15);
    glow.check(500.151,"burst pickup lifetime edge");glow.disturb_output();glow.check(500.154,"repeated pickup lifetime edge");
    CacheComparison retained;
    for(unsigned i=0;i<38;++i)retained.push(rich_world(i*.02f,i*.02f),600+i*.02);
    for(double t:{600.741,600.745,600.76,600.79})retained.check(t,"history pruning");
    auto owner_change=rich_world(.78f,.78f);retained.push(owner_change,600.78,{1,1});retained.check(600.781,"owner generation reset");
    // Cached output may be replaced/cleared by its owner between leases.
    retained.output.clear();retained.vehicles.clear();retained.check(600.785,"scratch reconstruction");
    // Missing/mismatched vehicle metadata uses the exact old stopping rules.
    CacheComparison vehicle;vehicle.push(rich_world(0,0),700);vehicle.check(700.049,"vehicle baseline");
    auto malformed=rich_world(.05f,.1f);std::get<std::vector<int64_t>>(malformed->mission.at("Session.Vehicles.Tags"))[6]=129;vehicle.push(malformed,700.05);vehicle.check(700.051,"vehicle count boundary");
    auto missing=rich_world(.10f,.2f);missing->mission.erase("Session.Vehicles.Tags");vehicle.push(missing,700.10);vehicle.check(700.101,"vehicle missing tags");
    auto short_values=rich_world(.15f,.3f);short_values->mission["Session.Vehicles.Values"]=std::vector<float>{1};vehicle.push(short_values,700.15);vehicle.check(700.151,"vehicle short values");
    ClientPresentation empty;std::map<EntityId,EntityState> output{{999,{}}};Properties untouched{{"caller_sentinel",int64_t(7)}};empty.sample_into(1,output,&untouched);require(output.empty() && untouched.size()==1 && untouched.contains("caller_sentinel"),"Empty buffer changed caller vehicle sentinel");
    auto immutable=rich_world(0,0);const auto frozen=*immutable;ClientPresentation guard;guard.push(immutable,800,{0,1});bool rejected=false;
    try{guard.sample_into(800.01,immutable->entities);}catch(const std::invalid_argument&){rejected=true;}require(rejected && entities_bits(immutable->entities,frozen.entities),"Output alias mutated an immutable entity map");
    rejected=false;try{guard.sample_into(800.01,output,&immutable->mission);}catch(const std::invalid_argument&){rejected=true;}require(rejected && properties_bits(immutable->mission,frozen.mission),"Vehicle output alias mutated an immutable snapshot");
    std::weak_ptr<const WorldState> weak=immutable;guard.sample_into(800.01,output);immutable.reset();guard.reset();require(weak.expired(),"Reset retained cached snapshot ownership");
}

}
int main(){try{vector_regressions();frame_regressions();cached_frame_regressions();std::cout<<"PASS presentation poses: exact float bits, immutable inputs, alias/fallback handling, complete frames, epoch/layout/lifetime/teleport guards and cached/reference frames; reuse="<<presentation_vector_reuse_enabled()<<'\n';return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
