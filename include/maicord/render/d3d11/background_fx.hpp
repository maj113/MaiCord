#pragma once
#include "maicord/render/effects/background_fx_data.hpp"
#include "maicord/ui/core/geometry.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/gpu/gpu.hpp"

namespace maicord {
struct Theme;

inline constexpr u32 kD3D11BackgroundFxCanvases = 32;

// The built-in Stars/Comets pass: a dedicated vertex shader that expands instanced sprites (one per comet
// lane and star cell) for the built-in pixel shader; one constant buffer (BackgroundFxConstants) at PS b0 /
// VS b2. Package effects have their own pass (effect_renderer.hpp). Everything is created lazily on the
// first effect frame.
struct D3D11BackgroundFxOwner {
    GpuVertexShader* vertex_shader;
    GpuPixelShader* builtin_shader;
    GpuBuffer* constant_buffer;
    GpuBlend* blend_state;
    float frame_pixel_scale; // the frame's DPI scale
    bool failed;             // resources could not be created; effects stay off
    // Per frame.
    BackgroundFxSource source;
    bool vertex_constants_bound; // VS b2 holds the effect constants
    BackgroundFxConstants frame;
    float layer_opacity; // the paint layer's group opacity, set by the draw loop
    u32 drawn[kD3D11BackgroundFxCanvases];
    u32 drawn_count;
    u32 draws_last_frame;
};

// Picks the frame's built-in effect (or none): `suppressed` when a package effect replaces it. False only on a
// device failure.
bool d3d11_background_fx_begin_frame(D3D11BackgroundFxOwner*, GpuDevice*, GpuContext*, const Theme&,
                                     float phase_seconds, u32 width, u32 height, u32 dpi, bool suppressed) noexcept;
// Draws the effect over one canvas (DIP rectangle, paint quad index) under the bound scissor, layer transform,
// strip topology and input layout, which it leaves alone. True when it drew: it bound its shaders, pixel
// constant slot 0 and blend state, which the caller binds again (VS b2 is bound once per frame). Skips
// canvases already drawn this frame.
bool d3d11_background_fx_draw(D3D11BackgroundFxOwner*, GpuContext*, UiRect canvas,
                              u32 quad_index) noexcept;
void d3d11_background_fx_release(D3D11BackgroundFxOwner*) noexcept;

} // namespace maicord
