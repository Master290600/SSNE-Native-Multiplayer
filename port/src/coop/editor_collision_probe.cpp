#include "editor_collision_probe.h"
#include "native_bootstrap.h"
#include "native_world.h"
#include "../../../launcher/src/launcher_json.h"
#include "ssne/game_addresses.h"
#include "ssne/native_party.h"
#include "ssne/recomp_hooks.h"
#include "ssne/replica_simulation.h"
#include "ssne/runtime.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace port::coop {
namespace {
using J=launcher::JSONValue;
using Point=std::array<float,3>;
using Clock=std::chrono::steady_clock;
void need(bool yes,const char* error){if(!yes)throw std::runtime_error(error);}
bool readable(uint32_t p,uint32_t size){return p>=0x80000000u&&uint64_t(p)+size<=ssne::guest_ram_end();}
std::string field(const J& j,const char* name){const auto* p=j.find(name);need(p&&p->string(),"Probe text field is missing");return*p->string();}
double number(const J& j){if(const auto* p=std::get_if<double>(&j.value))return*p;int64_t n=0;need(j.integer(n),"Probe coordinate is not numeric");return double(n);}
Point point(const J& j){need(j.array()&&j.array()->size()==3,"Probe point must have three coordinates");Point p;for(unsigned n=0;n<3;++n){const auto v=number(j.array()->at(n));need(std::isfinite(v)&&std::abs(v)<=1e6,"Probe coordinate is out of bounds");p[n]=float(v);}return p;}
J json_point(const Point& p){J::Array values;for(auto v:p){need(std::isfinite(v),"Native probe returned a non-finite coordinate");values.emplace_back(double(v));}return values;}
struct Query {std::string id,kind;Point from{},to{};float radius=.7f;};
struct Actor {uint32_t id=0;std::vector<Point> points;};
struct State {bool enabled=false,initialized=false,finished=false;std::filesystem::path request,output;std::string id,level;std::vector<Query> queries;std::vector<Actor> actors;Clock::time_point ready{};};
State state;
bool slug(const std::string& v){return !v.empty()&&v.size()<=64&&std::all_of(v.begin(),v.end(),[](unsigned char c){return c>='a'&&c<='z'||c>='A'&&c<='Z'||c>='0'&&c<='9'||c=='_'||c=='-';});}
void write(J::Object value){
 value.emplace("schema",1);value.emplace("case",state.id);value.emplace("request",state.request.string());value.emplace("observations_only",true);
 const auto bytes=launcher::write_json(value,true);auto pending=state.output;pending+=".pending";
 std::ofstream out(pending,std::ios::binary|std::ios::trunc);need(bool(out),"Cannot write collision probe result");out.write(bytes.data(),bytes.size());out.close();need(bool(out),"Cannot finish collision probe result");
#ifdef _WIN32
 need(MoveFileExW(pending.c_str(),state.output.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,"Cannot publish collision probe result");
#else
 std::filesystem::rename(pending,state.output);
#endif
}
void initialize(){
 state.initialized=true;const auto* hidden=std::getenv("SSNE_TEST_HIDDEN");const auto* path=std::getenv("SSNE_EDITOR_COLLISION_PROBE");
 if(!hidden||std::string(hidden)!="1"||!path||!*path)return;
 state.request=std::filesystem::absolute(std::filesystem::u8path(path));state.output=state.request;state.output+=".result.json";state.enabled=true;
 need(!std::filesystem::is_symlink(state.request),"Probe request cannot be a symlink");
 std::ifstream file(state.request,std::ios::binary|std::ios::ate);need(bool(file)&&file.tellg()>0&&file.tellg()<=65536,"Probe request is missing or too large");std::string bytes(size_t(file.tellg()),'\0');file.seekg(0);need(bool(file.read(bytes.data(),bytes.size())),"Cannot read probe request");
 J request;std::string error;need(launcher::parse_json(bytes,request,error,{65536,256,4096,8}),"Invalid probe request JSON");int64_t schema=0;const auto* sv=request.find("schema");need(sv&&sv->integer(schema)&&schema==1,"Unsupported collision probe schema");
 state.id=field(request,"case");state.level=field(request,"level");need(slug(state.id)&&slug(state.level),"Invalid collision probe identity");
 const auto* queries=request.find("queries");need(queries&&queries->array()&&queries->array()->size()<=128,"Invalid collision probe queries");std::set<std::string> ids;
 for(const auto& row:*queries->array()){Query q;q.id=field(row,"id");q.kind=field(row,"kind");need(slug(q.id)&&ids.insert(q.id).second&&(q.kind=="segment"||q.kind=="sphere"),"Invalid collision probe query");const auto* from=row.find("from");const auto* to=row.find("to");need(from&&to,"Missing collision probe segment");q.from=point(*from);q.to=point(*to);if(q.kind=="sphere"){const auto* radius=row.find("radius");need(radius,"Missing collision probe radius");q.radius=float(number(*radius));need(q.radius>.01f&&q.radius<=100&&std::isfinite(q.radius),"Invalid collision probe radius");}state.queries.push_back(q);}
 const auto* actors=request.find("actors");need(actors&&actors->array()&&actors->array()->size()<=32,"Invalid actor probe list");size_t total_points=0;
 for(const auto& row:*actors->array()){const auto* id=row.find("id");int64_t value=0;need(id&&id->integer(value)&&value>0&&value<=32767,"Invalid actor probe ID");Actor actor;actor.id=uint32_t(value);const auto* points=row.find("points");need(points&&points->array()&&points->array()->size()<=16,"Invalid actor probe points");for(const auto& v:*points->array())actor.points.push_back(point(v));total_points+=actor.points.size();need(total_points<=128,"Too many actor probe points");state.actors.push_back(std::move(actor));}
}
struct Scratch {
 PPCContext& base;std::array<uint8_t,0x2000> bytes{};
 explicit Scratch(PPCContext& c):base(c){need(c.r[1]>=0x80002000u&&readable(c.r[1],4),"Native collision probe stack is unavailable");for(unsigned n=0;n<bytes.size();++n)bytes[n]=mem_read8(c.r[1]-uint32_t(bytes.size())+n);}
 ~Scratch(){for(unsigned n=0;n<bytes.size();++n)mem_write8(base.r[1]-uint32_t(bytes.size())+n,bytes[n]);}
 PPCContext call(){auto c=base;c.msr&=~0x8000u;c.r[1]-=0x800;mem_write32(c.r[1],base.r[1]);return c;}
};
bool collidable_chain_contains(uint32_t actor){std::set<uint32_t> seen;for(auto p=mem_read32(0x802DA8E8u);p;p=mem_read32(p+8)){need(readable(p,12)&&seen.insert(p).second&&seen.size()<=8192,"Invalid native collidable-actor chain");if(mem_read32(p+4)==actor)return true;}return false;}
J actor_observation(PPCContext& ctx,const Actor& requested){
 J::Object row{{"id",int64_t(requested.id)}};const auto actor=native_script_reference_address(requested.id);
 if(!actor||!*actor){row.emplace("present",false);return row;}
 const auto p=*actor;need(readable(p,0x8c)&&mem_read32(p+0x48)==0x80206348u,"Probe target is not a native ModelHolder2");const auto vt=mem_read32(p+0x48);
 need(mem_read32(vt+0x194)==0x8007DCCCu&&mem_read32(vt+0x19c)==0x8009C410u&&mem_read32(vt+0x1a4)==0x8009C418u&&mem_read32(vt+0x1ac)==0x8009C44Cu,"Native ModelHolder2 collision getter revision differs");
 Point centre;for(unsigned n=0;n<3;++n)centre[n]=u2f(mem_read32(p+0x20+n*4));
 Scratch scratch(ctx);auto call=scratch.call();call.r[3]=p;ssne_call(call,0x8007DCCCu);const auto radius=float(call.ps0[1]);need(std::isfinite(radius)&&radius>=0&&radius<=10000,"Invalid native ModelHolder2 collision radius");
 row.emplace("present",true);row.emplace("type",4200);row.emplace("vtable",int64_t(vt));row.emplace("centre",json_point(centre));row.emplace("radius",double(radius));row.emplace("sphere_count",1);row.emplace("active_flags",int64_t(mem_read32(p+0x1c)));row.emplace("in_collidable_actor_chain",collidable_chain_contains(p));
 J::Array results;const auto player=mem_read32(ssne::native_party_registry());const bool player_ready=ssne::native_player_collision_radius(player).has_value()&&(mem_read32(player+0x1c)&1u);
 for(const auto& point:requested.points){J::Object result{{"point",json_point(point)},{"available",player_ready}};if(player_ready){auto c=scratch.call();const auto location=ctx.r[1]-0x100;for(unsigned n=0;n<3;++n)mem_write32(location+n*4,f2u(point[n]));c.r[3]=player;c.r[4]=location;c.r[5]=p;ssne_call(c,0x800A3408u);result.emplace("native_player_overlap",c.r[3]!=0);}results.emplace_back(std::move(result));}
 row.emplace("points",std::move(results));return row;
}
}
void sample_editor_collision_probe(PPCContext& ctx,uint64_t frame){
 if(state.finished)return;
 try {
  if(!state.initialized)initialize();if(!state.enabled)return;
  if(mem_read32(0x80000000u)!=0x47334250u||mem_read32(ssne::game_address(0x802DB0E0))!=4)return;
  if(ctx.r[13]!=0x802E22A0u||ctx.r[2]!=0x802F22A0u||ctx.r[1]<0x80002000u||!readable(ctx.r[1],4))return;
  const auto level=native_active_level();if(!level||*level!=state.level)return;
  const auto now=Clock::now();if(state.ready==Clock::time_point{})state.ready=now;if(now-state.ready<std::chrono::milliseconds(250))return;
  // Wait for the real player and requested actors to finish normal activation.
  const auto player=mem_read32(ssne::native_party_registry());if(!ssne::native_player_collision_radius(player)||!(mem_read32(player+0x1c)&1u))return;
  for(const auto& actor:state.actors)if(!native_script_reference_address(actor.id))return;
  need(ssne_find_func(0x8007DCCCu)&&mem_read32(0x8007DCCCu)==0x9421FFE0u&&ssne_find_func(0x800A3408u)&&mem_read32(0x800A3408u)==0x9421FF40u,"Native ModelHolder collision query revision differs");
  J::Array actors,queries;for(const auto& actor:state.actors)actors.push_back(actor_observation(ctx,actor));
  for(const auto& query:state.queries){std::string error;J::Object row{{"id",query.id},{"kind",query.kind},{"from",json_point(query.from)},{"to",json_point(query.to)}};
   if(query.kind=="segment"){std::optional<ssne::NativeSegmentHit> hit;const bool available=ssne::native_world_segment(ctx,query.from,query.to,hit,error);row.emplace("available",available);row.emplace("hit",bool(hit));if(hit){row.emplace("position",json_point(hit->position));row.emplace("normal",json_point(hit->normal));row.emplace("flags",int64_t(hit->flags));}}
   else{ssne::NativeSphereResult result;const bool available=ssne::native_world_sphere(ctx,query.from,query.to,query.radius,result,error);row.emplace("available",available);row.emplace("radius",double(query.radius));if(available){row.emplace("corrected",json_point(result.position));J::Array contacts;for(const auto& hit:result.contacts)contacts.emplace_back(J::Object{{"position",json_point(hit.position)},{"normal",json_point(hit.normal)},{"flags",int64_t(hit.flags)}});row.emplace("contacts",std::move(contacts));}}
   row.emplace("error",error);queries.emplace_back(std::move(row));
  }
  write({{"ready",true},{"frame",int64_t(frame)},{"level",*level},{"authority",!ssne::replica_simulation()},{"actors",std::move(actors)},{"queries",std::move(queries)}});state.finished=true;
 }catch(const std::exception& e){state.finished=true;if(state.enabled)try{write({{"ready",false},{"error",e.what()},{"frame",int64_t(frame)}});}catch(...){} }
}
}
