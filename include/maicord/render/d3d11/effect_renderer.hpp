#pragma once
#include "maicord/render/effects/background_effect.hpp"
#include "maicord/ui/core/geometry.hpp"
#include "maicord/gpu/gpu.hpp"

namespace maicord {
struct Theme;

inline constexpr u32 kD3D11EffectTimingFrames = 4;
inline constexpr u32 kD3D11EffectCanvases = 32;
inline constexpr u32 kD3D11EffectStrips = 4;
// The target texture is allocated in buckets of this many pixels per axis.
inline constexpr u32 kD3D11EffectTargetBucket = 64;
// effect_use inputs: noise, glyph grid, blue noise, TEXTURE0..3 (t0..t6); a graph's targets follow at t7..t10.
inline constexpr u32 kD3D11EffectInputs = 7;

// A graph target: one plane, or a persistent target's two (`current` holds the last state).
struct D3D11EffectGraphTarget {
    GpuRtv* views[2];
    GpuSrv* reads[2];
    u32 width, height, current;
};
union D3D11EffectStage {
    GpuVertexShader* vertex;
    GpuPixelShader* pixel;
};

// One effect frame's timestamps; read with DONOTFLUSH a few frames later, never waited on.
struct D3D11EffectTiming {
    GpuTimer timer;
    u64 identity, frame_ms;
    bool begun, pending;
};

// The package effect pass: one offscreen render of the package's pixel shader (or of its pass graph, all passes
// timed as one frame) at a share of the window (the resolution step), drawn only when a frame is due under the
// frame-rate step, then sampled by every background canvas. Device objects are created lazily and rebuilt after a
// device loss; the selection (identity, knobs, settings, clock) survives it, a graph's persistent state does not.
struct D3D11EffectOwner {
    // Device objects.
    GpuVertexShader *target_vertex, *composite_vertex;
    GpuPixelShader *composite_pixel, *shader;
    GpuBuffer *frame_buffer, *composite_buffer;
    GpuRaster* rasterizer;
    // The composite's blends (EffectBlend), then a graph pass's (EffectPassBlend alpha, add, premultiplied), then the
    // composite's adapted screen and add (effect_light_composite): while they cross-fade, source plus the background
    // times the shader's second output; settled, the background times the source.
    GpuBlend* blends[8];
    GpuSampler* samplers[3];
    GpuSrv* inputs[kD3D11EffectInputs];
    GpuTexture* target;
    GpuRtv* target_view;
    GpuSrv* target_srv;
    // The pass graph (interface 3): the package's, its shaders but shader 0 (`shader`; vertex or pixel by
    // graph.vertex_shaders) and its targets, sized for the output. graph.pass_count 0: the single pass.
    EffectGraph graph;
    D3D11EffectStage graph_shaders[kEffectGraphShaders];
    D3D11EffectGraphTarget graph_targets[kEffectGraphTargets];
    D3D11EffectTiming timing[kD3D11EffectTimingFrames];
    u32 timing_cursor;
    u32 target_width, target_height; // allocated
    u32 used_width, used_height;     // rendered (the viewport)
    // Selection.
    u64 identity, adapter_key;
    u32 uses;
    float knobs[kEffectKnobRows][4];
    EffectSettings settings;
    EffectBudgetState budget;
    EffectProbe probe;
    u32 step; // the resolution step now rendering (the controller moves it)
    // The constants of the last redraw (palette, theme flags and knobs decide a repaint), the clock, and the
    // window the target was sized for.
    EffectFrameConstants constants;
    float last_phase, time, frames;
    float pointer[2]; // window pixels; negative when none
    float ambient[4]; // mc_ambient as the host eased it (EffectAmbientValue order)
    float light;      // the light adaptation's amount as the host eased it (effect_ambient_sample), 0..1
    u32 dpi, window_width, window_height;
    bool selected, probed, content_valid, dirty, composite_bound;
    // Written before an effect frame is submitted and cleared once its timing resolves, in a mapping that
    // survives a crash (null: none).
    volatile u64* marker;
    // The verdict for the host, once.
    EffectVerdict verdict;
    u64 verdict_identity;
    // Per frame.
    bool active;          // the frame's canvases composite the target
    bool rendered;        // this frame redrew the target
    float layer_opacity;
    float pixel_scale;    // the frame's DPI scale
    u32 caption_pixels;   // window rows above the content
    u32 drawn[kD3D11EffectCanvases];
    u32 drawn_count, draws_last_frame, renders;
};

enum class EffectSelect : u8 { unchanged, changed, rejected, refused };
struct EffectSelectResult {
    GpuStatus native_error;
    EffectSelect state;
};

// The window size in pixels (content plus caption) is what the cost probe extrapolates to.
// `glyph_view` is a ready glyph grid the owner adopts (null without effect_use_glyphs).
EffectSelectResult d3d11_effect_select(D3D11EffectOwner*, GpuDevice*, GpuContext*,
                                       const BackgroundEffectSpec&, const EffectSettings&, u64 adapter_key,
                                       GpuSrv* glyph_view, u32 window_width,
                                       u32 window_height) noexcept;
// Back to no effect. True when one was selected.
bool d3d11_effect_clear(D3D11EffectOwner*) noexcept;
// The pointer in window pixels (negative: none); the effect sees it only when it declared the pointer and the
// user allowed it.
void d3d11_effect_set_pointer(D3D11EffectOwner*, float x, float y) noexcept;
// A word in a crash-surviving mapping the pass writes the running effect's identity to before it submits a frame
// and clears once the GPU timing resolves (null: none).
void d3d11_effect_set_marker(D3D11EffectOwner*, volatile u64* marker) noexcept;

// Collects finished GPU timings, then redraws the target when a frame is due: the clock moved, or something
// the picture depends on changed, and the window is not being resized. `phase` is the app's effect clock
// in seconds; while it runs (`clock_running`) a change of inputs waits for its next tick, so mc.delta is 0 only on a
// redraw of a stopped clock. Sets owner->active when canvases can sample the target; owner->rendered says the target
// was redrawn (the caller restores the viewport and the render target it bound). False only on a device failure.
bool d3d11_effect_begin_frame(D3D11EffectOwner*, GpuDevice*, GpuContext*, const Theme&, float phase,
                              bool clock_running, u32 content_width, u32 content_height, u32 caption_height, u32 dpi,
                              bool live_resize, u64 now_ms) noexcept;
// Composites the target over one canvas (DIP rectangle, paint quad index) under the bound scissor, layer
// transform, strip topology and bilinear sampler, which it leaves alone. True when it drew: it bound its
// shaders, pixel constant slot 0, pixel view 0 and the blend state, which the caller binds again. Skips
// canvases already drawn this frame. A positive tile_radius draws a tile instead: the whole effect, cover-fitted
// to the rectangle (a miniature of the window background) with corners rounded to that radius in DIPs.
bool d3d11_effect_draw(D3D11EffectOwner*, GpuContext*, UiRect canvas, u32 quad_index,
                       float tile_radius = 0) noexcept;
void d3d11_effect_end_frame(D3D11EffectOwner*, GpuContext*) noexcept;
// Releases the device objects but keeps the selection, so the next frame rebuilds them.
void d3d11_effect_release_device(D3D11EffectOwner*) noexcept;
void d3d11_effect_release(D3D11EffectOwner*) noexcept;

} // namespace maicord
