#pragma once
#include <array>
#include <cmath>

namespace port::coop {

// PAL Arrow::Update (80077154) advances spin at 4 rad/s and hover at 6 rad/s.
// Its pose routine (800773B0) postmultiplies the authored frame by a Y rotation
// and adds 0.25*sin(hover) along its unscaled Y axis.
struct NativeArrowClock {
    double start, spin, hover;

    std::array<float,12> pose(double now, double host_spin, double host_hover, float host_scale,
                              std::array<float,12> matrix) const {
        if (!(host_scale > 0.0001f)) return matrix;
        const double age=now-start;
        const double delta=spin+age*4.-host_spin;
        const double sine=std::sin(delta),cosine=std::cos(delta);
        const double lift=.25*(std::sin(hover+age*6.)-std::sin(host_hover))/host_scale;
        for(unsigned row=0;row<3;++row){
            const unsigned i=row*4;
            const double x=matrix[i],z=matrix[i+2];
            matrix[i]=float(x*cosine-z*sine);
            matrix[i+2]=float(x*sine+z*cosine);
            matrix[i+3]+=float(matrix[i+1]*lift);
        }
        return matrix;
    }
};
}
