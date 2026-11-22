#pragma once

extern "C" {
#include "ikcp.h"
}
#include <algorithm>
#include <cstdint>
#include <optional>

namespace port::coop::invite {

// KCP's fast retransmit normally needs ACKs for several subsequent packets.
// A one- or two-packet congestion window can stall behind its oldest hole
// for an entire backed-off RTO while control messages accumulate.
class InviteLossRecovery {
    std::optional<uint32_t> probed_sequence_,last_probe_ms_;
    uint64_t probes_=0;

public:
    uint64_t probes() const {return probes_;}

    // Only the invitation worker owns KCP, including its output callback.
    // Normal updates, ACK processing and flow control keep their original path.
    void update(ikcpcb& k,uint32_t now,bool received_valid_packet){
        ikcp_update(&k,now);
        if(probed_sequence_ && *probed_sequence_!=k.snd_una)probed_sequence_.reset();
        if(!received_valid_packet || probed_sequence_ || k.nsnd_que<2 || !k.nsnd_buf ||
           k.nsnd_buf>2 || !k.cwnd || k.cwnd>4 || !k.rmt_wnd || k.snd_nxt-k.snd_una<k.cwnd)return;
        if(last_probe_ms_ && now-*last_probe_ms_<1000)return;
        auto& oldest=*iqueue_entry(k.snd_buf.next,IKCPSEG,node);
        if(oldest.sn!=k.snd_una || !oldest.xmit || oldest.xmit+1>=k.dead_link || oldest.rto<1200)return;
        const auto delay=std::max(600u,uint32_t(std::max(0,k.rx_srtt))*3/2+100u);
        // KCP timestamps are 32-bit milliseconds. Unsigned ages and signed
        // deadline differences follow its clock across rollover.
        if(now-oldest.ts<delay || int32_t(oldest.resendts-now)<int32_t(delay/2))return;

        // Take one extra opportunity using KCP's own segment encoder, sequence
        // number and congestion response. Even if this copy is lost, the
        // outstanding normal retry and its subsequent backoff remain due.
        const auto due=oldest.resendts,backoff=oldest.rto;
        oldest.resendts=now;
        ikcp_flush(&k);
        oldest.resendts=due;
        oldest.rto=backoff;
        probed_sequence_=oldest.sn;
        last_probe_ms_=now;
        ++probes_;
    }
};

} // namespace port::coop::invite
