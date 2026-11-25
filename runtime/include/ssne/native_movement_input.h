#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ssne {
// PAL's clamp/input processor BEFORE walking's unit-circle normalization.
// Bytes use the published pad convention:128 is neutral. Installing already
// normalized values in the original bank would apply the final clamp twice.
inline std::array<float,2> native_movement_actions(uint8_t raw_x,uint8_t raw_y){
    const int x=int(raw_x)-128,y=int(raw_y)-128;
    const int sign_x=x<0?-1:1,sign_y=y<0?-1:1;
    int a=std::max(0,std::abs(x)-15),b=std::max(0,std::abs(y)-15);
    constexpr int cardinal=72,diagonal=40;
    const int boundary=diagonal*std::max(a,b)+(cardinal-diagonal)*std::min(a,b);
    if(boundary>diagonal*cardinal){a=a*(diagonal*cardinal)/boundary;b=b*(diagonal*cardinal)/boundary;}
    const auto square=[](int value,int other){
        const double magnitude=std::abs(other);
        // Preserve the native signed branch and signed-byte quantization.
        const double denominator=other<=40 ? (1.-magnitude/40.)*32.+40. :
            40.-((magnitude-40.)*(-.03125))*40.;
        const float scale=float(72./denominator);
        return int8_t(std::clamp(float(double(value)*scale),-128.f,127.f));
    };
    return {float(square(sign_x*a,sign_y*b))/72.f,float(square(sign_y*b,sign_x*a))/72.f};
}

inline std::array<float,2> normalize_native_movement_actions(std::array<float,2> actions){
    auto& right=actions[0];auto& forward=actions[1];
    const float forward2=float(double(forward)*forward),length2=float(double(right)*right+forward2);
    if(length2>1.f){
        const float length=float(std::sqrt(double(length2))),scale=float(1./length);
        right=float(double(right)*scale);forward=float(double(forward)*scale);
    }
    return actions;
}
inline std::array<float,2> native_movement_axes(uint8_t raw_x,uint8_t raw_y){
    return normalize_native_movement_actions(native_movement_actions(raw_x,raw_y));
}
}
