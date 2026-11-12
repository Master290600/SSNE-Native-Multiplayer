#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ssne::d3d11 {
// Native D3D11 device/presentation layer. The GX/TEV adapter is in d3d11_gx.h.
// Vertices use conventional GL clip coordinates; depth is converted to [0,w].
struct Vertex { float clip[4]; uint32_t rgba; float uv[2]; };
class Device {
public:
    Device();
    ~Device();
    Device(const Device&)=delete;
    Device& operator=(const Device&)=delete;
    bool initialize(void* hwnd,unsigned width,unsigned height,bool warp,std::string& error);
    bool resize(unsigned width,unsigned height,std::string& error);
    bool texture(unsigned width,unsigned height,std::span<const uint8_t> rgba,std::string& error);
    bool begin(const float clear[4],std::string& error);
    bool draw(std::span<const Vertex> vertices,std::span<const uint32_t> indices,bool blend,bool depth,std::string& error);
    bool readback(std::vector<uint8_t>& rgba,std::string& error);
    bool present(bool vsync,std::string& error);
    const std::string& info()const;
    unsigned width()const;
    unsigned height()const;
    void* native_device()const;
    void* native_context()const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
