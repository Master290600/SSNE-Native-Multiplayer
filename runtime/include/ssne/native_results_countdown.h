#pragma once
#include <cstdint>
namespace ssne {
// A real-time countdown belongs to one displayed authoritative result, not
// to frame counts or a restored campaign's previous wall-clock deadline.
class NativeResultsCountdown {
    uint64_t sequence_=0,started_=0,issued_=0;
public:
    static constexpr uint64_t delay_ms=15000;
    void reset(){sequence_=started_=issued_=0;}
    void issued(uint64_t sequence){issued_=sequence;}
    bool was_issued(uint64_t sequence)const{return sequence && issued_==sequence;}
    bool due(uint64_t sequence,bool active,bool authority,uint64_t now){
        if(!active || !authority || !sequence){reset();return false;}
        if(sequence_!=sequence || now<started_){sequence_=sequence;started_=now;issued_=0;return false;}
        if(issued_==sequence)return false;
        return now-started_>=delay_ms;
    }
};
}
