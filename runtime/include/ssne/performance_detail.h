#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <ostream>
#include <string>

// Sample whole operations, not individual vertices. No logging or locks in
// nested scopes. Inclusive spans overlap; self spans partition the sampled root.
namespace ssne::perf::detail {
using Clock=std::chrono::steady_clock;
int64_t thread_cpu_ns(); // -1 when unsupported; includes kernel and user time.
enum RootId { GuestRender, GuestFlip, Presentation, Reprojection, ReplicaApply, UiSubmission, RootCount };
inline constexpr std::array root_names={"guest_render","guest_flip","presentation","reprojection","replica_apply","ui_submission"};
enum Stage { Viewport, ModelDraw, Frustum, VisibilityProjection, ListDraw, SkeletonPose,
    DisplayListCopy, VertexDecode, DrawRecord, TextureLookup, TextureDecode,
    FrameSubmit, QueueLock, QueueBackpressure, FramePool, FrameTake, FrameRecycle,
    InterpolationPrepare, InterpolationApply, Execute, VertexUpload, DriverDraw,
    TextureUpload, ShaderCompile, EfbCopy,
    CaptureBindings, Preflight, PropertyPlan, Creation, Scalars, Destructibles,
    Hud, Markers, PlayerResources, WeaponEffects, Skeletons, Retirement,
    FifoContendedLock,DrawConfig,DrawPack,DrawState,DrawDepth,DrawBatch,Native493d0,Native2203c,Native266c8,Native42d94,Native43308,Native4cc6c,Native1818c,InterpolationMaterialize,InterpolationMatch,InterpolationNativeCopy,ShaderLookup,TextureBindings,XfUniforms,DrawUniforms,DrawSubmission,ExecuteNative,ExecutePredicted,UiQuery,UiUpload,UiDraw,UiVao,UiProgramCheck,VertexCommon,VertexFixed,VertexPlanFloat,VertexPlanQuantized,VertexGeneric,VertexClassic,PackedBlock,PackedPreflight,VertexPlanBuild,RenderVisualSample,RenderVisualApply,RenderVisualRestore,InterpolationMatchSetup,InterpolationMatchLookup,InterpolationMatchAttributes,InterpolationMatchPositions,StageCount };
inline constexpr std::array stage_names={"viewport","model_draw","frustum","visibility_projection","list_draw","skeleton_pose",
    "display_list_copy","vertex_decode","draw_record","texture_lookup","texture_decode",
    "frame_submit","queue_lock","queue_backpressure","frame_pool","frame_take","frame_recycle",
    "interpolation_prepare","interpolation_apply","execute","vertex_upload","driver_draw",
    "texture_upload","shader_compile","efb_copy",
    "capture_bindings","preflight","property_plan","creation","scalars","destructibles",
    "hud","markers","player_resources","weapon_effects","skeletons","retirement",
    "fifo_contended_lock","draw_config","draw_pack","draw_state","draw_depth","draw_batch","native_801493d0","native_8012203c","native_801266c8","native_80142d94","native_80143308","native_8014cc6c","native_8011818c","interpolation_materialize","interpolation_match","interpolation_native_copy","shader_lookup","texture_bindings","xf_uniforms","draw_uniforms","draw_submission","execute_native","execute_predicted","ui_query","ui_upload","ui_draw","ui_vao","ui_program_check","vertex_common","vertex_fixed","vertex_plan_float","vertex_plan_quantized","vertex_generic","vertex_classic","packed_block","packed_preflight","vertex_plan_build","render_visual_sample","render_visual_apply","render_visual_restore","interpolation_match_setup","interpolation_match_lookup","interpolation_match_attributes","interpolation_match_positions"};
enum Work { Vertices, DisplayListBytes, Draws, TextureMisses, TexturePixels, VertexBytes, IndexBytes,
    Commands, RepeatedExecutes, Entities, Properties, BindingCandidates, FifoContentions, BatchedClipVertices, InterpolationNativeBytes, PackedPredictionUploads, CommonVertices, FixedVertices, PlannedVertices, FallbackVertices, PackedBlocks, PlannedPrimitives, DirectPrimitives, VertexPlanHits, VertexPlanMisses, WorkCount };
inline constexpr std::array work_names={"vertices","display_list_bytes","draws","texture_misses","texture_pixels",
    "vertex_bytes","index_bytes","commands","repeated_executes","entities","properties","binding_candidates","fifo_contentions","batched_clip_vertices","interpolation_native_bytes","packed_prediction_uploads","common_vertices","fixed_vertices","planned_vertices","fallback_vertices","packed_blocks","planned_primitives","direct_primitives","vertex_plan_hits","vertex_plan_misses"};
struct Span {uint64_t calls=0,inclusive=0,self=0;};
struct Context {uint64_t frame=0;unsigned scene=0;std::string level;std::array<float,3> position{};};
struct Batch {std::array<Span,StageCount> spans{};std::array<uint64_t,WorkCount> work{};};
struct Sample {uint64_t wall=0,cpu=0;bool cpu_valid=false;Context context;Batch batch;};
struct Totals {uint64_t seen=0,samples=0,wall=0,cpu=0,cpu_samples=0,max_wall=0,root_self=0;Batch batch;Sample worst;};
inline std::mutex mutex;
inline Context current_context;
inline std::array<Totals,RootCount> totals;
inline std::atomic<unsigned> period{0}; // 0 disables detail without disabling coarse logging.
inline thread_local Batch* active=nullptr;
inline thread_local std::array<uint64_t,RootCount> sequences{};
struct Scope;
inline thread_local Scope* parent=nullptr;
inline void configure(){
    const char* value=std::getenv("SSNE_PERF_DETAIL_EVERY");
    unsigned n=16;
    if(value){char* end=nullptr;const auto parsed=std::strtol(value,&end,10);if(end!=value && !*end && parsed>=0 && parsed<=256)n=unsigned(parsed);}
    period.store(n,std::memory_order_relaxed);
}
inline void set_context(Context context){if(!period.load(std::memory_order_relaxed))return;std::lock_guard lock(mutex);current_context=std::move(context);}
inline void count(Work id,uint64_t value=1){if(active)active->work[id]+=value;}
inline uint64_t ns(Clock::duration d){return uint64_t(std::max<int64_t>(0,std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()));}
// One sample per block, cycling through every offset. A fixed multiple of 16
// aliases alternating cheap/native and expensive/corrected presentations.
inline bool select_sample(uint64_t sequence,unsigned n){
    if(!n || !sequence)return false;
    const auto index=sequence-1;
    return index%n==(index/n)%n;
}
struct Scope {
    Stage id;Batch* batch;Scope* previous=nullptr;Clock::time_point start{};uint64_t children=0;
    explicit Scope(Stage stage):id(stage),batch(active){if(batch){previous=parent;parent=this;start=Clock::now();}}
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
    void finish(){if(batch){const auto elapsed=ns(Clock::now()-start);auto& span=batch->spans[id];++span.calls;span.inclusive+=elapsed;span.self+=elapsed-std::min(elapsed,children);parent=previous;if(previous)previous->children+=elapsed;batch=nullptr;}}
    void next(Stage stage){finish();id=stage;batch=active;children=0;if(batch){previous=parent;parent=this;start=Clock::now();}}
    ~Scope(){finish();}
};
struct Frame {
    RootId id;Batch batch;bool sampled=false;Clock::time_point start{};int64_t cpu_start=-1;Context context;
    explicit Frame(RootId root):id(root){
        const auto n=period.load(std::memory_order_relaxed);if(!n || active)return;
        const bool chosen=select_sample(++sequences[id],n);
        {std::lock_guard lock(mutex);++totals[id].seen;if(chosen)context=current_context;}
        if(!chosen)return;
        sampled=true;active=&batch;parent=nullptr;cpu_start=thread_cpu_ns();start=Clock::now();
    }
    Frame(const Frame&)=delete;Frame& operator=(const Frame&)=delete;
    ~Frame(){if(!sampled)return;
        const auto wall=ns(Clock::now()-start);const auto cpu_end=thread_cpu_ns();active=nullptr;parent=nullptr;
        const bool valid=cpu_start>=0 && cpu_end>=cpu_start;const auto cpu=valid?uint64_t(cpu_end-cpu_start):0;
        std::lock_guard lock(mutex);auto& t=totals[id];++t.samples;t.wall+=wall;t.cpu+=cpu;t.cpu_samples+=valid;t.max_wall=std::max(t.max_wall,wall);
        uint64_t child_self=0;for(unsigned i=0;i<StageCount;++i){auto& out=t.batch.spans[i];const auto& in=batch.spans[i];out.calls+=in.calls;out.inclusive+=in.inclusive;out.self+=in.self;child_self+=in.self;}
        t.root_self+=wall-std::min(wall,child_self);
        for(unsigned i=0;i<WorkCount;++i)t.batch.work[i]+=batch.work[i];
        if(wall>=t.worst.wall)t.worst={wall,cpu,valid,std::move(context),batch};
    }
};
// Called only by the existing bounded once-per-second writer.
template<class Quote> void append_json(std::ostream& out,Quote quote){
    std::array<Totals,RootCount> copy;{std::lock_guard lock(mutex);copy=std::move(totals);totals={};}
    out<<",\"detail\":{\"every_n\":"<<period.load()<<",\"sampling\":\"rotating\",\"roots\":{";bool first=true;
    auto batch_json=[&](const Batch& b,uint64_t frames){
        out<<"\"spans\":{";bool a=true;for(unsigned i=0;i<StageCount;++i){const auto& s=b.spans[i];if(!s.calls)continue;if(!a)out<<',';a=false;
            out<<quote(stage_names[i])<<":{\"calls\":"<<s.calls<<",\"inclusive_ms\":"<<s.inclusive/1e6<<",\"self_ms\":"<<s.self/1e6<<",\"self_ms_per_sample\":"<<s.self/1e6/std::max<uint64_t>(1,frames)<<'}';}
        out<<"},\"work\":{";a=true;for(unsigned i=0;i<WorkCount;++i){if(!b.work[i])continue;if(!a)out<<',';a=false;out<<quote(work_names[i])<<':'<<b.work[i];}out<<'}';
    };
    for(unsigned i=0;i<RootCount;++i){const auto& t=copy[i];if(!t.seen && !t.samples)continue;if(!first)out<<',';first=false;
        out<<quote(root_names[i])<<":{\"seen\":"<<t.seen<<",\"samples\":"<<t.samples<<",\"wall_ms\":"<<t.wall/1e6<<",\"thread_cpu_ms\":"<<t.cpu/1e6<<",\"cpu_samples\":"<<t.cpu_samples<<",\"max_wall_ms\":"<<t.max_wall/1e6<<",\"unattributed_self_ms\":"<<t.root_self/1e6<<',';batch_json(t.batch,t.samples);
        if(t.samples){const auto& w=t.worst;out<<",\"worst\":{\"wall_ms\":"<<w.wall/1e6<<",\"thread_cpu_ms\":"<<w.cpu/1e6<<",\"cpu_valid\":"<<w.cpu_valid<<",\"guest_frame\":"<<w.context.frame<<",\"scene\":"<<w.context.scene<<",\"level\":"<<quote(w.context.level)<<",\"position\":["<<w.context.position[0]<<','<<w.context.position[1]<<','<<w.context.position[2]<<"],";batch_json(w.batch,1);out<<'}';}
        out<<'}';
    }out<<"}}";
}
static_assert(stage_names.size()==StageCount && work_names.size()==WorkCount && root_names.size()==RootCount);
}
