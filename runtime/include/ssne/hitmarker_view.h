#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace ssne {
// Normalized edges of the original camera's viewport within the game image.
struct HitmarkerView { float left=0, top=0, right=1, bottom=1; };

namespace detail {
struct HitmarkerEdges { uint16_t left, top, right, bottom; };
inline std::array<std::atomic<HitmarkerEdges>,4> hitmarker_views{};
}

inline HitmarkerView hitmarker_view(unsigned slot) {
    const auto edges=detail::hitmarker_views[slot].load(std::memory_order_relaxed);
    constexpr float unit=1.f/65535.f;
    return {edges.left*unit, edges.top*unit, edges.right*unit, edges.bottom*unit};
}
}
