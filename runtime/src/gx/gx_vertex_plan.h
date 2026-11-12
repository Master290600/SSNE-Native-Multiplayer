#pragma once
#include <cstddef>
#include <cstdint>

namespace ssne::gx {
// Value-only description of the common indexed-f32 format. A plan belongs to
// one CP/XF state. Register writes, nested lists and state restoration must
// invalidate retained plans. Partial primitive input never caches vertex data.
// Vertex bytes and RAM contents are always read afresh by the decoder.
struct PackedCommonPlan {
    uint32_t base[4], step[4];
    uint32_t input_stride, uv_offset, stride, inputs;
    uint32_t tex_matrix[2];
    float position_matrix;
    bool color16;
};
bool prepare_gpu_common_plan(int vat, uint32_t packed_stride, PackedCommonPlan& out);
bool decode_gpu_common_into(const PackedCommonPlan& plan, const uint8_t* stream,
                            uint32_t count, uint8_t* destination, size_t size);

struct PackedVertexPlan {
    PackedCommonPlan common;
    struct Attribute {uint32_t type,fmt,n,offset,array;float scale;};
    Attribute attributes[12];
    uint32_t base[12],step[12],stride,input_stride,inputs,position_matrix;
    int32_t tex_matrix_offset[8];
    uint32_t tex_matrix[8];
    bool common_format,position_matrix_stream;
    // Value-only dispatch metadata;decoded vertex data is never retained.
    bool quantized_components=false;
};
bool prepare_gpu_vertex_plan(int vat,uint32_t packed_stride,PackedVertexPlan& out);
bool decode_gpu_vertex_plan_into(const PackedVertexPlan&,const uint8_t* stream,
                                 uint32_t count,uint8_t* destination,size_t size);
// Called only by the serialized FIFO processor. Returned metadata stays valid
// until invalidation; no decoded vertices, model pointers or RAM bytes persist.
const PackedVertexPlan* cached_gpu_vertex_plan(int vat,uint32_t packed_stride);
void invalidate_gpu_vertex_plans();
}
