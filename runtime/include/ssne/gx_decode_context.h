#pragma once
#include <cstdint>

namespace ssne {
// Host-side draw classification only. These values neither own guest RAM nor
// extend the lifetime of display lists, vertex arrays, poses, HUD or layout.
struct GxDecodeContext {
    bool discard=false,local=false,ui=false,gui_camera=false,frontend=false;
    bool operator==(const GxDecodeContext&)const=default;
};
// Private diagnostic metadata. It is excluded from the five classification
// values and from the GX02 state format; serial0 means no captured debug tag.
struct GxDecodeMarkerDebug {
    uint64_t serial=0;
    uint32_t reason=0,engine=0,sda=0,scene=0;
    int32_t view=-1,list_owner=-1,local_player=-1;
    uint32_t information_object=0,information_camera=0;
    bool information_open=false,cutscene=false,movie=false;
    bool operator==(const GxDecodeMarkerDebug&)const=default;
};
struct GxDecodeDebugState {
    GxDecodeMarkerDebug marker{};
    uint64_t opcode_start=0,marker_offset=0;
    uint32_t opcode=0,depth=0;
    uint64_t raw_head=0,raw_taken=0,raw_done=0;
    uint64_t pending_begin=0,pending_bytes=0,pending_need=0;
    uint64_t last_fence_head=0,last_fence_pending_begin=0,last_fence_pending_bytes=0,last_fence_need=0;
};
// Context support is enabled by default; SSNE_GX_SCOPED_CONTEXT=0 selects
// the legacy reference. Only authoritative PAL HLE roots issue marked scopes.
// Live-only states keep GX01; known pending contexts require GX02 support.
bool gx_scoped_context_enabled();
// Guest thread only. Reads the direct legacy HLE state, never the decoder TLS.
GxDecodeContext capture_gx_decode_context();
// PAL CameraRender callsites invoke this after their current-view SDA store.
// No-op outside the verified authoritative native render transaction.
void gx_mark_native_view_context(uint32_t return_address);
// Ordered at the current FIFO byte offset, including partial commands.
void gx_mark_decode_context(const GxDecodeContext&);
void gx_mark_decode_context_debug(const GxDecodeContext&,const GxDecodeMarkerDebug&);
// Decoder-only: caller already holds the FIFO parser lock.
GxDecodeDebugState gx_decode_debug_state();
// Return to the legacy live getters outside an explicitly marked render scope.
void gx_clear_decode_context();
// Non-null only while decoding a marked opcode (and its nested display lists).
// Guest execution and GPU frame presentation never inherit this override.
const GxDecodeContext* gx_decode_context_override();
}
