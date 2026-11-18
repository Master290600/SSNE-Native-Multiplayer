// Private owned-PAL parity/rollback test, compiled only after profiling is idle.
#include "native_replica.h"
#include "client_presentation.h"
#include "native_presentation.h"
#include "ssne/native_entity.h"
#include "ssne/native_party.h"
#include "ssne/replica_simulation.h"
#include "ssne/runtime.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <cstring>
#include <cstdlib>
#include <tuple>
#include <new>
#include <chrono>
static thread_local bool count_allocations=false;
static thread_local size_t allocation_count=0;
void* operator new(std::size_t size){if(void* p=std::malloc(size?size:1)){if(count_allocations)++allocation_count;return p;}throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
// Original PAL apply routines may read OSGetTime; freeze the existing runtime
// clock so separate reference/span processes start from identical full RAM.
void ssne_timebase_freeze();
void ssne_timebase_shift(int64_t ticks);
namespace port::coop {struct NativeReplicaRenderTest {
 static size_t spans(const NativeReplica& r){return r.presentation_active_?(r.presentation_reference_active_?r.presentation_reference_:r.presentation_scratch_).span_count:0;}
 static size_t words(const NativeReplica& r){return r.presentation_saved_.size();}
 static bool active(const NativeReplica& r){return r.presentation_active_;}
 static size_t cached_bytes(const NativeReplica& r){return r.presentation_scratch_.retained_bytes();}
 static size_t cached_poses(const NativeReplica& r){return r.presentation_scratch_.poses.size();}
 static bool transient(const NativeReplica& r){return r.presentation_active_&&r.presentation_reference_active_;}
 static void exceed_budget(NativeReplica& r){r.presentation_scratch_.poses.resize(1);r.presentation_scratch_.poses[0].visual.reserve(r.presentation_scratch_budget_/sizeof(float)+1);}
 static void exceed_slots(NativeReplica& r){r.presentation_scratch_.poses.resize(r.presentation_scratch_pose_limit_+1);}
 static bool scrubbed(const NativeReplica& r){
  if(r.presentation_active_||r.presentation_reference_active_)return false;
  for(const auto* buffers:{&r.presentation_scratch_,&r.presentation_reference_}){
   if(buffers->pose_count||buffers->span_count||!buffers->spans.empty()||!buffers->scalar_ranges.empty())return false;
   for(const auto& pose:buffers->poses)if(pose.object||pose.model||pose.hand||pose.weapon!=-1||!pose.original.empty()||!pose.visual.empty())return false;
   for(const auto& span:buffers->saved_spans)if(span.address||!span.original.empty())return false;
   for(const auto& packed:buffers->packed_spans)if(!packed.empty())return false;
  }return true;
 }
};}
using namespace port::coop;
namespace {
void require(bool ok,const std::string& why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
std::vector<uint8_t> read(const std::string& path){std::ifstream in(path,std::ios::binary);require(bool(in),"Missing private fixture: "+path);return {(std::istreambuf_iterator<char>(in)),{}};}
uint64_t digest(){uint64_t h=14695981039346656037ull;for(unsigned i=0;i<ssne::guest_ram_size();++i){h^=ssne::g_ram[i];h*=1099511628211ull;}return h;}
struct RegistryExpectation {uint32_t object,model,matrices;std::vector<float> values;};
void verify_registry(const PPCContext& context,const std::vector<RegistryExpectation>& expected){
 for(const auto& pose:expected){
  std::vector<uint8_t> original(pose.values.size()*4);std::memcpy(original.data(),ssne::host_ptr(pose.matrices),original.size());const auto stamp=mem_read32(pose.model+0x80);
  mem_write32(pose.matrices,0x42F68000u);auto call=context;call.r[3]=pose.model;ssne_call(call,0x80129154u);require(call.r[3]==0,"Original native bones hook did not accept registered pose");
  for(unsigned i=0;i<pose.values.size();++i)require(mem_read32(pose.matrices+i*4)==f2u(pose.values[i]),"Native pose registry replay differs from expected exact float bits");
  std::memcpy(ssne::host_ptr(pose.matrices),original.data(),original.size());mem_write32(pose.model+0x80,stamp);
 }
}
std::map<EntityId,EntityState> authority(const NativeWorldSample& sample){std::map<EntityId,EntityState> out;for(const auto& [id,e]:sample.entities){auto v=e;v.id=id+10000;for(auto& [key,value]:v.properties)if(auto* ref=std::get_if<EntityReference>(&value);ref&&ref->id)ref->id+=10000;out.emplace(v.id,std::move(v));}return out;}
}
int main(int argc,char** argv){
 require(argc==3,"Pass owned game_data and current private PAL fixture");ssne::native_party_extended_memory(true);PPCContext context;std::string error;require(ssne::boot(context,argv[1],error)!=0,error);
 const std::string fixture=argv[2];auto ram=read(fixture+"/native-capture.ram");require(ram.size()==ssne::guest_ram_size(),"Extended fixture RAM differs");std::memcpy(ssne::g_ram,ram.data(),ram.size());ram.clear();ram.shrink_to_fit();
 auto cpu=read(fixture+"/native-capture-context.bin");require(cpu.size()==sizeof(context),"CPU fixture differs");std::memcpy(&context,cpu.data(),sizeof(context));context.msr&=~0x8000u;context.r[1]=ssne::guest_ram_end()-0x40000;mem_write32(context.r[1],0);
 auto cache=read(fixture+"/native-capture-locked-cache.bin");require(cache.size()==ssne::kLockedCacheSize,"Locked-cache fixture differs");std::memcpy(ssne::g_locked_cache,cache.data(),cache.size());require(ssne::native_party_load_state(read(fixture+"/native-capture-party.bin"),error),error);
 NativeWorld local;for(unsigned slot=0;slot<8;++slot)local.set_player_owner(slot,slot<2?PlayerId{slot,1}:PlayerId{});
 std::ifstream objects(fixture+"/native-capture-objects.csv");std::string line;while(std::getline(objects,line)){unsigned long long id;unsigned address,kind,definition;require(std::sscanf(line.c_str(),"%llu,%u,%u,%u",&id,&address,&kind,&definition)==4,"Invalid object metadata");if(kind==unsigned(EntityKind::Enemy))ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedActor,address,definition});if(kind==unsigned(EntityKind::Pickup))ssne::notify_native_entity({ssne::NativeEntityNotice::Event::CreatedPickup,address,definition});ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Registered,address});}
 for(unsigned slot=0;slot<ssne::native_party_capacity();++slot)ssne::notify_native_entity({ssne::NativeEntityNotice::Event::Registered,mem_read32(ssne::native_party_registry()+slot*4)});
 ssne_timebase_freeze();ssne_timebase_shift(int64_t(40500000ull*846000000ull)-int64_t(ssne_timebase()));
 ssne::netplay_set_local_player(0);const auto sample=local.capture_for_publication(1);require(sample.error.empty(),sample.error);auto source=authority(sample);NativeReplica replica(local);const auto applied=replica.apply(context,1,source);require(applied.applied,applied.error);
 EntityId enemy=0,other=0;std::map<std::tuple<uint32_t,int64_t,int64_t>,EntityId> candidates;
 for(const auto& [id,e]:source)if(e.kind==EntityKind::Enemy&&e.properties.contains("enemy_bone_matrices")&&!std::get<std::vector<float>>(e.properties.at("enemy_bone_matrices")).empty()){
  const auto key=std::tuple{e.definition,std::get<int64_t>(e.properties.at("enemy_bone_count")),std::get<int64_t>(e.properties.at("enemy_bone_layout"))};const auto [entry,fresh]=candidates.emplace(key,id);if(!fresh){enemy=entry->second;other=id;break;}}
 require(enemy!=0,"Need two real equivalent enemy skeletons for render spans");auto visual=presentation_fields(source.at(enemy));const float visual_first=std::get<std::vector<float>>(visual.properties.at("enemy_bone_matrices")).front()==.375f?.625f:.375f;std::get<std::vector<float>>(visual.properties.at("enemy_bone_matrices")).front()=visual_first;std::map<EntityId,EntityState> one{{enemy,visual}};
 const auto* mode=std::getenv("SSNE_RENDER_BONE_SPANS");const bool spans=!mode||std::strcmp(mode,"0");const auto before=digest();
 // Exercise one complete real84-enemy frame, not just the focused fault cases.
 std::map<EntityId,EntityState> full_frame;std::vector<RegistryExpectation> original_registry,visual_registry;
 std::vector<std::pair<uint64_t,uint64_t>> full_ranges;size_t bone_words=0;
 for(const auto& [id,e]:source)if(e.kind==EntityKind::Enemy){
  auto fields=presentation_fields(e);auto item=fields.properties.find("enemy_bone_matrices");
  if(item!=fields.properties.end())if(auto* values=std::get_if<std::vector<float>>(&item->second);values&&!values->empty()){
   const auto actor=replica.local_address(id);const auto fresh=native_enemy_pose_binding(actor);require(fresh&&fresh->model&&fresh->count*12==values->size(),"Full frame has a stale native skeletal binding");
   require(std::get<int64_t>(fields.properties.at("enemy_bone_count"))==fresh->count&&uint64_t(std::get<int64_t>(fields.properties.at("enemy_bone_layout")))==fresh->layout,"Full frame resource/count/layout differs");
   std::vector<float> previous;previous.reserve(values->size());for(unsigned i=0;i<values->size();++i)previous.push_back(u2f(mem_read32(fresh->matrices+i*4)));
   original_registry.push_back({actor,fresh->model,fresh->matrices,std::move(previous)});
   values->front()=.125f+float(visual_registry.size())*.0001f;visual_registry.push_back({actor,fresh->model,fresh->matrices,*values});
   full_ranges.emplace_back(fresh->matrices,uint64_t(fresh->matrices)+values->size()*4);bone_words+=values->size();
  }
  full_frame.emplace(id,std::move(fields));
 }
 require(full_frame.size()==84&&visual_registry.size()>=80,"Full frame fixture lacks84enemies/80live skeletons");
 std::sort(full_ranges.begin(),full_ranges.end());bool matrix_ranges_disjoint=true;for(size_t i=1;i<full_ranges.size();++i)matrix_ranges_disjoint&=full_ranges[i].first>=full_ranges[i-1].second;
 const auto full_before=digest();ssne::set_replica_simulation(true);require(replica.begin_presentation(full_frame,error),error);
 const auto full_during=digest();const auto full_spans=NativeReplicaRenderTest::spans(replica),full_words=NativeReplicaRenderTest::words(replica);require(full_during!=full_before,"Full real frame changed no RAM");
 require(!full_spans||(spans&&full_spans==visual_registry.size()&&matrix_ranges_disjoint),"Full frame used an incomplete/aliased span set");
 verify_registry(context,visual_registry);require(digest()==full_during,"Native registry validation leaked writes into render lease");
 replica.end_presentation();require(digest()==full_before,"Full real frame did not restore exact full RAM");verify_registry(context,original_registry);require(digest()==full_before,"Restored native pose registry does not preserve full RAM");
 const auto full_restored=digest();
 const auto* scratch_mode=std::getenv("SSNE_PRESENTATION_SCRATCH_REUSE");const bool scratch_reuse=!scratch_mode||std::strcmp(scratch_mode,"0")!=0;
 // Count only warmed lease work; hashing and fixture construction stay outside.
 require(replica.begin_presentation(full_frame,error),error);replica.end_presentation();
 const auto scratch_started=std::chrono::steady_clock::now();
 allocation_count=0;count_allocations=true;
 for(unsigned repeat=0;repeat<3;++repeat){require(replica.begin_presentation(full_frame,error),error);replica.end_presentation();}
 count_allocations=false;const auto scratch_allocations_per3=allocation_count;
 const auto scratch_nanoseconds_per3=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-scratch_started).count();
 require(digest()==full_before&&NativeReplicaRenderTest::scrubbed(replica),"Warm scratch leases changed RAM or retained old metadata");
 const auto retained_scratch_bytes=NativeReplicaRenderTest::cached_bytes(replica),retained_scratch_poses=NativeReplicaRenderTest::cached_poses(replica);
 require(retained_scratch_bytes<=8u*1024u*1024u&&retained_scratch_poses<=512,"Presentation cache exceeded retention bound");
 require(scratch_reuse?retained_scratch_bytes>0:retained_scratch_bytes==0,"Requested scratch/reference path did not execute");
 // Large/small/empty/large frames must use only each frame's active prefixes.
 require(replica.begin_presentation(one,error),error);replica.end_presentation();require(digest()==before,"Shrinking lease replayed a retained tail");
 const std::map<EntityId,EntityState> empty;
 require(replica.begin_presentation(empty,error)&&NativeReplicaRenderTest::active(replica)&&digest()==before,"Empty lease changed RAM or lost its active guard");
 require(!replica.begin_presentation(empty,error)&&!replica.apply(context,1,source).applied&&!replica.apply(1,source).applied&&digest()==before,"Empty lease allowed nested presentation/apply");
 replica.end_presentation();require(NativeReplicaRenderTest::scrubbed(replica),"Empty lease retained live metadata");
 require(replica.begin_presentation(full_frame,error),error);require(digest()==full_during,"Growing lease reused stale pose/scalar values");replica.end_presentation();require(digest()==before,"Growing lease failed exact RAM restore");

 auto invalid=one;std::get<std::vector<float>>(invalid.at(enemy).properties.at("enemy_bone_matrices")).back()=std::numeric_limits<float>::quiet_NaN();require(!replica.begin_presentation(invalid,error)&&digest()==before&&NativeReplicaRenderTest::scrubbed(replica),"Late NaN changed guest RAM or retained stale preflight metadata");
 invalid=one;std::get<std::vector<float>>(invalid.at(enemy).properties.at("enemy_bone_matrices")).front()=10001.f;require(!replica.begin_presentation(invalid,error)&&error=="Client visual pose registration failed"&&digest()==before&&NativeReplicaRenderTest::spans(replica)==0&&NativeReplicaRenderTest::scrubbed(replica),"Post-write registry reject did not restore exact RAM/stamps and clear metadata");
 // Passing a temporary map must never leave a borrowed source-vector lifetime.
 require(replica.begin_presentation(std::map<EntityId,EntityState>(one),error),error);const auto during=digest();require(during!=before,"Real visual lease changed no RAM");require(NativeReplicaRenderTest::spans(replica)==(spans?1u:0u),"Requested span/reference path did not execute");const auto scalar_words=NativeReplicaRenderTest::words(replica);
 require(!replica.begin_presentation(one,error)&&digest()==during,"Nested lease changed RAM");require(!replica.apply(context,1,source).applied&&digest()==during,"Apply invalidated live render lease");replica.end_presentation();require(digest()==before,"Real span lease did not restore exact RAM");
 const auto object=replica.local_address(enemy);const auto binding=native_enemy_pose_binding(object);require(binding&&binding->model,"Enemy binding expired");
 auto changed=one;auto& changed_values=std::get<std::vector<float>>(changed.at(enemy).properties.at("enemy_bone_matrices"));changed_values.front()=visual_first==.8125f?.9375f:.8125f;
 require(replica.begin_presentation(changed,error),error);require(mem_read32(binding->matrices)==f2u(changed_values.front()),"Warm scratch reused stale source pose values");replica.end_presentation();require(digest()==before,"Changed source pose did not restore fresh RAM");
 require(replica.begin_presentation(one,error),error);require(mem_read32(binding->matrices)==f2u(visual_first),"Scratch accumulated a prior frame's pose");replica.end_presentation();require(digest()==before,"Prior pose replay did not restore exact RAM");
 const auto root=native_render_binding(object,EntityKind::Enemy);require(root&&root->model,"Enemy root binding expired");
 const uint32_t scalar_address=root->matrix+12,bone_address=binding->matrices+4,old_scalar=mem_read32(scalar_address),old_bone=mem_read32(bone_address);
 mem_write32(scalar_address,f2u(.5625f));mem_write32(bone_address,f2u(-.4375f));const auto fresh_original=digest();
 require(replica.begin_presentation(one,error),error);replica.end_presentation();require(digest()==fresh_original,"Warm scratch restored a stale authoritative scalar/bone backup");
 mem_write32(scalar_address,old_scalar);mem_write32(bone_address,old_bone);require(digest()==before,"Fresh-backup fixture restoration changed RAM");
 const auto bytes_count=binding->count*48;const auto original_pointer=mem_read32(binding->model+0x78);const auto scratch=ssne::guest_ram_end()-0x20000;std::vector<uint8_t> saved_scratch(bytes_count);std::memcpy(saved_scratch.data(),ssne::host_ptr(scratch),bytes_count);std::memcpy(ssne::host_ptr(scratch),ssne::host_ptr(original_pointer),bytes_count);mem_write32(binding->model+0x78,scratch);const auto retarget_before=digest();
 require(replica.begin_presentation(one,error),error);require(NativeReplicaRenderTest::spans(replica)==(spans?1u:0u),"Fresh matrix retarget did not use requested path");require(mem_read32(scratch)==f2u(visual_first)&&mem_read32(original_pointer)!=f2u(visual_first),"Lease reused stale matrix RAM address");replica.end_presentation();require(digest()==retarget_before,"Retargeted lease failed exact restore");mem_write32(binding->model+0x78,original_pointer);std::memcpy(ssne::host_ptr(scratch),saved_scratch.data(),saved_scratch.size());require(digest()==before,"Retarget fixture restoration changed RAM");
 uint64_t alias_during=0;if(other){const auto other_object=replica.local_address(other);const auto second=native_enemy_pose_binding(other_object);require(second&&second->model&&second->matrices!=binding->matrices,"Need independent second enemy model");auto second_visual=presentation_fields(source.at(other));std::get<std::vector<float>>(second_visual.properties.at("enemy_bone_matrices")).front()=.875f;auto aliases=one;aliases.emplace(other,std::move(second_visual));mem_write32(second->model+0x78,binding->matrices);const auto alias_before=digest();require(replica.begin_presentation(aliases,error),error);require(NativeReplicaRenderTest::spans(replica)==0,"Aliased bone models did not take complete word fallback");alias_during=digest();replica.end_presentation();require(digest()==alias_before,"Alias fallback restore changed RAM");mem_write32(second->model+0x78,second->matrices);require(digest()==before,"Alias fixture restoration changed RAM");}
 require(other!=0,"Fixture needs two equivalent real enemy resources for alias proof");
 if(scratch_reuse){
  // Seed unused owned capacity; real lease backup/restore still uses fresh RAM.
  NativeReplicaRenderTest::exceed_budget(replica);require(replica.begin_presentation(one,error)&&NativeReplicaRenderTest::transient(replica),"Byte-budget overflow did not use transient lease storage");replica.end_presentation();require(digest()==before&&NativeReplicaRenderTest::cached_bytes(replica)==0&&NativeReplicaRenderTest::scrubbed(replica),"Byte-budget fallback retained storage or changed RAM");
  NativeReplicaRenderTest::exceed_slots(replica);require(replica.begin_presentation(one,error)&&NativeReplicaRenderTest::transient(replica),"Pose-slot overflow did not use transient lease storage");replica.end_presentation();require(digest()==before&&NativeReplicaRenderTest::cached_bytes(replica)==0&&NativeReplicaRenderTest::scrubbed(replica),"Pose-slot fallback retained storage or changed RAM");
 }
 const auto epoch_apply=replica.apply(context,2,source);require(epoch_apply.applied,epoch_apply.error);const auto epoch_before=digest();require(replica.begin_presentation(one,error),error);require(NativeReplicaRenderTest::spans(replica)==(spans?1u:0u),"Fresh epoch retained stale span state");replica.end_presentation();require(digest()==epoch_before,"Epoch lease failed exact RAM restoration");
 require(replica.begin_presentation(one,error),error);replica.reset();require(digest()==epoch_before&&NativeReplicaRenderTest::scrubbed(replica)&&NativeReplicaRenderTest::cached_bytes(replica)==0&&!replica.local_address(enemy),"Reset during lease did not restore RAM and release owned scratch");
 std::cout<<"{\"passed\":true,\"presentation_scratch_reuse\":"<<(scratch_reuse?"true":"false")<<",\"scratch_allocations_per3\":"<<scratch_allocations_per3<<",\"scratch_nanoseconds_per3\":"<<scratch_nanoseconds_per3<<",\"retained_scratch_bytes\":"<<retained_scratch_bytes<<",\"retained_scratch_poses\":"<<retained_scratch_poses<<",\"scratch_metadata_scrubbed\":true,\"fresh_source_values\":true,\"fresh_authoritative_backups\":true,\"shrink_empty_grow\":true,\"empty_lease_guard\":true,\"budget_and_slot_fallback\":true,\"reset_active_lease\":true,\"render_bone_spans\":"<<(spans?"true":"false")<<",\"original_ram_digest\":"<<before<<",\"visual_ram_digest\":"<<during<<",\"alias_visual_ram_digest\":"<<alias_during<<",\"full_frame_original_ram_digest\":"<<full_before<<",\"full_frame_visual_ram_digest\":"<<full_during<<",\"full_frame_restored_ram_digest\":"<<full_restored<<",\"full_frame_enemies\":"<<full_frame.size()<<",\"full_frame_bound_skeletons\":"<<visual_registry.size()<<",\"full_frame_bone_words\":"<<bone_words<<",\"full_frame_spans\":"<<full_spans<<",\"full_frame_scalar_backup_words\":"<<full_words<<",\"full_frame_matrix_ranges_disjoint\":"<<(matrix_ranges_disjoint?"true":"false")<<",\"full_frame_native_registry_before_after\":true,\"full_frame_alias_fallback\":"<<((spans&&!full_spans)?"true":"false")<<",\"scalar_backup_words\":"<<scalar_words<<",\"late_nan_atomic\":true,\"registry10001_rollback_exact\":true,\"temporary_source_owned\":true,\"nested_apply_guard\":true,\"fresh_matrix_address\":true,\"real_model_alias_fallback\":true,\"epoch_fresh\":true,\"restore_exact\":true}\n";std::cout.flush();std::_Exit(0);
}
