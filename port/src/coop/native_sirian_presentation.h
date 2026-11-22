#pragma once
#include "client_presentation.h"
#include <optional>

namespace port::coop {

// Sampled gun roots can include scale or interpolation shear. Isolate
// the rigid axes so attached effects preserve their native shape and size.
inline std::optional<PresentationMatrix> rigid_weapon_basis(PresentationMatrix pose){
    using Axis=std::array<float,3>;
    const auto cross=[](const Axis& a,const Axis& b){
        return Axis{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
    };
    const auto normalize=[](Axis& axis){
        const auto length=std::hypot(axis[0],axis[1],axis[2]);
        if(!std::isfinite(length) || length<.0001f)return false;
        for(auto& component:axis)component/=length;
        return true;
    };

    Axis forward{pose[2],pose[6],pose[10]};
    const Axis sampled_up{pose[1],pose[5],pose[9]};
    if(!normalize(forward))return {};
    auto right=cross(sampled_up,forward);
    if(!normalize(right))return {};
    const auto up=cross(forward,right);
    for(unsigned row=0;row<3;++row){
        pose[row*4]=right[row];
        pose[row*4+1]=up[row];
        pose[row*4+2]=forward[row];
    }
    return pose;
}

// The original owner beam joins the weapon muzzle to the result of its
// camera ray. Preserve the native range, thickness and two material layers
// while drawing that ray in the view in which the player is currently aiming.
inline std::optional<PresentationMatrix> sirian_owner_beam(
        const PresentationMatrix& native,const PresentationMatrix& camera,
        const std::array<float,3>& muzzle){
    const float range=std::hypot(native[2],native[6],native[10]);
    auto aim=camera;
    for(unsigned axis=0;axis<3;++axis){
        const unsigned i=axis*4;
        aim[i+2]=camera[i+3]+camera[i+2]*range-muzzle[axis];
        aim[i+3]=muzzle[axis];
    }
    auto pose=rigid_weapon_basis(aim);
    if(!pose)return {};
    for(unsigned col=0;col<3;++col){
        const float scale=std::hypot(native[col],native[col+4],native[col+8]);
        for(unsigned row=0;row<3;++row)(*pose)[row*4+col]*=scale;
    }
    return pose;
}

inline std::optional<PresentationMatrix> rigid_attachment_motion(const PresentationMatrix& source,
                                                                 const PresentationMatrix& destination){
    if(source==destination)return matrix_identity();
    const auto from=rigid_weapon_basis(source),to=rigid_weapon_basis(destination);
    if(!from || !to)return {};
    return matrix_product(*to,matrix_rigid_inverse(*from));
}

}
