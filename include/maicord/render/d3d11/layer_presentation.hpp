#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

struct D3D11Renderer;

enum class RendererLayerSurface : u8 {
    gallery,
    popup,
    settings_choice,
    settings_page,
    theme_editor,
    profile,
    forum_form,
    count
};

// A draw-time parameter: changes damage the surface without changing its
// borrowed scene, retained geometry, shaped text or uploaded instances.
// Invalid normalized values fall back to identity. Returns whether it changed.
bool renderer_set_layer_opacity(D3D11Renderer*, RendererLayerSurface, float) noexcept;
// Popup layer entrance scale in (0, 1] about a DIP origin; invalid values fall
// back to identity. Returns whether it changed.
bool renderer_set_popup_scale(D3D11Renderer*, float scale, float origin_x, float origin_y) noexcept;
// Settings dialog entrance in [0, 1]: scrim opacity, dialog opacity and scale.
bool renderer_set_settings_modal_progress(D3D11Renderer*, float) noexcept;
// Toast layer opacity in [0, 1] (invalid values: 1); damages the toast's
// current rectangle and shadow. Returns whether it changed.
bool renderer_set_toast_opacity(D3D11Renderer*, float) noexcept;

} // namespace maicord
