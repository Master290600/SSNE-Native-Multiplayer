#pragma once
#include <array>
#include <cmath>
namespace ssne {
// AXART uses a full turn around the listener, with zero at the front. Use
// the local camera basis so the half-plane cannot change with distance or
// with rounding of an almost axial sound. Distance/Doppler remain native.
inline float native_camera_sound_pan(const std::array<float,3>& source,
                                    const std::array<float,12>& camera){
    double right=0,front=0;
    for(unsigned i=0;i<3;++i){const auto delta=double(source[i])-camera[i*4+3];right+=delta*camera[i*4];front-=delta*camera[i*4+2];}
    if(std::abs(right)+std::abs(front)<1e-9)return 0;
    auto angle=std::atan2(right,front);if(angle<0)angle+=6.2831853071795864769;
    return float(angle);
}
}
