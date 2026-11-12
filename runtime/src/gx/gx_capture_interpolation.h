// Diagnostic-only validation and restoration of captured texture ownership.
#pragma once
#include "gx_frame_interpolation.h"
#include <unordered_map>
namespace ssne::gx {
inline bool capture_texture_equal(const Texture& a,const Texture& b){
    if(a.w!=b.w || a.h!=b.h || a.id!=b.id || a.efb_copy!=b.efb_copy || a.copy_fmt!=b.copy_fmt ||
       a.tex_fmt!=b.tex_fmt || a.intensity!=b.intensity || a.rgba!=b.rgba || a.mips!=b.mips ||
       a.copy_regions.size()!=b.copy_regions.size())return false;
    for(size_t i=0;i<a.copy_regions.size();++i){const auto& x=a.copy_regions[i];const auto& y=b.copy_regions[i];
        if(x.id!=y.id || x.source_y!=y.source_y || x.dest_y!=y.dest_y || x.height!=y.height)return false;}
    return true;
}
inline bool validate_interpolation_capture(const GpuFrame& f,std::string& error){
    auto fail=[&](const char* text){error=text;return false;};
    if(f.capture_version<8)return fail("Interpolation replay requires capture version 8 or newer");
    if(f.capture_stream==std::array<uint64_t,2>{} || !f.seq || !std::isfinite(f.produced_at) || f.produced_at<=0)
        return fail("Missing capture identity/sequence/time");
    if(!f.local_camera || !f.camera_valid)return fail("Recording has no valid local gameplay camera");
    if(!std::isfinite(f.wide) || f.wide<=0)return fail("Invalid aspect");
    for(float x:f.camera)if(!std::isfinite(x))return fail("Nonfinite camera");
    if(f.capture_texture_ids.size()!=f.textures.size())return fail("Texture identity table size differs");
    std::unordered_set<uint64_t> ids;std::unordered_set<const Texture*> pointers;
    for(size_t i=0;i<f.textures.size();++i){
        if(!f.textures[i] || !f.capture_texture_ids[i] || !ids.insert(f.capture_texture_ids[i]).second)return fail("Duplicate/missing texture identity");
        pointers.insert(f.textures[i].get());
    }
    for(const auto& state:f.states){
        if(state.xf>f.xf_states.size())return fail("Invalid XF state");
        for(auto* t:state.cfg.tex)if(t && !pointers.contains(t))return fail("Unowned texture pointer");
    }
    for(const auto& c:f.cmds){
        if(c.type!=GpuCmd::DRAW)continue;
        if(c.state>=f.states.size() || c.ntex>8 || c.stride<24+c.ntex*12 ||
           uint64_t(c.vtx_off)+uint64_t(c.stride)*c.vcount>f.vtx.size() ||
           uint64_t(c.idx_first)+c.idx_count>f.idx.size())return fail("Invalid draw ranges/layout");
        if(f.states[c.state].xf && !f.positions_materialized){
            if(c.stride<52 || (c.stride-52)%12)return fail("Invalid raw XF stride");
            const unsigned offset=c.stride-4;
            for(unsigned v=0;v<c.vcount;++v){float index;std::memcpy(&index,f.vtx.data()+c.vtx_off+size_t(v)*c.stride+offset,4);
                if(!std::isfinite(index) || index<0 || double(index)>UINT32_MAX)return fail("Invalid raw matrix index");}
        }
    }
    return true;
}
inline bool restore_capture_texture_identity(const GpuFrame& previous,GpuFrame& current,std::string& error){
    if(!validate_interpolation_capture(previous,error) || !validate_interpolation_capture(current,error))return false;
    if(previous.capture_stream!=current.capture_stream){error="Captures belong to different processes/streams";return false;}
    std::unordered_map<uint64_t,std::shared_ptr<const Texture>> prior;
    for(size_t i=0;i<previous.textures.size();++i)prior.emplace(previous.capture_texture_ids[i],previous.textures[i]);
    std::unordered_map<const Texture*,std::shared_ptr<const Texture>> replacements;
    for(size_t i=0;i<current.textures.size();++i)if(auto p=prior.find(current.capture_texture_ids[i]);p!=prior.end()){
        if(!capture_texture_equal(*p->second,*current.textures[i])){error="Same immutable texture identity has different contents";return false;}
        replacements.emplace(current.textures[i].get(),p->second);
    }
    // No partial mutation on a failed validation.
    for(auto& state:current.states)for(auto*& t:state.cfg.tex)if(auto p=replacements.find(t);p!=replacements.end())t=p->second.get();
    for(auto& t:current.textures)if(auto p=replacements.find(t.get());p!=replacements.end())t=p->second;
    return true;
}
}
