#pragma once
#include "session_authority.h"
#include "checkpoint_codec.h"
#include <cstdlib>
#include <iostream>
#include <functional>

namespace authority_checkpoint_test {
using namespace port::coop;
inline void require(bool result,const char* message){if(!result){std::cerr<<message<<"\n";std::exit(1);}}
inline unsigned checkpoint_checks=0;
inline void checkpoint_require(bool result,const char* message){++checkpoint_checks;require(result,message);}
inline void resign(std::vector<uint8_t>& b){const auto h=checkpoint::hash(b,b.size()-8);for(unsigned i=0;i<8;++i)b[b.size()-8+i]=uint8_t(h>>(56-i*8));}
inline void word(std::vector<uint8_t>& b,size_t at,uint32_t value){for(unsigned i=0;i<4;++i)b.at(at+i)=uint8_t(value>>(24-i*8));}
inline void wide(std::vector<uint8_t>& b,size_t at,uint64_t value){for(unsigned i=0;i<8;++i)b.at(at+i)=uint8_t(value>>(56-i*8));}
inline std::vector<uint8_t> authority_fixture(uint32_t format,uint64_t counter,uint64_t vote){
    checkpoint::Writer w;w.u32(format);w.u32(2);if(format>=0x53414333)w.u32(0);
    w.u64(counter);w.u64(vote);w.flag(false);w.flag(false);
    for(unsigned i=0;i<max_players;++i){w.u32(max_players);w.u64(0);for(unsigned n=0;n<3;++n)w.flag(false);w.u64(0);w.text("");w.u64(0);w.u64(0);w.flag(false);if(format>=0x53414333)w.flag(false);}
    return w.finish();
}
inline void checkpoint_regression(){
    using Header=SessionAuthority::CheckpointHeader;
    std::string error;
    for(uint32_t format=0x53414331;format<=0x53414334;++format){
        const bool presentation=format==0x53414332||format==0x53414334;
        const bool spectators=format>=0x53414333;
        const uint64_t counter=0x100000001ull,vote=presentation?counter:0;
        const auto bytes=authority_fixture(format,counter,vote),before=bytes;
        Header h;error="stale error";
        checkpoint_require(SessionAuthority::read_checkpoint_header(bytes,h,error),"Versioned authority prefix rejected");
        checkpoint_require(error.empty()&&bytes==before,"Read-only authority prefix changed bytes or retained error");
        checkpoint_require(h.format==format&&h.capacity==2&&h.counter==counter&&h.cutscene==vote,"Authority counter/vote decoded from wrong version offsets");
        checkpoint_require(h.has_spectators==spectators&&h.presentation_vote==presentation,"Authority feature flags decoded incorrectly");
        checkpoint_require(h.counter_offset==(spectators?12u:8u)&&h.participants_offset==(spectators?36u:32u),"Authority prefix positions incorrect");
        SessionAuthority restored(2);
        checkpoint_require(restored.restore_checkpoint(bytes,error),"Versioned authority restore rejected valid state");
        checkpoint_require(restored.skip_vote().cutscene==vote,"Authority restore lost current vote");
        if(vote)checkpoint_require(restored.end_cutscene(vote),"Active legacy/presentation vote did not end");
        checkpoint_require(restored.begin_cutscene()==counter+1,"Authority restore lost historical counter");
        // Exercise the same schema-derived mutation used by the native
        // checkpoint self-test. SAC3/4 must not overwrite the lock word.
        auto changed=bytes;wide(changed,h.counter_offset,counter+1);resign(changed);
        Header later;checkpoint_require(SessionAuthority::read_checkpoint_header(changed,later,error)&&later.counter==counter+1&&later.cutscene==vote&&later.spectator_locks==0,"Versioned counter mutation changed another header field");
        checkpoint::Reader r(changed,4096);r.at=later.participants_offset;
        for(unsigned i=0;i<max_players;++i){r.u32();r.u64();for(unsigned n=0;n<3;++n)r.flag();r.u64();r.text(64);r.u64();r.u64();r.flag();if(later.has_spectators)r.flag();}
        r.done();checkpoint_require(true,"Versioned participant traversal");
    }
    SessionAuthority actual(2);const auto a=*actual.join("Host"),b=*actual.join("Client");
    actual.attach_character(a,11);actual.finish_join(a);actual.attach_character(b,22);actual.finish_join(b);
    const auto ended=actual.begin_cutscene();actual.end_cutscene(ended);
    actual.set_spectator_lock(2);const auto inactive=actual.checkpoint_state();Header h;
    checkpoint_require(SessionAuthority::read_checkpoint_header(inactive,h,error)&&h.format==0x53414333&&h.counter==1&&h.cutscene==0&&h.spectator_locks==2,"Real SAC3 completed-cutscene checkpoint did not decode");
    uint64_t obsolete_vote=0;for(unsigned i=16;i<24;++i)obsolete_vote=(obsolete_vote<<8)|inactive[i];
    checkpoint_require(obsolete_vote==0x100000000ull&&h.cutscene!=obsolete_vote,"Actual F9 counter1/vote0 regression did not reproduce old offset failure");
    checkpoint_require(actual.restore_checkpoint(inactive,error),"Real SAC3 counter1/vote0 exact rollback rejected");
    checkpoint_require(actual.restore_gameplay_checkpoint(inactive,error),"Real SAC3 counter1/vote0 gameplay restore rejected");
    actual.set_spectator_lock(0);const auto presentation=actual.begin_cutscene(true);
    const auto active=actual.checkpoint_state();
    checkpoint_require(SessionAuthority::read_checkpoint_header(active,h,error)&&h.format==0x53414334&&h.counter==presentation&&h.cutscene==presentation,"Real SAC4 active presentation did not decode");
    checkpoint_require(actual.restore_checkpoint(active,error),"Real SAC4 active presentation rollback rejected");
    // Presentation formats have always required a nonzero active vote. Do
    // not weaken that invariant while fixing the versioned prefix offsets.
    for(auto format:{0x53414332u,0x53414334u}){
        const auto bad=authority_fixture(format,1,0);Header sentinel;sentinel.counter=99;
        checkpoint_require(!SessionAuthority::read_checkpoint_header(bad,sentinel,error)&&sentinel.counter==99&&!error.empty(),"Inactive presentation header was accepted or changed output");
        SessionAuthority next(2);const auto state=next.checkpoint_state();
        checkpoint_require(!next.restore_checkpoint(bad,error)&&next.checkpoint_state()==state,"Invalid presentation checkpoint mutated authority");
    }
    const auto baseline=authority_fixture(0x53414333,1,0);
    auto rejects=[&](const std::function<void(std::vector<uint8_t>&)>& mutate){
        auto bad=baseline;mutate(bad);resign(bad);const auto saved=bad;Header out;out.counter=99;
        checkpoint_require(!SessionAuthority::read_checkpoint_header(bad,out,error)&&out.counter==99&&bad==saved&&!error.empty(),"Malformed authority prefix accepted or mutated input/output");
        SessionAuthority next(2);const auto state=next.checkpoint_state();
        checkpoint_require(!next.restore_checkpoint(bad,error)&&next.checkpoint_state()==state,"Malformed authority checkpoint changed live state");
    };
    rejects([](auto& v){word(v,0,0x53414330);});
    rejects([](auto& v){word(v,0,0x53414335);});
    rejects([](auto& v){word(v,4,0);});
    rejects([](auto& v){word(v,4,9);});
    rejects([](auto& v){word(v,8,4);});
    rejects([](auto& v){wide(v,20,2);});
    rejects([](auto& v){word(v,28,2);});
    rejects([](auto& v){word(v,28,1);});
    rejects([](auto& v){word(v,32,2);});
    auto corrupt=baseline;corrupt.back()^=1;Header unchanged;unchanged.counter=99;
    checkpoint_require(!SessionAuthority::read_checkpoint_header(corrupt,unchanged,error)&&unchanged.counter==99,"Bad header checksum accepted");
    for(size_t n=0;n<36;++n){std::vector<uint8_t> short_header(baseline.begin(),baseline.begin()+n);checkpoint::Writer w;w.bytes=short_header;const auto truncated=w.finish();checkpoint_require(!SessionAuthority::read_checkpoint_header(truncated,unchanged,error),"Truncated authority prefix accepted");}
    // A valid committed decision uses the same central header parser during
    // both non-mutating inspection and exact full restoration.
    const auto first=*actual.participant(a),second=*actual.participant(b);
    actual.attach_character(a,first.character);actual.finish_join(a);actual.attach_character(b,second.character);actual.finish_join(b);
    actual.request_skip(a,1,presentation);actual.request_skip(b,1,presentation);
    const auto decided=actual.checkpoint_state();
    checkpoint_require(SessionAuthority::read_checkpoint_header(decided,h,error)&&h.skip_requested&&h.decision&&h.decision->vote.required==2&&h.participants_offset==68,"Committed presentation prefix was lost");
    checkpoint_require(actual.restore_checkpoint(decided,error),"Committed presentation checkpoint did not restore");
}
}
