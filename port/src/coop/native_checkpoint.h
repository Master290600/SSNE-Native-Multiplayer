#pragma once
#include "native_world.h"
#include "ssne/checkpoint.h"
#include "ssne/native_party.h"
#include "ssne/native_effect.h"
#include "ssne/native_music.h"
#include "ssne/native_results.h"
#include <algorithm>
#include <limits>

namespace port::coop {
// Runtime and host gameplay participants. A fresh network authority epoch and
// client/event reconciliation are still required before exposing live saves.
class NativeCheckpointParticipant {
    NativeWorld& world_;
    ssne::RuntimeCheckpointHooks session_;
    std::function<bool(uint64_t,uint64_t,std::string&)> prepare_resume_;
    uint64_t current_epoch_=0;
    std::unique_ptr<NativeWorld::PreparedLifetimes> candidate_;
    static uint32_t u32(const std::vector<uint8_t>& b,size_t at) {
        uint32_t v=0;for(unsigned i=0;i<4;++i)v=(v<<8)|b.at(at+i);return v;
    }
    static uint64_t u64(const std::vector<uint8_t>& b,size_t at) {return (uint64_t(u32(b,at))<<32)|u32(b,at+4);}
    static void put(std::vector<uint8_t>& b,uint32_t v) {for(int s=24;s>=0;s-=8)b.push_back(uint8_t(v>>s));}
    static uint64_t hash(const std::vector<uint8_t>& b,size_t size) {
        uint64_t h=14695981039346656037ull;for(size_t i=0;i<size;++i){h^=b[i];h*=1099511628211ull;}return h;
    }
    bool capture(std::vector<uint8_t>& out,std::string& error) {
        candidate_.reset();
        std::vector<uint8_t> party,world,effects,music,story,session;
        if(!ssne::native_party_save_state(party,error) || !world_.save_lifetimes(world,error) || !ssne::native_effect_save_state(effects,error) || !ssne::native_music_save_state(music,error) || !ssne::native_story_save_state(story,error))return false;
        current_epoch_=u64(world,16);
        if(session_.capture && !session_.capture(session,error))return false;
        std::vector<uint8_t> bytes;put(bytes,session_.capture?0x4E434335:0x4E434334);put(bytes,uint32_t(party.size()));put(bytes,uint32_t(world.size()));put(bytes,uint32_t(effects.size()));put(bytes,uint32_t(music.size()));put(bytes,uint32_t(story.size()));
        if(session_.capture)put(bytes,uint32_t(session.size()));
        bytes.insert(bytes.end(),party.begin(),party.end());bytes.insert(bytes.end(),world.begin(),world.end());bytes.insert(bytes.end(),effects.begin(),effects.end());
        bytes.insert(bytes.end(),music.begin(),music.end());
        bytes.insert(bytes.end(),story.begin(),story.end());
        bytes.insert(bytes.end(),session.begin(),session.end());
        const auto h=hash(bytes,bytes.size());put(bytes,uint32_t(h>>32));put(bytes,uint32_t(h));
        out=std::move(bytes);return true;
    }
    bool restore(const std::vector<uint8_t>& bytes,std::string& error) {
        // Release a rejected candidate before accessing the old world again.
        candidate_.reset();
        auto fail=[&](const char* message){error=message;return false;};
        const unsigned header=session_.capture?28:24;
        if(bytes.size()<header+8 || bytes.size()>42*1024*1024 || u32(bytes,0)!=(session_.capture?0x4E434335:0x4E434334) || u64(bytes,bytes.size()-8)!=hash(bytes,bytes.size()-8))return fail("Invalid native checkpoint bundle");
        const auto party_size=u32(bytes,4),world_size=u32(bytes,8),effect_size=u32(bytes,12),music_size=u32(bytes,16),story_size=u32(bytes,20);
        const auto session_size=session_.capture?u32(bytes,24):0;
        if(party_size<584 || world_size<68 || effect_size<168 || music_size!=160 || story_size!=136 || (session_.capture && session_size<88) || uint64_t(party_size)+world_size+effect_size+music_size+story_size+session_size+header+8!=bytes.size())return fail("Invalid native checkpoint section sizes");
        std::vector<uint8_t> party(bytes.begin()+header,bytes.begin()+header+party_size);
        std::vector<uint8_t> world(bytes.begin()+header+party_size,bytes.begin()+header+party_size+world_size);
        std::vector<uint8_t> effects(bytes.begin()+header+party_size+world_size,bytes.begin()+header+party_size+world_size+effect_size);
        std::vector<uint8_t> music(bytes.end()-8-session_size-story_size-music_size,bytes.end()-8-session_size-story_size);
        std::vector<uint8_t> story(bytes.end()-8-session_size-story_size,bytes.end()-8-session_size);
        std::vector<uint8_t> session(bytes.end()-8-session_size,bytes.end()-8);
        const auto epoch=std::max(current_epoch_,u64(world,16));
        if(epoch>=std::numeric_limits<uint64_t>::max()-1)return fail("Native checkpoint epoch exhausted");
        if(!ssne::native_party_load_state(party,error) || !ssne::native_effect_load_state(effects,error) || !ssne::native_music_load_state(music,error) || !ssne::native_story_load_state(story,error))return false;
        if(session_.restore && !session_.restore(session,error))return false;
        candidate_=world_.prepare_lifetimes(world,epoch+1,error);
        return candidate_ && (!prepare_resume_ || prepare_resume_(epoch+1,candidate_->projectile_visual_tail(),error));
    }
public:
    explicit NativeCheckpointParticipant(NativeWorld& world,ssne::RuntimeCheckpointHooks session={},std::function<bool(uint64_t,uint64_t,std::string&)> resume={}):world_(world),session_(std::move(session)),prepare_resume_(std::move(resume)){}
    ssne::RuntimeCheckpointHooks hooks() {
        return {
            [this](auto& bytes,auto& error){return capture(bytes,error);},
            [this](const auto& bytes,auto& error){return restore(bytes,error);},
            [this]{candidate_->commit();candidate_.reset();if(session_.commit)session_.commit();},
            [this]{candidate_.reset();if(session_.abort)session_.abort();}
        };
    }
};
}
