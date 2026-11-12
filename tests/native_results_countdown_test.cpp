#include "ssne/native_results_countdown.h"
#include <cstdlib>
#include <iostream>
void check(bool ok){if(!ok)std::abort();}
int main(){
    ssne::NativeResultsCountdown clock;
    check(!clock.due(1,true,true,100));
    for(unsigned frame=1;frame<3600;++frame)check(!clock.due(1,true,true,100+frame*15000ull/3600));
    check(!clock.due(1,true,true,15099));check(clock.due(1,true,true,15100));
    clock.issued(1);check(!clock.due(1,true,true,16000));check(!clock.due(1,true,true,999999));
    check(!clock.due(1,false,true,20000));check(!clock.due(1,true,true,25000));
    check(!clock.due(1,true,true,39999));check(clock.due(1,true,true,40000));
    check(!clock.due(2,true,true,40001));check(!clock.due(2,true,true,55000));check(clock.due(2,true,true,55001));
    check(!clock.due(2,true,false,60000));check(!clock.due(2,true,false,90000));
    check(!clock.due(2,true,true,100000));check(!clock.due(2,true,true,90000));
    check(!clock.due(2,true,true,104999));check(clock.due(2,true,true,105000));
    clock.reset();check(!clock.due(2,true,true,200000));check(!clock.due(0,true,true,999999));
    std::cout<<"Authoritative results wait fifteen real seconds; FPS, close/reopen, sequence, replica, rewind and reset cases passed\n";
}
