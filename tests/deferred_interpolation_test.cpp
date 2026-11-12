#include "gx_frame_interpolation.h"
#include <cassert>
#include <iostream>

using namespace ssne::gx;

std::unique_ptr<GpuFrame> fixture(unsigned sequence) {
    auto f=std::make_unique<GpuFrame>();
    f->seq=sequence;f->produced_at=sequence/60.;f->presentation_generation=1;
    f->local_camera=true;f->camera_valid=true;f->wide=1.333333f;
    f->camera={1,0,0,float(sequence)*.01f, 0,1,0,0, 0,0,1,0};
    GpuXf xf{};xf.model[0]=xf.model[5]=xf.model[10]=1;
    xf.model[3]=sequence*.02f;xf.model[11]=-10;f->xf_states.push_back(xf);
    GpuState state{};state.perspective=true;state.world_camera=true;
    state.xf=1;state.render_proxy=1;state.vp[0]=320;state.vp[1]=-224;
    state.projection[0]=state.projection[2]=1;state.projection[4]=-1;state.projection[5]=-.1f;
    f->states.push_back(state);
    state.camera_locked=true;state.render_proxy=2;f->states.push_back(state);
    state.camera_locked=false;state.local_ui=true;state.render_proxy=0;f->states.push_back(state);
    for(unsigned draw=0;draw<3;++draw){
        GpuCmd cmd{};cmd.type=GpuCmd::DRAW;cmd.state=draw;cmd.stride=52;cmd.vcount=3;
        cmd.vtx_off=draw*156;cmd.idx_first=draw*3;cmd.idx_count=3;
        f->cmds.push_back(cmd);f->idx.insert(f->idx.end(),{0,1,2});
        f->vtx.resize((draw+1)*156);
        for(unsigned vertex=0;vertex<3;++vertex){
            const float position[4]={float(vertex)*.1f,float(vertex%2),0,1};
            auto* bytes=f->vtx.data()+cmd.vtx_off+vertex*52;
            std::memcpy(bytes,position,16);std::memcpy(bytes+36,position,12);
            std::memset(bytes+16,255,8);
        }
        if(draw==0){GpuCmd copy{};copy.type=GpuCmd::COPY_TEX;copy.copy_id=700+sequence;f->cmds.push_back(copy);}
    }
    GpuCmd display{};display.type=GpuCmd::COPY_XFB;f->cmds.push_back(display);
    return f;
}

int main(){
    for(bool compact:{false,true}){
    DeferredFrameInterpolation deferred;deferred.packed_positions=compact;
    std::unique_ptr<GpuFrame> previous;
    unsigned checked=0;
    for(unsigned seq=1;seq<=18;++seq){
        auto expected=fixture(seq),input=fixture(seq);
        if(seq==10){expected->presentation_generation=input->presentation_generation=2;}
        if(seq==12){expected->seq=input->seq=100;} // nonconsecutive capture
        if(seq==14){expected->states[0].local_ui=input->states[0].local_ui=true;}
        auto untouched=input->vtx;
        auto retired=deferred.advance(std::move(input));
        if(retired)assert(retired->seq!=deferred.frame->seq);
        assert(!deferred.prepared && !deferred.frame->positions_materialized);
        assert(deferred.frame->vtx==untouched); // advance does not transform a native recording

        materialize_positions(*expected);
        FrameInterpolation eager;eager.prepare(previous.get(),*expected);
        assert(FrameInterpolation::camera_possible(*deferred.frame)==!eager.camera_vertices.empty());
        if(seq%4==0 || seq%4==1){
            deferred.prepare();
            assert(deferred.frame->positions_materialized && deferred.prepared);
            if(compact){VertexBytes packed;deferred.layout.gather(*expected,packed);assert(deferred.native==packed);}
            else assert(deferred.native==expected->vtx);
            if(deferred.previous)assert(deferred.previous->vtx==previous->vtx);
            assert(deferred.motion.positions.size()==eager.positions.size());
            assert(deferred.motion.camera_vertex_count()==eager.camera_vertex_count());
            for(float alpha:{0.f,.5f,1.f,1.25f,2.f}){
                for(bool aim:{false,true}){
                    auto want=expected->vtx;
                    auto look=expected->camera;look[0]=.99995f;look[2]=.01f;look[8]=-.01f;look[10]=.99995f;
                    eager.apply(want,alpha,aim?&look:nullptr);
                    deferred.motion.apply_if_changed(deferred.presented(),deferred.scratch,
                        deferred.native,alpha,aim?&look:nullptr,true);
                    auto actual=deferred.frame->vtx;
                    if(compact){assert(actual==expected->vtx);deferred.layout.scatter(*deferred.frame,deferred.presented(),actual);}
                    assert(want==actual);
                    assert(!deferred.motion.apply_if_changed(deferred.presented(),deferred.scratch,
                        deferred.native,alpha,aim?&look:nullptr));
                    // Weapon and UI draw bytes must remain the current native pose.
                    assert(!std::memcmp(want.data()+156,expected->vtx.data()+156,312));
                    assert(deferred.frame->cmds[1].copy_id==700+seq);
                    ++checked;
                }
            }
            const auto last=deferred.frame->vtx;
            deferred.prepare();assert(deferred.frame->vtx==last); // do not capture prediction as native
            deferred.previous.reset(); // the production renderer recycles it after preparation
        }
        previous=std::move(expected);
    }
    // No frame was prepared: all native bytes stay raw and only two recordings survive.
    DeferredFrameInterpolation unused;unused.packed_positions=compact;
    for(unsigned seq=1;seq<=100;++seq){
        unused.advance(fixture(seq));
        assert(!unused.prepared && unused.native.empty() && unused.scratch.empty());
        assert(!unused.frame->positions_materialized);
        if(unused.previous)assert(!unused.previous->positions_materialized && unused.previous->seq==seq-1);
    }
    std::cout<<(compact?"Packed":"Full")<<" deferred interpolation: "<<checked<<" eager/predicted payload comparisons; raw history, cuts, HUD and weapon guards passed\n";
    }
    // Layout fallback may change representation between frames. Restore full
    // history before selecting the next representation, never after it.
    DeferredFrameInterpolation switching;std::unique_ptr<GpuFrame> prior;
    for(unsigned seq=1;seq<=30;++seq){
        auto expected=fixture(seq);materialize_positions(*expected);
        switching.advance(fixture(seq));switching.packed_positions=(seq%2)==0;
        switching.prepare();FrameInterpolation reference;reference.prepare(prior.get(),*expected);
        auto want=expected->vtx;reference.apply(want,1.5f);
        switching.motion.apply(switching.presented(),1.5f);
        auto actual=switching.frame->vtx;
        if(switching.packed_positions)switching.layout.scatter(*switching.frame,switching.presented(),actual);
        assert(actual==want);
        if(switching.previous)assert(switching.previous->vtx==prior->vtx);
        prior=std::move(expected);
    }
    std::cout<<"PASS 30 full/packed history transitions\n";
    auto f=fixture(1);PackedPositionLayout layout;assert(layout.prepare(*f));
    f->cmds[2].vtx_off=0;assert(!layout.prepare(*f)); // aliased draw ranges
    f=fixture(1);f->cmds[0].stride=12;assert(!layout.prepare(*f));
    f=fixture(1);f->cmds[0].vcount=1000;assert(!layout.prepare(*f));
}
