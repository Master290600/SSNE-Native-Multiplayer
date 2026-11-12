#include "gx_frame_interpolation.h"
#include <bit>
#include <cassert>
#include <iostream>
#include <limits>

using namespace ssne::gx;
namespace {
uint32_t seed=0x793e512fu;
uint32_t next(){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
float random(float scale){return (float(next()%200001)/100000.f-1.f)*scale;}
std::array<float,12> camera(float yaw,float pitch,float x){
    const float c=std::cos(yaw),s=std::sin(yaw),cp=std::cos(pitch),sp=std::sin(pitch);
    return {c,s*sp,s*cp,x, 0,cp,-sp,2, -s,c*sp,c*cp,-3};
}
GpuFrame fixture(unsigned sequence,unsigned variant){
    GpuFrame f;f.seq=sequence;f.produced_at=sequence/60.;f.presentation_generation=1;
    f.local_camera=f.camera_valid=true;f.camera=camera(sequence*.002f,.05f,sequence*.01f);
    for(unsigned d=0;d<10;++d){
        GpuState state{};state.perspective=true;state.world_camera=true;
        state.render_proxy=d==9?0:d+1;state.camera_locked=d==7;state.local_ui=d==8;
        state.projection[0]=state.projection[2]=1;state.projection[4]=-.99f;state.projection[5]=-.1f;
        state.vp[0]=320;state.vp[1]=-224;f.states.push_back(state);
        GpuCmd cmd{};cmd.type=GpuCmd::DRAW;cmd.state=d;cmd.ntex=d%3;cmd.stride=24+cmd.ntex*12;
        cmd.vcount=1+d;cmd.vtx_off=uint32_t(f.vtx.size());cmd.idx_first=uint32_t(f.idx.size());cmd.idx_count=cmd.vcount;
        f.vtx.resize(f.vtx.size()+cmd.vcount*cmd.stride+13); // independent byte alignment per draw
        for(unsigned v=0;v<cmd.vcount;++v){
            float p[4]={v*.1f+sequence*.003f,d*.03f+sequence*.001f,-2,3};
            auto* dest=f.vtx.data()+cmd.vtx_off+v*cmd.stride;std::memcpy(dest,p,16);std::memset(dest+16,255,8);
            for(unsigned t=0;t<cmd.ntex;++t){float uv[3]={v*.2f,d*.3f,1};std::memcpy(dest+24+t*12,uv,12);}
            f.idx.push_back(v);
        }
        f.cmds.push_back(cmd);
        if(d==4){GpuCmd copy{};copy.type=GpuCmd::COPY_TEX;f.cmds.push_back(copy);}
    }
    // Exercise the existing rejection paths on only the new recording.
    if(sequence==2){
        switch(variant%12){
        case 1:f.presentation_generation=2;break;
        case 2:f.seq=4;break;
        case 3:f.camera_valid=false;break;
        case 4:f.camera=camera(1.8f,0,100);break;
        case 5:f.camera=camera(.002f,.05f,.01f);break; // stationary camera
        case 6:f.vtx[f.cmds[2].vtx_off+16]=0;break; // identified lighting change
        case 7:f.vtx[f.cmds[2].vtx_off+24]=1;break; // reject UV change
        case 8:f.idx[4]=99;break; // topology change
        case 9:f.states[3].projection[0]=2;break;
        case 10:{const float nan=std::numeric_limits<float>::quiet_NaN();std::memcpy(f.vtx.data()+f.cmds[3].vtx_off,&nan,4);break;}
        case 11:{const float near=0;std::memcpy(f.vtx.data()+f.cmds[3].vtx_off+12,&near,4);break;}
        }
    }
    return f;
}
}
int main(){
    size_t accepted=0,rejected=0,partial=0;
    const float edges[]={0.f,-0.f,.02f,std::nextafter(.02f,0.f),std::nextafter(.02f,1.f),
        -1.f,1.f,std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::max(),std::numeric_limits<float>::denorm_min()};
    for(unsigned setup=0;setup<64;++setup){
        const auto from=camera(random(2),random(1),random(20)),to=camera(random(2),random(1),random(20));
        const FrameInterpolation::Reprojection r(from,to);
        float p[6]={.8f+random(.4f),random(.2f),1.3f+random(.5f),random(.2f),-.99f,-.1f};
        if(setup==0)p[0]=0;if(setup==1)p[2]=std::numeric_limits<float>::quiet_NaN();
        if(setup==2)p[4]=std::numeric_limits<float>::max();
        const FrameInterpolation::ClipReprojection clip(r,p);
        for(unsigned batch=0;batch<4096;++batch){
            float scalar[4][4],packed[4][4];
            for(unsigned v=0;v<4;++v)for(unsigned k=0;k<4;++k){
                scalar[v][k]=k==3?std::abs(random(100)):random(100);
                if((batch+v*7+k)%11==0)scalar[v][k]=edges[next()%std::size(edges)];
                if((batch+v+k)%29==0)scalar[v][k]=std::bit_cast<float>(next());
            }
            std::memcpy(packed,scalar,sizeof(scalar));unsigned mask=0;
            for(unsigned v=0;v<4;++v){if(clip.apply(scalar[v])){mask|=1u<<v;++accepted;}else ++rejected;}
            assert(clip.apply_four(packed)==mask);
            assert(!std::memcmp(scalar,packed,sizeof(scalar)));
            partial+=mask!=0 && mask!=15;
        }
    }
    unsigned payloads=0;
    for(unsigned variant=0;variant<12;++variant){
        auto a=fixture(1,variant),b=fixture(2,variant);
        FrameInterpolation scalar,packed;packed.batch_reprojection=true;
        scalar.prepare(&a,b);packed.prepare(&a,b);
        PackedPositionLayout layout;assert(layout.prepare(b));
        FrameInterpolation compact;compact.batch_reprojection=true;compact.prepare(&a,b,&layout);
        VertexBytes native_compact;layout.gather(b,native_compact);
        assert(scalar.positions.size()==packed.positions.size());
        for(size_t i=0;i<scalar.positions.size();++i){const auto& s=scalar.positions[i];const auto& p=packed.positions[i];
            assert(s.offset==p.offset && !std::memcmp(s.from,p.from,16) && !std::memcmp(s.to,p.to,16));}
        assert(!std::memcmp(&scalar.diagnostics,&packed.diagnostics,sizeof(scalar.diagnostics)));
        for(float alpha:{0.f,.5f,1.f,1.25f,2.f})for(bool aim:{false,true}){
            auto want=b.vtx,actual=b.vtx;const auto look=camera(.05f,.1f,0);
            scalar.apply(want,alpha,aim?&look:nullptr);packed.apply(actual,alpha,aim?&look:nullptr);
            assert(want==actual);
            auto reduced=native_compact;compact.apply(reduced,alpha,aim?&look:nullptr);
            auto expanded=b.vtx;layout.scatter(b,reduced,expanded);assert(expanded==want);++payloads;
        }
    }
    assert(accepted && rejected && partial);
    std::cout<<"PASS "<<accepted+rejected<<" point comparisons ("<<accepted<<" accepted, "<<rejected
        <<" rejected, "<<partial<<" partial masks); "<<payloads<<" complete frame payload comparisons\n";
}
