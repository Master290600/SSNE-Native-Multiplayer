#include "ssne/input.h"
#include <array>
#include <cstdlib>
#include <iostream>
void require(bool ok,const char* message){if(!ok){std::cerr<<message<<std::endl;std::_Exit(1);}}
int main() {
    std::array<ssne::PadState,8> inputs;
    for(unsigned slot=0;slot<8;++slot){inputs[slot].buttons=uint16_t(1u<<slot);inputs[slot].stick_x=uint8_t(30+slot*20);inputs[slot].stick_y=uint8_t(220-slot*20);ssne::set_pad(slot,inputs[slot]);}
    for(unsigned slot=0;slot<8;++slot){auto got=ssne::get_pad(slot);require(got.buttons==inputs[slot].buttons && got.stick_x==inputs[slot].stick_x && got.stick_y==inputs[slot].stick_y,"Logical player inputs alias another slot");}
    auto physical=ssne::input_snapshot();
    for(unsigned slot=0;slot<4;++slot)require(physical.pads[slot].buttons==inputs[slot].buttons,"Higher slots overwrite physical input reports");
    ssne::set_pad(8,inputs[7]);ssne::set_pad(-1,inputs[7]);
    require(ssne::get_pad(0).buttons==inputs[0].buttons && ssne::get_pad(7).buttons==inputs[7].buttons,"Invalid index overwrites a player");
    require(ssne::get_pad(8).buttons==0 && ssne::get_pad(-1).buttons==0,"Invalid index reads another player");
    ssne::inject_buttons(ssne::PAD_X,1000,6);
    require((ssne::get_pad(6).buttons&ssne::PAD_X)!=0,"Weapon-key pulse missing from logical network inputs");
    require(ssne::get_pad(2).buttons==inputs[2].buttons,"Weapon-key pulse aliases another slot");
    ssne::inject_buttons(ssne::PAD_Y,1000,1);
    require((ssne::get_pad(1).buttons&ssne::PAD_Y)!=0 && (ssne::get_pad(6).buttons&ssne::PAD_X)!=0,"Simultaneous players overwrite weapon pulses");
    std::cout<<"Eight logical inputs remain independent; physical channels retain their own input; invalid slots do not wrap.\n"<<std::flush;std::_Exit(0);
}
