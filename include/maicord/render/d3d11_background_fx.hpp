#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/render/background_fx.hpp"
#include "maicord/core/win32_types.hpp"

struct ID3D11BlendState;
struct ID3D11Buffer;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11PixelShader;
struct ID3D11Query;
struct ID3D11VertexShader;

namespace maicord {

inline constexpr u32 kD3D11BackgroundFxTimingFrames = 4;
inline constexpr u32 kD3D11BackgroundFxTimedDraws = 8;
inline constexpr u32 kD3D11BackgroundFxCanvases = 32;

// One frame's timestamp pairs around plugin effect draws; read with
// DONOTFLUSH a few frames later, never waited on.
struct D3D11BackgroundFxTiming {
    ID3D11Query* disjoint;
    ID3D11Query* stamps[kD3D11BackgroundFxTimedDraws * 2];
    u64 owner_id, effect_id, frame_ms;
    u32 draws;
    bool begun, pending;
};

// The effect pass: a dedicated vertex shader over each canvas rectangle,
// the built-in Stars/Comets pixel shader or the selected plugin's, one
// constant buffer (mc_fx_constants + canvas row) at PS b0 / VS b2.
// Everything is created lazily on the first effect frame.
struct D3D11BackgroundFxOwner {
    ID3D11VertexShader* vertex_shader;
    ID3D11PixelShader* builtin_shader;
    ID3D11Buffer* constant_buffer;
    ID3D11BlendState* blend_state;
    ID3D11PixelShader* plugin_shader;
    u64 plugin_owner_id, plugin_effect_id, plugin_code_hash;
    u64 plugin_revision; // host revision; the bytecode is re-hashed when it moves
    float plugin_parameters[kBackgroundFxParameters];
    float plugin_time_base, plugin_last_phase;
    D3D11BackgroundFxTiming timing[kD3D11BackgroundFxTimingFrames];
    D3D11BackgroundFxTiming* timing_frame;
    u32 timing_cursor;
    BackgroundFxGuard guard;
    // Set when the guard turned the plugin effect off; the host consumes it.
    u64 tripped_owner_id, tripped_effect_id;
    bool tripped;
    bool failed; // built-in resources could not be created; effects stay off
    // Per frame.
    BackgroundFxSource source;
    bool vertex_constants_bound; // VS b2 holds the effect constants
    BackgroundFxConstants frame;
    float frame_window_width, frame_window_height, frame_pixel_scale;
    float layer_opacity; // the paint layer's group opacity, set by the draw loop
    u32 drawn[kD3D11BackgroundFxCanvases];
    u32 drawn_count;
    u32 draws_last_frame;
};

inline bool d3d11_background_fx_plugin_active(const D3D11BackgroundFxOwner& owner) noexcept {
    return owner.plugin_shader != nullptr;
}

// Builds (or keeps) the plugin pixel shader for a validated ps_5_0 program.
// The shader object is reused while owner, effect and bytecode are unchanged;
// parameters may change every frame. Returns the CreatePixelShader failure.
HRESULT d3d11_background_fx_select_plugin(D3D11BackgroundFxOwner*, ID3D11Device*, u64 owner_id, u64 effect_id,
                                          u64 revision, ConstSlice<u8> bytecode, const float* parameters,
                                          float phase_seconds, bool* changed) noexcept;
// Back to the built-in effect (or none). True when a plugin effect was active.
bool d3d11_background_fx_clear_plugin(D3D11BackgroundFxOwner*) noexcept;

// Picks the frame's effect (plugin, theme built-in or none) and collects
// completed GPU timings. False only on a device failure.
bool d3d11_background_fx_begin_frame(D3D11BackgroundFxOwner*, ID3D11Device*, ID3D11DeviceContext*,
                                     const Theme&, float phase_seconds, u32 width, u32 height, u32 dpi,
                                     u64 now_ms) noexcept;
// Draws the effect over one canvas (DIP rectangle, paint quad index) under the
// bound scissor and layer transform. True when it drew: it bound its shaders,
// topology, input layout, pixel constants, view 0 and blend state, which the
// caller binds again (VS b2 is bound once per frame). Skips canvases already
// drawn this frame.
bool d3d11_background_fx_draw(D3D11BackgroundFxOwner*, ID3D11DeviceContext*, UiRect canvas,
                              u32 quad_index) noexcept;
void d3d11_background_fx_end_frame(D3D11BackgroundFxOwner*, ID3D11DeviceContext*) noexcept;
void d3d11_background_fx_release(D3D11BackgroundFxOwner*) noexcept;

} // namespace maicord
