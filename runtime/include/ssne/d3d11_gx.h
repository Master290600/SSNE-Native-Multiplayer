#pragma once
#include <cstdint>
#include <string>
namespace ssne::d3d11 {
class Device;
// Render-thread-only adapter for the shared GX executor. No OpenGL context or
// driver is used. IDs are internal resource handles, never native pointers.
bool initialize_gx(Device& device,std::string& error);
void shutdown_gx();
void* gx_proc(const char* name);
void* texture_view(uint32_t texture);
void invalidate_gx_context(); // before external UI/presentation context changes
void wait_idle();
}
