#pragma once

#include "maicord/ui/core/geometry.hpp"

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
// brace initializer performs). The four edges are one vector: each 16-bit
// field gives its offset (bits 0-14, sign-extended) and its corner (bit 15).
__declspec(noinline) inline UiRect ui_rect_from_edges(const UiRect& base, u64 edges) noexcept {
    typedef short Fields __attribute__((__vector_size__(8)));
    typedef int Ints __attribute__((__vector_size__(16)));
    typedef float Floats __attribute__((__vector_size__(16)));
    static_assert(sizeof(UiRect) == sizeof(Floats) && sizeof(edges) == sizeof(Fields));
    Fields fields;
    __builtin_memcpy(&fields, &edges, sizeof(fields));
    Floats corners;
    __builtin_memcpy(&corners, &base, sizeof(corners));
    const Ints from_end = __builtin_convertvector(fields >> 15, Ints);
    const Floats offsets = __builtin_convertvector(__builtin_convertvector((fields << 1) >> 1, Ints), Floats);
    const Floats out = (from_end ? __builtin_shufflevector(corners, corners, 2, 3, 2, 3)
                                 : __builtin_shufflevector(corners, corners, 0, 1, 0, 1)) +
                       offsets;
    UiRect result;
    __builtin_memcpy(&result, &out, sizeof(result));
    return result;
}

__forceinline UiRect ui_rect_edges(const UiRect& base, UiRectEdge left, UiRectEdge top, UiRectEdge right,
                                   UiRectEdge bottom) noexcept {
    return ui_rect_from_edges(base, static_cast<u64>(left.bits) | (static_cast<u64>(top.bits) << 16) |
                                        (static_cast<u64>(right.bits) << 32) |
                                        (static_cast<u64>(bottom.bits) << 48));
}

} // namespace maicord
