#pragma once

#include "maicord/ui/geometry.hpp"

namespace maicord {

// One rectangle edge as a constant offset (about +-16000 DIPs) from the start
// (left or top) or the end (right or bottom) of the same axis of a base
// rectangle. Four edges pack into one 64-bit immediate, so a call site passes
// one constant instead of building and storing four floats.
struct UiRectEdge {
    u16 bits;
};

consteval UiRectEdge ui_start(int offset) noexcept {
    return {static_cast<u16>(offset & 0x7fff)};
}

consteval UiRectEdge ui_end(int offset) noexcept {
    return {static_cast<u16>(0x8000 | (offset & 0x7fff))};
}

// Every edge is base.edge + offset (the same float addition a hand-written
// brace initializer performs).
__declspec(noinline) inline UiRect ui_rect_from_edges(const UiRect& base, u64 edges) noexcept {
    const float* const corners = &base.left;
    float out[4];
    for (u32 index = 0; index < 4; ++index) {
        const u32 bits = static_cast<u32>(edges >> (16 * index)) & 0xffffu;
        const i32 offset = static_cast<i16>(bits << 1) >> 1;
        out[index] = corners[(index & 1u) + ((bits >> 15) << 1)] + static_cast<float>(offset);
    }
    return {out[0], out[1], out[2], out[3]};
}

__forceinline UiRect ui_rect_edges(const UiRect& base, UiRectEdge left, UiRectEdge top, UiRectEdge right,
                                   UiRectEdge bottom) noexcept {
    return ui_rect_from_edges(base, static_cast<u64>(left.bits) | (static_cast<u64>(top.bits) << 16) |
                                        (static_cast<u64>(right.bits) << 32) |
                                        (static_cast<u64>(bottom.bits) << 48));
}

} // namespace maicord
