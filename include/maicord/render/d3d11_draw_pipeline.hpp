#pragma once

#include "maicord/core/base.hpp"
#include "maicord/ui/theme.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/sys/d3d11.hpp"

struct ID3D11BlendState;
struct ID3D11Buffer;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11InputLayout;
struct ID3D11PixelShader;
struct ID3D11RasterizerState;
struct ID3D11SamplerState;
struct ID3D11VertexShader;

namespace maicord {

inline constexpr u32 kD3D11UiFrameConstantBytes = 32;
inline constexpr u32 kD3D11InitialUiInstanceCapacity = 512;
inline constexpr u32 kD3D11MaximumUiInstanceCapacity = 4096;
constexpr u32 d3d11_ui_instance_capacity_for_count(u32 count) noexcept {
    if (!count || count > kD3D11MaximumUiInstanceCapacity)
        return 0;
    u32 capacity = kD3D11InitialUiInstanceCapacity;
    while (capacity < count)
        capacity *= 2;
    return capacity;
}
struct D3D11BackgroundConstants {
    float colors[5][4];
    float controls[4]; // Direction XY, stop count, intensity.
    float motion[4];   // Host phase seconds, speed, effect, strength.
};
static_assert(sizeof(D3D11BackgroundConstants) == 112);

enum class D3D11DrawPipelineError : u8 {
    none,
    invalid_input,
    ui_vertex_shader,
    ui_pixel_shader,
    ui_input_layout,
    ui_instance_buffer,
    ui_frame_buffer,
    rasterizer_state,
    blend_state,
    glyph_vertex_shader,
    glyph_pixel_shader,
    media_pixel_shader,
    glyph_input_layout,
    glyph_instance_buffer,
    media_instance_buffer,
    sampler_state,
};

struct D3D11DrawPipelineResult {
    HRESULT native_error;
    D3D11DrawPipelineError error;

    constexpr bool ok() const noexcept {
        return error == D3D11DrawPipelineError::none;
    }
};

struct D3D11DrawPipelineConfig {
    u32 ui_instance_stride;
    u32 ui_instance_capacity;
    u32 glyph_instance_stride;
    u32 glyph_instance_capacity;
    u32 media_instance_stride;
    u32 media_instance_capacity;
};

// Owns immutable draw state and the three dynamic instance streams. The
// renderer retains only logical commands, atlases and upload-generation state.
struct D3D11DrawPipelineOwner {
    ID3D11VertexShader* ui_vertex_shader;
    ID3D11PixelShader* ui_pixel_shader;
    ID3D11InputLayout* ui_input_layout;
    ID3D11Buffer* ui_instance_buffer;
    ID3D11Buffer* ui_frame_buffer;
    ID3D11RasterizerState* ui_rasterizer_state;
    ID3D11BlendState* ui_blend_state;
    ID3D11VertexShader* glyph_vertex_shader;
    ID3D11PixelShader* glyph_pixel_shader;
    ID3D11PixelShader* media_pixel_shader;
    ID3D11VertexShader* media_vertex_shader;
    ID3D11InputLayout* media_input_layout;
    ID3D11InputLayout* glyph_input_layout;
    ID3D11Buffer* glyph_instance_buffer;
    ID3D11Buffer* media_instance_buffer;
    ID3D11SamplerState* glyph_sampler;
    u32 glyph_instance_capacity;
    u32 glyph_instance_end;
    u32 ui_instance_capacity;
    ID3D11Buffer* background_buffer;
    D3D11BackgroundConstants background_constants;
    BackgroundTheme background_source;
    u32 background_updates;
    bool background_valid;
    bool background_active;
    bool background_light;
    bool background_source_valid;
    // Pixels of window caption above the content: the backdrop gradient spans
    // both, so it continues under the caption.
    u32 backdrop_caption;
};

static_assert(sizeof(D3D11DrawPipelineResult) == 8);
static_assert(sizeof(D3D11DrawPipelineConfig) == 24);
static_assert(sizeof(D3D11DrawPipelineOwner) == 304);

// Creates the shaders, states and buffers without touching a context, so any
// thread may build them (startup does so beside swap-chain creation).
D3D11DrawPipelineResult d3d11_draw_pipeline_create(D3D11DrawPipelineOwner* staged, ID3D11Device* device,
                                                   const D3D11DrawPipelineConfig& config) noexcept;
// Moves created objects into the owner and sets the frame transform; the
// immediate-context thread only. A failure releases the staged objects.
D3D11DrawPipelineResult d3d11_draw_pipeline_publish(D3D11DrawPipelineOwner* owner,
                                                    D3D11DrawPipelineOwner* staged,
                                                    ID3D11DeviceContext* context, u32 width, u32 height,
                                                    u32 dpi) noexcept;
D3D11DrawPipelineResult d3d11_draw_pipeline_initialize(D3D11DrawPipelineOwner* owner, ID3D11Device* device,
                                                       ID3D11DeviceContext* context, u32 width, u32 height,
                                                       u32 dpi,
                                                       const D3D11DrawPipelineConfig& config) noexcept;
D3D11DrawPipelineResult d3d11_draw_pipeline_reserve_glyph_instances(D3D11DrawPipelineOwner* owner,
                                                                    ID3D11Device* device, u32 stride,
                                                                    u32 capacity) noexcept;
// Grow only for real paint demand. Failure retains the existing buffer/capacity.
D3D11DrawPipelineResult d3d11_draw_pipeline_reserve_ui_instances(D3D11DrawPipelineOwner* owner,
                                                                 ID3D11Device* device, u32 stride,
                                                                 u32 required_count) noexcept;
// The viewport starts origin_y pixels down the target (below a window caption).
bool d3d11_draw_pipeline_update_transform(const D3D11DrawPipelineOwner& owner, ID3D11DeviceContext* context,
                                          u32 width, u32 height, u32 dpi, u32 origin_y = 0) noexcept;
// Update only the existing shared frame buffer, without uploading instances or
// changing the viewport. Invalid opacity becomes identity; the caller restores
// identity before drawing other groups. update_transform also restores it.
bool d3d11_draw_pipeline_update_opacity(const D3D11DrawPipelineOwner& owner, ID3D11DeviceContext* context,
                                        u32 width, u32 height, u32 dpi, float opacity) noexcept;
// Opacity plus a uniform scale about a DIP origin, folded into the existing
// frame transform. Scale 1 is identical to update_opacity.
bool d3d11_draw_pipeline_update_layer(const D3D11DrawPipelineOwner& owner, ID3D11DeviceContext* context,
                                      u32 width, u32 height, u32 dpi, float opacity, float scale, float origin_x,
                                      float origin_y) noexcept;
// The caption band's frame: `height` pixels at the target's top over a
// `content_height` content. Caption DIPs map to the band; `content_space`
// maps content DIPs -caption..0 there instead (the background effect pass).
bool d3d11_draw_pipeline_update_caption(const D3D11DrawPipelineOwner& owner, ID3D11DeviceContext* context,
                                        u32 width, u32 height, u32 content_height, u32 dpi,
                                        bool content_space) noexcept;
void d3d11_draw_pipeline_release(D3D11DrawPipelineOwner* owner) noexcept;
// Lazy GPU storage; inactive configurations do not allocate. Copied validated
// constants update only when configuration or sampled phase changes.
bool d3d11_draw_pipeline_update_background(D3D11DrawPipelineOwner*, ID3D11Device*, ID3D11DeviceContext*,
                                           const Theme&, float phase_seconds) noexcept;

} // namespace maicord
