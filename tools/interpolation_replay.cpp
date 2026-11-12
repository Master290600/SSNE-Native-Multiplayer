// CPU component replay, not a game/FPS/network benchmark. Private captures stay local.
#include "gx_capture_interpolation.h"
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <sstream>
using namespace ssne::gx;
// This portable tool records wall time only; thread CPU time is unavailable.
int64_t ssne::perf::detail::thread_cpu_ns(){return -1;}
namespace {
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point began){return std::chrono::duration<double,std::milli>(Clock::now()-began).count();}
uint64_t checksum(const VertexBytes& bytes){uint64_t h=14695981039346656037ull;for(auto b:bytes){h^=b;h*=1099511628211ull;}return h;}
void require(bool condition,const std::string& message){if(!condition)throw std::runtime_error(message);}
void stats(const char* name,std::vector<double> samples){
    std::sort(samples.begin(),samples.end());std::cout<<'"'<<name<<"\":{\"samples\":"<<samples.size()
        <<",\"median_ms\":"<<samples[samples.size()/2]<<",\"p95_ms\":"<<samples[(samples.size()-1)*95/100]<<'}';
}
}
int main(int argc,char** argv){try{
    require(argc==2 || argc==3,"Usage: interpolation-replay CAPTURE_DIRECTORY [iterations 1..200]");
    const int iterations=argc==3?std::stoi(argv[2]):30;
    require(iterations>=1 && iterations<=200,"Iterations must be 1..200");
    std::vector<std::filesystem::path> files;
    for(const auto& item:std::filesystem::directory_iterator(argv[1]))if(item.is_regular_file() && item.path().extension()==".bin" && item.path().filename().string().starts_with("frame_"))files.push_back(item.path());
    std::sort(files.begin(),files.end());require(files.size()>=2 && files.size()<=8,"Need 2..8 consecutive frame captures");
    std::vector<std::unique_ptr<GpuFrame>> frames;
    std::string error;size_t canonicalized=0;
    for(const auto& path:files){
        require(std::filesystem::file_size(path)<=128ull*1024*1024,"Capture exceeds 128 MiB diagnostic limit");
        auto f=gpu_load_captured_frame(path.string(),error);require(bool(f),error);
        require(validate_interpolation_capture(*f,error),error);
        if(!frames.empty()){
            const auto& p=*frames.back();
            require(f->seq==p.seq+1 && f->presentation_generation==p.presentation_generation && f->wide==p.wide,
                    "Frames are not consecutive within one presentation generation/aspect");
            require(f->produced_at>p.produced_at && frame_motion_period(&p,*f)>0,"Recorded interval is outside live interpolation limits");
            require(restore_capture_texture_identity(p,*f,error),error);
            for(const auto& t:f->textures)for(const auto& old:p.textures)canonicalized+=t==old;
        }
        frames.push_back(std::move(f));
    }
    // Keep raw inputs for repeated materialization; reset/copy work is excluded
    // from the reported kernel timer. No texture/image upload or GL calls occur.
    std::vector<VertexBytes> raw;for(const auto& f:frames)raw.push_back(f->vtx);
    std::vector<double> materialize,matching,apply;size_t payload_checks=0,pairs=0,position_pairs=0,camera_vertices=0;
    std::ostringstream pair_report;pair_report<<std::setprecision(12);
    uint64_t output_hash=0;
    for(size_t i=0;i<frames.size();++i){auto& f=*frames[i];const bool originally_materialized=f.positions_materialized;
        for(int repeat=-2;repeat<iterations;++repeat){
            f.vtx=raw[i];f.positions_materialized=originally_materialized;auto start=Clock::now();materialize_positions(f);
            if(repeat>=0)materialize.push_back(elapsed(start));
        }
    }
    for(size_t i=1;i<frames.size();++i){const auto& previous=*frames[i-1];const auto& current=*frames[i];
        const auto before=checksum(current.vtx),prior_before=checksum(previous.vtx);
        PackedPositionLayout layout;require(layout.prepare(current),"Capture requires unsupported aliased packed positions");
        VertexBytes native;layout.gather(current,native);
        FrameInterpolation reference;reference.prepare(&previous,current);
        FrameInterpolation actual;actual.batch_reprojection=true;
        for(int repeat=-2;repeat<iterations;++repeat){auto start=Clock::now();actual.prepare(&previous,current,&layout);if(repeat>=0)matching.push_back(elapsed(start));}
        require(!std::memcmp(&actual.diagnostics,&reference.diagnostics,sizeof(actual.diagnostics)),"Matching diagnostic counts differ");
        pairs+=actual.diagnostics.pairs;position_pairs+=actual.positions.size();camera_vertices+=actual.camera_vertex_count();
        if(i>1)pair_report<<',';
        const auto& d=actual.diagnostics;
        pair_report<<"{\"previous_seq\":"<<previous.seq<<",\"current_seq\":"<<current.seq
            <<",\"recorded_interval_ms\":"<<(current.produced_at-previous.produced_at)*1000
            <<",\"generation\":"<<current.presentation_generation<<",\"camera_changed\":"<<(previous.camera!=current.camera?"true":"false")
            <<",\"matched_draws\":"<<d.pairs<<",\"shape_rejections\":"<<d.shape<<",\"state_rejections\":"<<d.state
            <<",\"texture_rejections\":"<<d.texture<<",\"colour_changes\":"<<d.color<<",\"uv0_rejections\":"<<d.uv0
            <<",\"uv_extra_rejections\":"<<d.uv_extra<<",\"position_pairs\":"<<actual.positions.size()<<'}';
        for(float alpha:{0.f,.5f,1.f,1.25f,2.f}){
            auto want=current.vtx;reference.apply(want,alpha);
            auto reduced=native;actual.apply(reduced,alpha);auto expanded=current.vtx;layout.scatter(current,reduced,expanded);
            require(want==expanded,"Complete interpolated vertex payload differs from scalar/full reference");
            output_hash^=checksum(expanded);++payload_checks;
        }
        for(int repeat=-2;repeat<iterations;++repeat){auto reduced=native;auto start=Clock::now();actual.apply(reduced,1.25f);
            if(repeat>=0)apply.push_back(elapsed(start));output_hash^=checksum(reduced);}
        require(before==checksum(current.vtx) && prior_before==checksum(previous.vtx),"Replay modified immutable materialized input");
    }
    require(pairs && camera_vertices,"No real interpolation matching/camera workload recorded");
    std::cout<<std::setprecision(9)<<"{\"scope\":\"CPU replay with warmed data/allocations; excludes copying, GL, game simulation and networking\",\"frames\":"<<frames.size()
        <<",\"iterations\":"<<iterations<<",\"restored_texture_identities\":"<<canonicalized<<",\"matched_draw_pairs\":"<<pairs
        <<",\"position_pairs\":"<<position_pairs<<",\"camera_vertices\":"<<camera_vertices<<",\"exact_payload_checks\":"<<payload_checks
        <<",\"output_checksum\":\""<<output_hash<<"\",\"pairs\":["<<pair_report.str()<<"],\"timings\":{";
    stats("materialize",materialize);std::cout<<',';stats("match",matching);std::cout<<',';stats("apply",apply);std::cout<<'}';
    ssne::perf::detail::period.store(1);
    for(size_t i=1;i<frames.size();++i){
        PackedPositionLayout layout;layout.prepare(*frames[i]);FrameInterpolation motion;motion.batch_reprojection=true;
        motion.prepare(frames[i-1].get(),*frames[i],&layout);
        ssne::perf::detail::Frame sample(ssne::perf::detail::Presentation);
        ssne::perf::detail::Scope scope(ssne::perf::detail::InterpolationMatch);
        motion.prepare(frames[i-1].get(),*frames[i],&layout);
    }
    ssne::perf::detail::append_json(std::cout,[](const auto& text){std::ostringstream out;out<<std::quoted(text);return out.str();});std::cout<<"}\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"Interpolation replay failed: "<<e.what()<<'\n';return 1;}}
