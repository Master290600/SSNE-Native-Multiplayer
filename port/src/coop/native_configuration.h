#pragma once
#include "world_state.h"
#include "ssne/ppc_context.h"
#include <optional>
namespace port::coop {
struct NativeConfigurationSample {
    Properties values;
    unsigned unsupported=0;
    std::string error;
};
// Reads the native config tree through verified layouts/getter instructions.
// No guest function is called and no native variable is changed.
NativeConfigurationSample read_native_configuration(uint32_t root,const std::string& prefix={});
uint32_t native_global_configuration();
std::optional<std::string> read_native_text(uint32_t object,unsigned limit=64);
bool validate_native_configuration(PPCContext&,uint32_t root,const Properties& values,
                                   const std::string& allowed_prefix,std::string& error);
// Guest-thread bootstrap only. Validates all requested fields against native
// types/ranges before invoking native string storage and committing scalar values.
bool write_native_configuration(PPCContext&,uint32_t root,const Properties& values,
                                const std::string& allowed_prefix,std::string& error);
}
