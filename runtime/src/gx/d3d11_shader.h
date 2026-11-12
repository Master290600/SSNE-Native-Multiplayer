#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace ssne::d3d11 {
// Translate the shared GX shader pair. Both stages are linked before assigning
// varying locations, so optimized-out attributes cannot shift the interface.
struct ShaderPair { std::string vertex, fragment; };
ShaderPair translate_gx(const std::string& vertex, const std::string& fragment);
}
