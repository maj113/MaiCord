#pragma once

#include "maicord/ui/geometry.hpp"

namespace maicord {

// Shared pane splitter: a thin vertical grip straddling one edge of a
// resizable pane. Owners keep the drag state and the width; these helpers fix
// the hit slop, the limits and the pointer-to-width mapping so every
// resizable pane (channels, members, the thread browser's left edge) behaves
// the same way.
enum class UiSplitterEdge : u8 {
    right, // grip on the pane's right edge; the pane grows to the right
    left,  // grip on the pane's left edge; the pane grows to the left
};

// Half of the grip's width on each side of the edge.
inline constexpr float kUiSplitterGripDips = 4.0f;
// Side-pane limits, as in the official client.
inline constexpr float kUiSplitterMinimumPaneDips = 180.0f;
inline constexpr float kUiSplitterMaximumPaneDips = 420.0f;

struct UiSplitterLimits {
    float minimum;
    float maximum;
};

// The preferred maximum shrinks with the space actually available (so the
// chat column keeps its minimum width) but never below the pane minimum.
constexpr UiSplitterLimits ui_splitter_limits(float available,
                                              float minimum_pane = kUiSplitterMinimumPaneDips,
                                              float preferred_maximum = kUiSplitterMaximumPaneDips) noexcept {
    const float bounded = available == available ? available : minimum_pane;
    return {minimum_pane, minimum(preferred_maximum, maximum(minimum_pane, bounded))};
}

constexpr float ui_splitter_clamp(float width, UiSplitterLimits limits) noexcept {
    if (width != width)
        return limits.minimum;
    return minimum(maximum(width, limits.minimum), limits.maximum);
}

constexpr bool ui_splitter_hit(UiRect pane, UiSplitterEdge edge, float x, float y,
                               float grip = kUiSplitterGripDips) noexcept {
    if (!(pane.right > pane.left) || !(y >= pane.top) || !(y < pane.bottom))
        return false;
    const float line = edge == UiSplitterEdge::right ? pane.right : pane.left;
    return x >= line - grip && x < line + grip;
}

// Width implied by the pointer while the pane's opposite edge stays fixed.
constexpr float ui_splitter_drag_width(float fixed_edge, UiSplitterEdge edge, float pointer_x,
                                       UiSplitterLimits limits) noexcept {
    return ui_splitter_clamp(edge == UiSplitterEdge::right ? pointer_x - fixed_edge : fixed_edge - pointer_x,
                             limits);
}

// Persisted widths are whole DIPs; zero means "theme default".
constexpr u16 ui_splitter_persisted_width(float width) noexcept {
    return width == width && width >= 1.0f && width < 65535.0f ? static_cast<u16>(width + 0.5f) : u16{0};
}
constexpr float ui_splitter_restored_width(u16 persisted, float fallback, UiSplitterLimits limits) noexcept {
    return persisted ? ui_splitter_clamp(static_cast<float>(persisted), limits) : fallback;
}

} // namespace maicord
