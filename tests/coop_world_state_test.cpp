#include "world_state.h"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace port::coop;
void require(bool result,const char* message){if(!result){std::cerr<<message<<'\n';std::exit(1);}}
int main() {
    WorldState rule;rule.level="Rlevel1_1";std::string rule_error;
    for(int64_t e=1;e<=5;++e)for(int64_t c=0;c<=5;++c)for(int64_t p=1;p<=5;++p){
        rule.mission={{"Session.EnemyMultiplier",e},{"Session.AmmoCapacity",c},{"Session.AmmoPickupMultiplier",p}};
        require(validate_world(rule,rule_error),"Extended host rules rejected");
    }
    for(auto key:{"Session.EnemyMultiplier","Session.AmmoCapacity","Session.AmmoPickupMultiplier"}){
        auto bad=rule;bad.mission[key]=int64_t(6);require(!validate_world(bad,rule_error),"Out-of-range multiplier accepted");
        bad=rule;bad.mission[key]=1.f;require(!validate_world(bad,rule_error),"Wrong rule type accepted");
    }
    WorldState host;host.revision=1;host.tick=50;host.level="Levels/Rome02";
    host.mission={{"checkpoint",int64_t(4)},{"first_door_open",int64_t(1)},{"secret_found",int64_t(1)}};
    for(unsigned i=0;i<1000;++i) {
        EntityState e;e.id=i+1;e.kind=i<8?EntityKind::Player:EntityKind::Enemy;e.definition=i%16;
        e.resource="actors/Sam";e.properties={{"position",std::array<float,3>{float(i),2,3}},
            {"health",100.f},{"target",EntityReference{1}},{"ammo",std::vector<int64_t>{12,100,5}},
            {"animation",std::string("walk")},{"rotation",std::array<float,4>{0,0,0,1}}};
        host.entities.emplace(e.id,e);
    }
    auto full=difference(nullptr,host);auto encoded=encode_delta(full);WorldDelta parsed;std::string error;
    require(decode_delta(encoded,parsed,error),error.c_str());WorldState client;
    require(apply_delta(client,parsed,error),error.c_str());
    require(client.entities==host.entities && client.mission==host.mission,"Full snapshot differs");
    auto original=host;host.revision=2;host.tick=51;
    host.entities.at(1).properties["position"]=std::array<float,3>{4,5,6};
    host.entities.at(1).properties.erase("animation");host.entities.erase(1000);
    host.entities.at(900).properties["health"]=0.f;host.mission["second_door_open"]=int64_t(1);
    auto delta=difference(&original,host);auto small=encode_delta(delta);
    require(delta.entities.size()==2 && delta.remove.size()==1,"Delta includes unchanged entities");
    require(small.size()*20<encoded.size(),"Single movement update resends whole world");
    require(decode_delta(small,parsed,error) && apply_delta(client,parsed,error),error.c_str());
    require(client.entities==host.entities && client.mission==host.mission,"Delta reconstruction differs");
    auto revision=client.revision;auto entities=client.entities;
    require(!apply_delta(client,parsed,error),"Duplicate delta accepted");
    require(client.revision==revision && client.entities==entities,"Rejected delta mutated world");
    for(size_t i=0;i<small.size();++i) {
        std::vector<uint8_t> truncated(small.begin(),small.begin()+i);
        require(!decode_delta(truncated,parsed,error),"Truncated world frame accepted");
    }
    auto bad=delta;bad.revision=3;bad.base_revision=2;
    bad.entities[0].properties.set["health"]=std::numeric_limits<float>::quiet_NaN();
    require(!apply_delta(client,bad,error),"NaN property accepted");
    require(client.revision==revision && client.entities==entities,"Malformed delta partially committed");
    bad=delta;bad.revision=3;bad.base_revision=2;bad.entities.push_back(bad.entities.front());
    require(!apply_delta(client,bad,error),"Duplicate entity patch accepted");
    bad=delta;bad.revision=3;bad.base_revision=1;require(!apply_delta(client,bad,error),"Missing baseline accepted");
    host.epoch=2;host.revision=3;host.level="Levels/Rome03";host.entities.clear();host.mission.clear();
    auto transition=difference(&original,host);require(transition.full,"Level transition needs full new world");
    require(apply_delta(client,transition,error),error.c_str());require(client.entities.empty() && client.mission.empty(),"Previous level entities retained");
    WorldState results;results.level="Rlevel1_1";results.mission={{"Session.Results.Active",int64_t(1)},{"Session.Results.Sequence",int64_t(1)},{"Session.Results.Level",std::string("Rlevel1_1")},{"Session.Results.Score",int64_t(139054)},{"Session.Results.Values",std::vector<float>(13,0.f)}};
    require(validate_world(results,error),"Valid authoritative results rejected");
    auto invalid_results=results;invalid_results.mission.erase("Session.Results.Score");require(!validate_world(invalid_results,error),"Incomplete result set accepted");
    invalid_results=results;invalid_results.mission["Session.Results.Values"]=std::vector<float>(12,0.f);require(!validate_world(invalid_results,error),"Incorrect result vector size accepted");
    invalid_results=results;invalid_results.mission["Session.Results.Score"]=int64_t(-1);require(!validate_world(invalid_results,error),"Negative result score accepted");
    invalid_results=results;invalid_results.mission["Session.Results.Active"]=1.f;require(!validate_world(invalid_results,error),"Incorrect active-result type accepted");
    WorldState scene;scene.level="Rlevel1_1";scene.vote.cutscene=2;
    scene.mission={{"Session.Cutscene.Id",int64_t(2)},{"Session.Cutscene.Reference",int64_t(1953)},{"Session.Cutscene.Camera",std::vector<float>{1,0,0,4,0,1,0,5,0,0,1,6,90,80}}};
    require(validate_world(scene,error),"Valid native cutscene rejected");
    auto invalid_scene=scene;invalid_scene.mission.erase("Session.Cutscene.Camera");require(!validate_world(invalid_scene,error),"Missing native camera accepted");
    invalid_scene=scene;invalid_scene.vote.cutscene=3;require(!validate_world(invalid_scene,error),"Mismatched scene/vote identity accepted");
    invalid_scene=scene;std::get<std::vector<float>>(invalid_scene.mission.at("Session.Cutscene.Camera"))[12]=0;require(!validate_world(invalid_scene,error),"Invalid cutscene projection accepted");
    invalid_scene=scene;invalid_scene.mission["Session.Cutscene.Camera"]=std::vector<float>(13,1.f);require(!validate_world(invalid_scene,error),"Truncated native camera accepted");
    WorldState music;music.level="Rlevel1_1";music.mission={{"Session.Music.Sequence",int64_t(1)},{"Session.Music.Track",int64_t(2367335969u)},{"Session.Music.Force",int64_t(0)},{"Session.Music.Fade",0.25f}};
    require(validate_world(music,error),"Valid native music request rejected");
    auto invalid_music=music;invalid_music.mission.erase("Session.Music.Fade");require(!validate_world(invalid_music,error),"Partial music request accepted");
    invalid_music=music;invalid_music.mission["Session.Music.Track"]=int64_t(-1);require(!validate_world(invalid_music,error),"Invalid native track accepted");
    invalid_music=music;invalid_music.mission["Session.Music.Force"]=int64_t(2);require(!validate_world(invalid_music,error),"Invalid music force flag accepted");
    invalid_music=music;invalid_music.mission["Session.Music.Fade"]=-1.f;require(!validate_world(invalid_music,error),"Invalid music fade accepted");
    WorldState speech;speech.level="Rlevel1_1";speech.mission={{"Session.Speech.Sequence",int64_t(1)},{"Session.Speech.Track",int64_t(4041755483u)},{"Session.Speech.Reference",int64_t(2809)},{"Session.Speech.OutputFrames",int64_t(6400)},{"Session.Speech.OutputRate",int64_t(32000)},{"Session.Speech.Phase",int64_t(1)}};
    require(validate_world(speech,error),"Valid shared speech rejected");
    auto invalid_speech=speech;invalid_speech.mission.erase("Session.Speech.Reference");require(!validate_world(invalid_speech,error),"Missing speech source accepted");
    invalid_speech=speech;invalid_speech.mission["Session.Speech.Track"]=int64_t(UINT32_MAX);require(!validate_world(invalid_speech,error),"Invalid speech asset accepted");
    invalid_speech=speech;invalid_speech.mission["Session.Speech.Sequence"]=0.f;require(!validate_world(invalid_speech,error),"Wrong speech sequence type accepted");
    invalid_speech=speech;invalid_speech.mission["Session.Speech.OutputFrames"]=int64_t(-1);require(!validate_world(invalid_speech,error),"Invalid speech playhead accepted");
    invalid_speech=speech;invalid_speech.mission["Session.Speech.OutputRate"]=int64_t(44100);require(!validate_world(invalid_speech,error),"Unexpected speech output clock accepted");
    auto preparing=speech;preparing.mission["Session.Speech.Phase"]=int64_t(0);preparing.mission["Session.Speech.OutputFrames"]=int64_t(0);require(validate_world(preparing,error),"Native preparing phase rejected");
    invalid_speech=preparing;invalid_speech.mission["Session.Speech.OutputFrames"]=int64_t(1);require(!validate_world(invalid_speech,error),"Preparing speech advances clock");
    invalid_speech=speech;invalid_speech.mission["Session.Speech.Phase"]=int64_t(2);require(!validate_world(invalid_speech,error),"Invalid speech phase accepted");
    std::cout<<"World replication: 1,000 entities, changed-field deltas, deletions, atomic rejection and level epochs passed ("<<encoded.size()<<" full bytes / "<<small.size()<<" delta bytes).\n";
}
