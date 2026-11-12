// Render-only interpolation. Never re-enter guest code or create EFB-copy revisions.
#pragma once
#include "gx_gpu_frame.h"
#include "ssne/performance_detail.h"
#include <cmath>
#include <cstring>
#include <unordered_map>
namespace ssne::gx {
inline double frame_motion_period(const GpuFrame* previous,const GpuFrame& current){
    if(!previous || !previous->produced_at || !current.produced_at)return 1./59.94; // older recorded captures
    const auto interval=current.produced_at-previous->produced_at;
    return interval>=.001 && interval<=.10?interval:0.;
}
struct FrameInterpolation {
    bool batch_reprojection=false; // selected by the renderer; scalar remains the reference
    struct Diagnostics {unsigned pairs=0,shape=0,state=0,texture=0,color=0,uv0=0,uv_extra=0;};
    Diagnostics diagnostics;
    struct Position {uint32_t offset; float from[4],to[4];};
    std::vector<Position> positions;
    struct CameraSpan {uint32_t offset,stride,count;std::array<float,6> projection;};
    std::vector<CameraSpan> camera_vertices;
    std::array<float,12> camera_previous{},camera_current{};
    struct Reprojection {
        std::array<float,12> eye;
        Reprojection(const std::array<float,12>& from,const std::array<float,12>& to){
            // Combine camera-to-world and world-to-camera once for the frame.
            // GX reflects Z on both sides of the native camera convention.
            for(unsigned r=0;r<3;++r){
                for(unsigned c=0;c<3;++c){float value=0;for(unsigned k=0;k<3;++k)value+=to[k*4+r]*from[k*4+c];eye[r*4+c]=value*(r==2?-1.f:1.f)*(c==2?-1.f:1.f);}
                float value=0;for(unsigned k=0;k<3;++k)value+=to[k*4+r]*(from[k*4+3]-to[k*4+3]);eye[r*4+3]=value*(r==2?-1.f:1.f);
            }
        }
        bool apply(float point[4],const float projection[6])const{
            if(!std::isfinite(point[3]) || point[3]<=.02f || std::abs(projection[0])<1e-6f || std::abs(projection[2])<1e-6f)return false;
            const float z=-point[3];const float input[3]={(point[0]-projection[1]*z)/projection[0],(point[1]-projection[3]*z)/projection[2],z};
            float next[3];for(unsigned r=0;r<3;++r)next[r]=eye[r*4]*input[0]+eye[r*4+1]*input[1]+eye[r*4+2]*input[2]+eye[r*4+3];
            const float result[4]={projection[0]*next[0]+projection[1]*next[2],projection[2]*next[1]+projection[3]*next[2],projection[4]*next[2]+projection[5],-next[2]};
            for(auto v:result)if(!std::isfinite(v))return false;if(result[3]<=.02f)return false;
            std::memcpy(point,result,16);return true;
        }
    };
    // Compose clip->eye->new eye->clip once per draw. All vertices in the
    // draw share its projection; only X/Y/W are needed to recover eye space.
    struct ClipReprojection {
        std::array<float,16> matrix{};bool valid=true;
        ClipReprojection(const Reprojection& r,const float p[6]){
            valid=std::abs(p[0])>=1e-6f && std::abs(p[2])>=1e-6f;if(!valid)return;
            float eye[12];for(unsigned row=0;row<3;++row){const auto i=row*4;
                eye[i]=r.eye[i]/p[0];eye[i+1]=r.eye[i+1]/p[2];
                eye[i+2]=eye[i]*p[1]+eye[i+1]*p[3]-r.eye[i+2];eye[i+3]=r.eye[i+3];
            }
            for(unsigned col=0;col<4;++col){
                matrix[col]=p[0]*eye[col]+p[1]*eye[8+col];
                matrix[4+col]=p[2]*eye[4+col]+p[3]*eye[8+col];
                matrix[8+col]=p[4]*eye[8+col]+(col==3?p[5]:0.f);matrix[12+col]=-eye[8+col];
            }
            for(auto x:matrix)valid&=std::isfinite(x);
        }
        bool apply(float point[4])const{
            if(!valid || !std::isfinite(point[3]) || point[3]<=.02f)return false;
            float result[4];for(unsigned row=0;row<4;++row){const auto i=row*4;
                result[row]=matrix[i]*point[0]+matrix[i+1]*point[1]+matrix[i+2]*point[3]+matrix[i+3];
            }
            for(auto x:result)if(!std::isfinite(x))return false;if(result[3]<=.02f)return false;
            std::memcpy(point,result,16);return true;
        }
        unsigned apply_four(float points[4][4])const{
#if defined(__SSE2__) || defined(_M_X64)
            if(!valid)return 0;
            __m128 x,y,z,w;
            std::memcpy(&x,points[0],16);std::memcpy(&y,points[1],16);
            std::memcpy(&z,points[2],16);std::memcpy(&w,points[3],16);
            _MM_TRANSPOSE4_PS(x,y,z,w);
            const auto exponent=_mm_set1_epi32(0x7f800000);
            auto nonfinite=[&](__m128 a){return _mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(a),exponent),exponent);};
            auto bad=nonfinite(w);
            auto accepted=_mm_cmpgt_ps(w,_mm_set1_ps(.02f));
            __m128 out[4];
            // Each SIMD lane is a different vertex. Keep the scalar expression's
            // multiply/add order; no reciprocal approximation or fused operation.
            for(unsigned row=0;row<4;++row){const auto i=row*4;
                out[row]=_mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(matrix[i]),x),
                    _mm_mul_ps(_mm_set1_ps(matrix[i+1]),y)),_mm_mul_ps(_mm_set1_ps(matrix[i+2]),w)),_mm_set1_ps(matrix[i+3]));
                bad=_mm_or_si128(bad,nonfinite(out[row]));
            }
            accepted=_mm_andnot_ps(_mm_castsi128_ps(bad),_mm_and_ps(accepted,_mm_cmpgt_ps(out[3],_mm_set1_ps(.02f))));
            const auto mask=unsigned(_mm_movemask_ps(accepted));
            _MM_TRANSPOSE4_PS(out[0],out[1],out[2],out[3]);
            for(unsigned v=0;v<4;++v)if(mask&(1u<<v))std::memcpy(points[v],&out[v],16);
            return mask;
#else
            unsigned mask=0;for(unsigned v=0;v<4;++v)if(apply(points[v]))mask|=1u<<v;return mask;
#endif
        }
    };
    static bool reproject(float point[4],const float projection[6],const std::array<float,12>& from,const std::array<float,12>& to){
        if(!std::isfinite(point[3]) || point[3]<=.02f || std::abs(projection[0])<1e-6f || std::abs(projection[2])<1e-6f)return false;
        const float z=-point[3];const float eye[3]={(point[0]-projection[1]*z)/projection[0],(point[1]-projection[3]*z)/projection[2],z};
        float world[3],next[3]{};
        // PAL viewport8014C9F0 inverts the camera and then NEGATES its Z row.
        // Native cameras face +Z; GX eye space faces -Z. Undo/reapply that
        // reflection on BOTH sides, matching the original viewport exactly.
        for(unsigned r=0;r<3;++r){world[r]=from[r*4+3];for(unsigned c=0;c<3;++c)world[r]+=from[r*4+c]*(c==2?-eye[c]:eye[c]);}
        for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)next[r]+=to[c*4+r]*(world[c]-to[c*4+3]);
        next[2]=-next[2];
        float result[4]={projection[0]*next[0]+projection[1]*next[2],projection[2]*next[1]+projection[3]*next[2],projection[4]*next[2]+projection[5],-next[2]};
        for(const auto v:result)if(!std::isfinite(v))return false;if(result[3]<=.02f)return false;
        std::memcpy(point,result,16);return true;
    }
    bool available()const{return !positions.empty() || !camera_vertices.empty();}
    static bool camera_draw(const GpuState& state){
        return state.world_camera && !state.camera_locked && state.perspective && !state.local_ui &&
            !(std::abs(state.projection[0])<1e-6f || std::abs(state.projection[2])<1e-6f);
    }
    static bool camera_possible(const GpuFrame& frame){
        if(!frame.local_camera || !frame.camera_valid)return false;
        return std::any_of(frame.cmds.begin(),frame.cmds.end(),[&](const auto& cmd){
            return cmd.type==GpuCmd::DRAW && camera_draw(frame.states[cmd.state]);
        });
    }
    size_t camera_vertex_count()const{size_t count=0;for(const auto& span:camera_vertices)count+=span.count;return count;}
    void clear(){positions.clear();camera_vertices.clear();}
    void append_position(const Position& point){
        if(!std::memcmp(point.from,point.to,16))return;
        bool safe=point.from[3]>0 && point.to[3]>0;
        const float w=std::min(point.from[3],point.to[3]);
#if defined(__SSE2__) || defined(_M_X64)
        __m128 from,to;std::memcpy(&from,point.from,16);std::memcpy(&to,point.to,16);
        const auto magnitude=_mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
        const auto exponent=_mm_set1_epi32(0x7f800000);
        const auto finite_from=_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(from),exponent),exponent);
        const auto finite_to=_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(to),exponent),exponent);
        safe &= !_mm_movemask_epi8(_mm_or_si128(finite_from,finite_to)) &&
            _mm_movemask_ps(_mm_cmplt_ps(_mm_and_ps(_mm_sub_ps(from,to),magnitude),_mm_set1_ps(w*.5f)))==15;
#else
        for(unsigned j=0;j<4;++j)safe &= std::isfinite(point.from[j]) && std::isfinite(point.to[j]) && std::abs(point.from[j]-point.to[j])<w*.5f;
#endif
        if(safe)positions.push_back(point);
    }
    void prepare(const GpuFrame* previous,const GpuFrame& current,const PackedPositionLayout* layout=nullptr) {
        ssne::perf::detail::Scope setup_scope(ssne::perf::detail::InterpolationMatchSetup);
        clear();
        diagnostics={};
        const bool compatible=previous && previous->local_camera && current.local_camera && previous->wide==current.wide && previous->presentation_generation==current.presentation_generation && current.seq==previous->seq+1 && frame_motion_period(previous,current)>0;
        size_t prefix=0;for(size_t i=0;i<current.cmds.size();++i)if(current.cmds[i].type==GpuCmd::COPY_TEX)prefix=i+1;
        camera_current=current.camera;camera_previous=current.camera;
        bool camera_motion=compatible && previous->camera_valid && current.camera_valid;
        if(camera_motion){
            camera_previous=previous->camera;camera_current=current.camera;float distance=0,forward=0,right=0;
            for(unsigned r=0;r<3;++r){const auto d=camera_current[r*4+3]-camera_previous[r*4+3];distance+=d*d;forward+=camera_current[r*4+2]*camera_previous[r*4+2];right+=camera_current[r*4]*camera_previous[r*4];}
            const float limit=float(std::max(1.,60.*frame_motion_period(previous,current)));
            camera_motion=std::isfinite(distance) && distance<limit*limit && forward>.7f && right>.7f;
            for(auto v:camera_current)camera_motion&=std::isfinite(v);for(auto v:camera_previous)camera_motion&=std::isfinite(v);
        }
        // An identical camera is an identity transform, including when its native
        // float basis is not perfectly orthonormal. Reprojecting through its
        // transpose invents motion and work for every stationary world vertex.
        camera_motion = camera_motion && camera_previous != camera_current;
        if(!camera_motion)camera_previous=camera_current;
        const Reprojection previous_to_current(camera_previous,camera_current);
        // Native EFB copies also follow the WORLD draw. These copy revisions
        // remain canonical, but the preceding world geometry must still move
        // in extra display frames. Otherwise only the gun after the last copy
        // receives the predicted camera.
        if(current.local_camera && current.camera_valid)for(size_t i=0;i<current.cmds.size();++i){
            const auto& draw=current.cmds[i];if(draw.type!=GpuCmd::DRAW)continue;const auto& state=current.states[draw.state];
            if(!camera_draw(state))continue;
            CameraSpan span{layout?uint32_t(layout->offsets[i]):draw.vtx_off,layout?16u:draw.stride,draw.vcount,{}};std::copy_n(state.projection,6,span.projection.begin());camera_vertices.push_back(span);
        }
        if(!compatible)return;
        // Frame-local open addressing avoids allocating one hash node for
        // every proxy and draw in each of three separate maps. Nonzero proxy
        // identities make zero an unambiguous empty key. Keep load below 1/2;
        // ordinal counters are shared by both frame scans, never guest state.
        struct Ordinal {uint32_t proxy=0,previous=0,current=0;};
        struct PriorDraw {uint64_t key=0;const GpuCmd* command=nullptr;};
        size_t previous_count=0,current_count=0;
        for(const auto& cmd:previous->cmds)if(cmd.type==GpuCmd::DRAW && previous->states[cmd.state].render_proxy)++previous_count;
        for(const auto& cmd:current.cmds)if(cmd.type==GpuCmd::DRAW && current.states[cmd.state].render_proxy)++current_count;
        size_t ordinal_capacity=2,draw_capacity=2;
        while(ordinal_capacity<2*(previous_count+current_count))ordinal_capacity*=2;
        while(draw_capacity<2*previous_count)draw_capacity*=2;
        const auto ordinal_mask=ordinal_capacity-1,draw_mask=draw_capacity-1;
        std::vector<Ordinal> ordinals(ordinal_capacity);
        std::vector<PriorDraw> prior_draws(draw_capacity);
        auto bucket=[](uint64_t key){key^=key>>33;key*=0xff51afd7ed558ccdULL;key^=key>>33;return size_t(key);};
        auto ordinal=[&](uint32_t proxy)->Ordinal&{auto i=bucket(proxy)&ordinal_mask;while(ordinals[i].proxy && ordinals[i].proxy!=proxy)i=(i+1)&ordinal_mask;ordinals[i].proxy=proxy;return ordinals[i];};
        auto prior_slot=[&](uint64_t key)->PriorDraw&{auto i=bucket(key)&draw_mask;while(prior_draws[i].key && prior_draws[i].key!=key)i=(i+1)&draw_mask;return prior_draws[i];};
        for(const auto& cmd:previous->cmds)if(cmd.type==GpuCmd::DRAW){const auto proxy=previous->states[cmd.state].render_proxy;if(proxy){const auto key=(uint64_t(proxy)<<32)|ordinal(proxy).previous++;auto& slot=prior_slot(key);slot.key=key;slot.command=&cmd;}}
        setup_scope.finish();
        for(size_t i=0;i<current.cmds.size();++i) {
            const auto& b=current.cmds[i];if(b.type!=GpuCmd::DRAW)continue;
            ssne::perf::detail::Scope match_scope(ssne::perf::detail::InterpolationMatchLookup);
            const auto proxy=current.states[b.state].render_proxy;
            const GpuCmd* prior=nullptr;
            if(proxy){const auto key=(uint64_t(proxy)<<32)|ordinal(proxy).current++;const auto& found=prior_slot(key);if(found.key==key)prior=found.command;}
            else if(i<previous->cmds.size() && previous->cmds[i].type==GpuCmd::DRAW && !previous->states[previous->cmds[i].state].render_proxy)prior=&previous->cmds[i];
            if(!prior){++diagnostics.shape;continue;}const auto& a=*prior;
            ++diagnostics.pairs;
            if(a.type!=GpuCmd::DRAW || b.type!=GpuCmd::DRAW || a.stride!=b.stride ||
               a.vcount!=b.vcount || a.idx_count!=b.idx_count || a.ntex!=b.ntex){++diagnostics.shape;continue;}
            const auto& sa=previous->states[a.state];const auto& sb=current.states[b.state];
            // World objects are drawn before the final native EFB copy too.
            // Extra frames skip copies, so matching world geometry can advance
            // without modifying the guest's canonical copy or UI/offscreen work.
            if(i<prefix && !(sa.world_camera && sb.world_camera))continue;
            if(!sa.perspective || !sb.perspective || sa.local_ui || sb.local_ui || sa.camera_locked || sb.camera_locked ||
               std::memcmp(sa.vp,sb.vp,sizeof(sa.vp)) || sa.cull!=sb.cull){++diagnostics.state;continue;}
            bool same=true;
            for(unsigned t=0;t<8;++t)if(sa.cfg.tex[t]!=sb.cfg.tex[t])same=false;
            if(!same){++diagnostics.texture;continue;}
            match_scope.next(ssne::perf::detail::InterpolationMatchAttributes);
            if(std::memcmp(previous->idx.data()+a.idx_first,current.idx.data()+b.idx_first,b.idx_count*4)){++diagnostics.shape;continue;}
            // Identical immutable payloads need no object extrapolation. This
            // comparison uses the current two frames, never cached guest RAM;
            // a changed camera still takes the original reprojection path.
            if(!camera_motion && !std::memcmp(previous->vtx.data()+a.vtx_off,
                    current.vtx.data()+b.vtx_off,size_t(b.stride)*b.vcount))continue;
            // Colour/UV/topology changes, including animated meshes, use the new
            // native frame. Matching geometry gets continuous camera/movement.
            for(unsigned v=0;v<b.vcount;++v) {
                const auto* av=previous->vtx.data()+a.vtx_off+v*a.stride;
                const auto* bv=current.vtx.data()+b.vtx_off+v*b.stride;
                if(std::memcmp(av+16,bv+16,8)){
                    ++diagnostics.color;
                    // Lighting/fade can change while the same indexed model
                    // moves. Keep its CURRENT colours; they do not invalidate
                    // position continuity. Unidentified effects stay guarded.
                    if(!(proxy && sa.world_camera && sb.world_camera)){same=false;break;}
                }
                if(b.ntex && std::memcmp(av+24,bv+24,12)){++diagnostics.uv0;same=false;break;}
                if(b.ntex>1 && std::memcmp(av+36,bv+36,12*(b.ntex-1))){++diagnostics.uv_extra;same=false;break;}
            }
            if(!same)continue;
            // Every vertex in a draw uses the same two cameras and projection.
            // Compose their transform once; the old inner loop inverted and
            // reapplied both cameras for every vertex of each matching mesh.
            if(camera_motion && sa.world_camera && sb.world_camera &&
               std::memcmp(sa.projection,sb.projection,sizeof(sa.projection)))continue;
            match_scope.next(ssne::perf::detail::InterpolationMatchPositions);
            const ClipReprojection previous_clip(previous_to_current,sa.projection);
            unsigned v=0;
            if(batch_reprojection && camera_motion && sa.world_camera && sb.world_camera){
                for(;v+4<=b.vcount;v+=4){
                    float from[4][4];for(unsigned k=0;k<4;++k)std::memcpy(from[k],previous->vtx.data()+a.vtx_off+(v+k)*a.stride,16);
                    const auto valid=previous_clip.apply_four(from);
                    for(unsigned k=0;k<4;++k)if(valid&(1u<<k)){
                        Position point{};point.offset=b.vtx_off+(v+k)*b.stride;
                        std::memcpy(point.from,from[k],16);std::memcpy(point.to,current.vtx.data()+point.offset,16);
                        if(layout)point.offset=uint32_t(layout->offsets[i]+size_t(v+k)*16);
                        append_position(point);
                    }
                }
                ssne::perf::detail::count(ssne::perf::detail::BatchedClipVertices,v);
            }
            for(;v<b.vcount;++v) {
                Position point{};point.offset=b.vtx_off+v*b.stride;
                std::memcpy(point.from,previous->vtx.data()+a.vtx_off+v*a.stride,16);
                std::memcpy(point.to,current.vtx.data()+point.offset,16);
                // Remove camera movement from matched world vertices before
                // predicting object motion; the whole camera is applied once below.
                if(camera_motion && sa.world_camera && sb.world_camera){
                    if(!previous_clip.apply(point.from))continue;
                }
                // No continuity entry is emitted for bit-identical endpoints.
                // Reject them before the finite/distance checks, including when
                // lighting changed elsewhere in an otherwise stationary mesh.
                if(layout)point.offset=uint32_t(layout->offsets[i]+size_t(v)*16);
                append_position(point);
            }
        }
    }
    // Commands/textures remain immutable until the next canonical frame. If
    // reprojection produces the exact already presented vertex bytes, another
    // full GX submission would only redraw the same image. Reuse the scratch
    // allocation and retain the last presented bytes for an exact comparison.
    template<class Bytes> bool apply_if_changed(Bytes& presented,Bytes& scratch,
                          const Bytes& native,float alpha,
                          const std::array<float,12>* look=nullptr,bool force=false) const {
        scratch=native;apply(scratch,alpha,look);
        if(!force && scratch==presented)return false;
        presented.swap(scratch);return true;
    }
    template<class Bytes> void apply(Bytes& vertices,float alpha,const std::array<float,12>* look=nullptr) const {
        alpha=std::clamp(alpha,0.f,2.f);
        for(const auto& point:positions) {
            float value[4];for(unsigned j=0;j<4;++j)value[j]=point.from[j]+(point.to[j]-point.from[j])*alpha;
            std::memcpy(vertices.data()+point.offset,value,16);
        }
        if(camera_vertices.empty() || (alpha<=1.f && !look))return;
        if(!look && camera_previous==camera_current)return;
        auto predicted=camera_current;const float lead=alpha-1.f;
        for(unsigned i=0;i<12;++i)predicted[i]+=(camera_current[i]-camera_previous[i])*lead;
        // Re-orthogonalize the predicted basis. Position prediction is bounded
        // to one measured frame; callers stop on stalls and camera generations.
        float forward[3]={predicted[2],predicted[6],predicted[10]},right[3]={predicted[0],predicted[4],predicted[8]};
        auto normalize=[](float v[3]){const float n=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(n<1e-6f || !std::isfinite(n))return false;for(unsigned i=0;i<3;++i)v[i]/=n;return true;};
        if(!normalize(forward))return;float dot=0;for(unsigned i=0;i<3;++i)dot+=right[i]*forward[i];for(unsigned i=0;i<3;++i)right[i]-=dot*forward[i];if(!normalize(right))return;
        const float up[3]={forward[1]*right[2]-forward[2]*right[1],forward[2]*right[0]-forward[0]*right[2],forward[0]*right[1]-forward[1]*right[0]};
        for(unsigned r=0;r<3;++r){predicted[r*4]=right[r];predicted[r*4+1]=up[r];predicted[r*4+2]=forward[r];}
        if(look)for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)predicted[r*4+c]=(*look)[r*4+c];
        if(predicted==camera_current)return;
        const Reprojection reprojection(camera_current,predicted);
        for(const auto& span:camera_vertices){
            const ClipReprojection clip(reprojection,span.projection.data());
            unsigned i=0;
            if(batch_reprojection){
                for(;i+4<=span.count;i+=4){
                    float points[4][4];for(unsigned k=0;k<4;++k)std::memcpy(points[k],vertices.data()+span.offset+(i+k)*span.stride,16);
                    const auto valid=clip.apply_four(points);
                    for(unsigned k=0;k<4;++k)if(valid&(1u<<k))std::memcpy(vertices.data()+span.offset+(i+k)*span.stride,points[k],16);
                }
                ssne::perf::detail::count(ssne::perf::detail::BatchedClipVertices,i);
            }
            for(;i<span.count;++i){const auto offset=span.offset+i*span.stride;float point[4];std::memcpy(point,vertices.data()+offset,16);if(clip.apply(point))std::memcpy(vertices.data()+offset,point,16);}
        }
    }
};

// Keep the last two immutable native recordings, but do no CPU vertex work
// until an additional presentation actually needs it. advance() returns the
// obsolete recording to the renderer's pool. Prediction never becomes history.
struct DeferredFrameInterpolation {
    std::unique_ptr<GpuFrame> frame,previous;
    FrameInterpolation motion;
    VertexBytes native,scratch,packed_presented;
    PackedPositionLayout layout;
    bool packed_positions=false;
    double period=1./59.94;
    bool prepared=false;

    std::unique_ptr<GpuFrame> advance(std::unique_ptr<GpuFrame> next){
        auto retired=std::move(previous);
        if(frame && prepared && !packed_positions)frame->vtx.swap(native);
        previous=std::move(frame);frame=std::move(next);
        period=frame_motion_period(previous.get(),*frame);
        prepared=false;motion.clear();
        return retired;
    }
    VertexBytes& presented(){return packed_positions?packed_presented:frame->vtx;}
    void prepare(){
        if(prepared || !frame)return;
        ssne::perf::detail::Scope prepare_scope(ssne::perf::detail::InterpolationPrepare);
        {
            ssne::perf::detail::Scope materialize_scope(ssne::perf::detail::InterpolationMaterialize);
            if(previous)materialize_positions(*previous);
            materialize_positions(*frame);
        }
        {
            ssne::perf::detail::Scope match_scope(ssne::perf::detail::InterpolationMatch);
            if(packed_positions && !layout.prepare(*frame))packed_positions=false;
            motion.prepare(previous.get(),*frame,packed_positions?&layout:nullptr);
        }
        {
            ssne::perf::detail::Scope copy_scope(ssne::perf::detail::InterpolationNativeCopy);
            if(packed_positions){layout.gather(*frame,native);packed_presented=native;}
            else native=frame->vtx;
            ssne::perf::detail::count(ssne::perf::detail::InterpolationNativeBytes,native.size());
        }
        prepared=true;
    }
};
}

