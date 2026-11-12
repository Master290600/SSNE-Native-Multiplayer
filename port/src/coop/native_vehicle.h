#pragma once
#include "world_state.h"
struct PPCContext;
namespace port::coop {
// Level-owned PAL Jeep/Combine. Wire values contain level references and
// player slots only; renderer, physics and driver pointers stay local.
bool capture_native_vehicles(Properties&,std::string& error);
bool validate_native_vehicles(const Properties&,const std::map<EntityId,EntityState>&,std::string& error);
bool prepare_native_vehicle_models(PPCContext&,const Properties&,std::string& error);
bool apply_native_vehicle_drivers(PPCContext&,const Properties&,const std::map<EntityId,EntityState>&,std::string& error);
bool apply_native_vehicle_poses(const Properties&,std::string& error);
bool update_native_vehicle_huds(PPCContext&,const Properties&,std::string& error);
bool update_native_vehicle_audio(PPCContext&,const Properties&,std::string& error);
bool stop_native_vehicle_audio(PPCContext&,std::string& error);
struct NativeVehiclePresentation {
    std::vector<std::pair<uint32_t,float>> words;
    struct Pose {uint32_t object,model;std::vector<float> bones;};
    std::vector<Pose> poses;
};
bool prepare_native_vehicle_presentation(const Properties&,NativeVehiclePresentation&,std::string& error);
}
