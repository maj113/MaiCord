#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

struct D3D11Renderer;

enum class RendererLayerSurface : u8 {
    gallery,
    popup,
    settings_choice,
    theme_editor,
    profile,
    forum_form,
    count
};

// A draw-time parameter: changes damage the surface without changing its
// borrowed scene, retained geometry, shaped text or uploaded instances.
// Invalid normalized values fall back to identity. Returns whether it changed.
bool renderer_set_layer_opacity(D3D11Renderer*, RendererLayerSurface, float) noexcept;
// A surface's entrance/exit pose: uniform scale in (0, 1] about a DIP origin, then a shift of at most
// kRendererMaximumLayerShift DIPs on each axis. Invalid values fall back to identity. Returns whether it changed.
inline constexpr float kRendererMaximumLayerShift = 64;
bool renderer_set_layer_transform(D3D11Renderer*, RendererLayerSurface, float scale, float origin_x, float origin_y,
                                  float shift_x, float shift_y) noexcept;
// Settings dialog entrance in [0, 1]: scrim opacity, dialog opacity and scale.
bool renderer_set_settings_modal_progress(D3D11Renderer*, float) noexcept;
// Toast layer opacity in [0, 1] (invalid values: 1); damages the toast's
// current rectangle and shadow. Returns whether it changed.
bool renderer_set_toast_opacity(D3D11Renderer*, float) noexcept;
// What remains of the tooltip's entrance in [0, 1] (0: settled, invalid values: 0): the tooltip draws at opacity
// 1 - remaining, scaled from kUiMotionTooltipScale about its centre. Damages the tooltip; returns whether it changed.
bool renderer_set_tooltip_entrance(D3D11Renderer*, float remaining) noexcept;

} // namespace maicord
