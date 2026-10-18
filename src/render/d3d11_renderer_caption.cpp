#include "d3d11_renderer_caption.hpp"

#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/ui/icon.hpp"

namespace maicord {

inline constexpr u32 kCaptionPaintCapacity = 32;
inline constexpr u32 kCaptionGlyphCapacity = 128;
inline constexpr u32 kCaptionRunCapacity = 8;
inline constexpr u32 kCaptionTitleCapacity = 256;

struct RendererCaption {
    PaintStream paint;
    ShapedText shape;
    TextGlyph shape_glyphs[kCaptionGlyphCapacity];
    TextRun shape_runs[kCaptionRunCapacity];
    u16 shape_clusters[kCaptionGlyphCapacity];
    wchar_t utf16[kCaptionGlyphCapacity];
    GlyphQuad glyphs[kCaptionGlyphCapacity];
    char title[kCaptionTitleCapacity];
    ID3D11Buffer* paint_buffer;
    ID3D11Buffer* glyph_buffer;
    // The app icon: one immutable texture, replaced only when its size changes.
    ID3D11ShaderResourceView* icon_view;
    ID3D11Buffer* icon_quad;
    u32 icon_extent;
    UiCaptionBarState state;
    u64 stamp;
    u64 text_key;
    u32 title_bytes;
    // Appearance changed since the last Present, and before it (the other
    // back buffer still shows the older caption).
    bool dirty;
    bool dirty_previous;
    bool shaped;
    bool drawn;
};

namespace {

float dips(u32 pixels, u32 dpi) noexcept {
    return static_cast<float>(pixels) * 96.0f / static_cast<float>(maximum<u32>(dpi, 1));
}

u64 appearance_stamp(const D3D11Renderer& renderer, const RendererCaption& caption,
                     const Theme& theme) noexcept {
    const UiCaptionBarState& s = caption.state;
    u64 hash = kHash64Seed;
    const u32 state[6]{static_cast<u32>(s.hovered),
                       static_cast<u32>(s.pressed),
                       s.active,
                       s.maximized,
                       static_cast<u32>(s.update),
                       s.settings};
    hash = hash64_bytes(hash, state, sizeof(state));
    // Whole percent steps of download progress.
    hash = hash64_mix(hash, static_cast<u64>(s.progress * 100.0f));
    hash = hash64_counted_bytes(hash, caption.title, caption.title_bytes);
    const u32 geometry[4]{renderer.width, renderer.caption_height, renderer.dpi, caption.icon_extent};
    hash = hash64_bytes(hash, geometry, sizeof(geometry));
    hash = hash64_bytes(hash, &theme.tokens.caption, sizeof(theme.tokens.caption));
    hash = hash64_bytes(hash, &theme.base_lowest, sizeof(theme.base_lowest));
    hash = hash64_mix(hash, static_cast<u64>(ui_icon_theme_weight(theme) * 1000.0f));
    return hash64_bytes(hash, &theme.scale, sizeof(theme.scale));
}

u64 caption_text_seed(const RendererCaption& caption) noexcept {
    return hash64_counted_bytes(kHash64Seed, caption.title, caption.title_bytes);
}

// The band is exactly the reserved rows, whatever the scale rounded to.
UiCaptionBarLayout caption_layout(const D3D11Renderer& renderer, const RendererCaption& caption,
                                  const Theme& theme) noexcept {
    return ui_caption_bar_layout(theme, dips(renderer.width, renderer.dpi), caption.state,
                                 dips(renderer.caption_height, renderer.dpi));
}

bool create_buffer(ID3D11Device* device, u32 bytes, ID3D11Buffer** buffer) noexcept {
    if (*buffer)
        return true;
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = bytes;
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device->CreateBuffer(&description, nullptr, buffer));
}

bool upload(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const void* data, u32 bytes) noexcept {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
        return false;
    copy_bytes(mapped.pData, data, bytes);
    context->Unmap(buffer, 0);
    return true;
}

// Settings and update icons resolve against the shared icon atlas.
bool upload_paint(D3D11Renderer* renderer, RendererCaption* caption, float weight) noexcept {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    auto* context = renderer->graphics.context;
    if (FAILED(context->Map(caption->paint_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) ||
        !mapped.pData)
        return false;
    auto* destination = static_cast<PaintQuad*>(mapped.pData);
    copy_bytes(destination, caption->paint.quads,
               caption->paint.count * static_cast<u32>(sizeof(PaintQuad)));
    if (!icon_atlas_resolve(&renderer->icon_atlas, context, caption->paint.quads, destination,
                            caption->paint.count, renderer->dpi, weight))
        log_write(LogLevel::warning, "render", "caption_icon_raster_failed");
    context->Unmap(caption->paint_buffer, 0);
    return true;
}

// The app icon at its own pixel size, on whole pixels.
bool upload_icon_quad(D3D11Renderer* renderer, RendererCaption* caption,
                      const UiCaptionBarLayout& layout) noexcept {
    if (!caption->icon_view)
        return true;
    if (!create_buffer(renderer->graphics.device, static_cast<u32>(sizeof(MediaQuad)), &caption->icon_quad))
        return false;
    const float scale = static_cast<float>(renderer->dpi) / 96.0f;
    const float extent = static_cast<float>(caption->icon_extent) / scale;
    const auto pixel = [&](float dip) noexcept {
        return static_cast<float>(static_cast<i32>(dip * scale + .5f)) / scale;
    };
    const float left = pixel((layout.icon.left + layout.icon.right - extent) * .5f);
    const float top = pixel((layout.icon.top + layout.icon.bottom - extent) * .5f);
    const UiRect box{left, top, left + extent, top + extent};
    const MediaQuad quad{box.left, box.top, box.right, box.bottom, 0, 0, 1, 1, 1, 0, 0, 0, box};
    return upload(renderer->graphics.context, caption->icon_quad, &quad, static_cast<u32>(sizeof(quad)));
}

// One title line, centred in the band but kept inside the title span.
bool append_title(D3D11Renderer* renderer, RendererCaption* caption, const UiCaptionBarLayout& layout,
                  Color color, u32* count) noexcept {
    *count = 0;
    if (!caption->title_bytes || !renderer->glyph_text.ready)
        return true;
    const float span = layout.title.right - layout.title.left;
    if (span <= 0)
        return true;
    if (!caption->shaped) {
        const TextStyleSpan style{0, caption->title_bytes, text_style_none, text_size_sm, {}};
        const TextShapeResult shaped =
            text_shape_utf8_styled(&renderer->glyph_text.system, {caption->title, caption->title_bytes},
                                   ConstSlice<TextStyleSpan>{&style, 1}, span, layout.height,
                                   Slice<wchar_t>{caption->utf16, kCaptionGlyphCapacity}, &caption->shape,
                                   TextWrapping::no_wrap, TextOverflow::ellipsis);
        if (!shaped.ok())
            return false;
        caption->shaped = true;
    }
    const float width = caption->shape.width;
    const float origin_x =
        minimum(maximum((layout.width - width) * .5f, layout.title.left), layout.title.right - width);
    const float origin_y = (layout.height - caption->shape.height) * .5f;
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        bool invalidated = false;
        *count = 0;
        const D3D11GlyphTextAppendRequest request{
            &caption->shape,
            caption->glyphs,
            count,
            &invalidated,
            {},
            {},
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            color,
            origin_x,
            origin_y,
            0,
            0,
            kCaptionGlyphCapacity,
            renderer->dpi,
        };
        if (d3d11_glyph_text_append(&renderer->glyph_text, renderer->graphics.context, request) &&
            !invalidated)
            return true;
        if (!invalidated)
            return false;
        // The atlas had to evict: start it over; retained content text
        // re-rasterizes on the next frame.
        d3d11_glyph_text_reset_atlas(&renderer->glyph_text);
        renderer->retained_presentation.valid = false;
        presentation_damage_invalidate_full(&renderer->presentation_damage);
    }
    return false;
}

} // namespace

bool renderer_set_caption(D3D11Renderer* renderer, const UiCaptionBarState& state,
                          Utf8View title) noexcept {
    if (!renderer)
        return false;
    auto* caption = renderer->caption;
    if (!caption) {
        caption = static_cast<RendererCaption*>(
            VirtualAlloc(nullptr, sizeof(RendererCaption), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!caption || !paint_stream_initialize(&caption->paint, kCaptionPaintCapacity) ||
            !shaped_text_initialize(&caption->shape,
                                    Slice<TextGlyph>{caption->shape_glyphs, kCaptionGlyphCapacity},
                                    Slice<TextRun>{caption->shape_runs, kCaptionRunCapacity},
                                    Slice<u16>{caption->shape_clusters, kCaptionGlyphCapacity})) {
            if (caption) {
                paint_stream_release(&caption->paint);
                VirtualFree(caption, 0, MEM_RELEASE);
            }
            return false;
        }
        caption->dirty = true;
        renderer->caption = caption;
    }
    caption->state = state;
    const u32 bytes = minimum<u32>(title.bytes, kCaptionTitleCapacity);
    if (bytes != caption->title_bytes || !bytes_equal(caption->title, title.data, bytes)) {
        copy_bytes(caption->title, title.data, bytes);
        caption->title_bytes = bytes;
        caption->shaped = false;
    }
    return true;
}

bool renderer_set_caption_icon(D3D11Renderer* renderer, const u32* pixels, u32 extent) noexcept {
    auto* caption = renderer ? renderer->caption : nullptr;
    if (!caption || !renderer->graphics.device || !pixels || !extent || extent > 256)
        return false;
    if (caption->icon_view && caption->icon_extent == extent)
        return true;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = description.Height = extent;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{pixels, extent * 4, 0};
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    const bool created =
        SUCCEEDED(renderer->graphics.device->CreateTexture2D(&description, &data, &texture)) &&
        SUCCEEDED(renderer->graphics.device->CreateShaderResourceView(texture, nullptr, &view));
    com_release(texture);
    if (!created)
        return false;
    com_release(caption->icon_view);
    caption->icon_view = view;
    caption->icon_extent = extent;
    caption->dirty = true;
    return true;
}

bool d3d11_renderer_caption_pending(D3D11Renderer* renderer, const Theme& theme) noexcept {
    auto* caption = renderer->caption;
    if (!renderer->caption_height || !caption)
        return false;
    const u64 stamp = appearance_stamp(*renderer, *caption, theme);
    if (stamp != caption->stamp) {
        caption->stamp = stamp;
        caption->dirty = true;
        const u64 text_key =
            hash64_mix(hash64_mix(hash64_mix(caption_text_seed(*caption), renderer->width), renderer->dpi),
                       hash64_bytes(kHash64Seed, &theme.scale, sizeof(theme.scale)));
        if (text_key != caption->text_key) {
            caption->text_key = text_key;
            caption->shaped = false;
        }
    }
    return caption->dirty || caption->dirty_previous;
}

bool d3d11_renderer_caption_draw(D3D11Renderer* renderer, const Theme& theme) noexcept {
    auto* caption = renderer->caption;
    if (!renderer->caption_height || !caption || !renderer->graphics.target)
        return true;
    if (!renderer->present_full && !d3d11_renderer_caption_pending(renderer, theme))
        return true;
    auto* context = renderer->graphics.context;
    auto& pipeline = renderer->pipeline;
    const UiCaptionBarLayout layout = caption_layout(*renderer, *caption, theme);
    const float scale = static_cast<float>(renderer->dpi) / 96.0f;
    u32 glyph_count = 0;
    if (!paint_stream_reset(&caption->paint, layout.width, layout.height) ||
        !ui_caption_bar_paint(&caption->paint, theme, layout, caption->state, scale) ||
        caption->paint.rejected ||
        !append_title(renderer, caption, layout, ui_caption_bar_title_color(theme, caption->state),
                      &glyph_count) ||
        !create_buffer(renderer->graphics.device,
                       static_cast<u32>(sizeof(PaintQuad)) * kCaptionPaintCapacity,
                       &caption->paint_buffer) ||
        !create_buffer(renderer->graphics.device,
                       static_cast<u32>(sizeof(GlyphQuad)) * kCaptionGlyphCapacity,
                       &caption->glyph_buffer) ||
        !upload_paint(renderer, caption, ui_icon_theme_weight(theme)) ||
        !upload_icon_quad(renderer, caption, layout) ||
        (glyph_count && !upload(context, caption->glyph_buffer, caption->glyphs,
                                glyph_count * static_cast<u32>(sizeof(GlyphQuad))))) {
        log_write(LogLevel::error, "render", "caption_build_failed");
        return false;
    }

    // Caption rows: their own viewport and frame transform, then the content's again.
    const D3D11_VIEWPORT viewport{
        0, 0, static_cast<float>(renderer->width), static_cast<float>(renderer->caption_height), 0, 1};
    const D3D11_RECT scissor{0, 0, static_cast<LONG>(renderer->width),
                             static_cast<LONG>(renderer->caption_height)};
    if (!d3d11_draw_pipeline_update_layer(pipeline, context, renderer->width, renderer->caption_height,
                                          renderer->dpi, 1, 1, 0, 0))
        return false;
    const float blend_factor[4]{};
    const UINT offset = 0;
    context->OMSetRenderTargets(1, &renderer->graphics.target, nullptr);
    context->RSSetState(pipeline.ui_rasterizer_state);
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(1, &scissor);
    context->OMSetBlendState(pipeline.ui_blend_state, blend_factor, 0xFFFFFFFFu);
    context->VSSetConstantBuffers(0, 1, &pipeline.ui_frame_buffer);
    ID3D11Buffer* pixel_constants[2]{pipeline.ui_frame_buffer, nullptr};
    context->PSSetConstantBuffers(0, 2, pixel_constants);
    context->PSSetSamplers(0, 1, &pipeline.glyph_sampler);

    const UINT paint_stride = sizeof(PaintQuad);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(pipeline.ui_input_layout);
    context->IASetVertexBuffers(0, 1, &caption->paint_buffer, &paint_stride, &offset);
    context->VSSetShader(pipeline.ui_vertex_shader, nullptr, 0);
    context->PSSetShader(pipeline.ui_pixel_shader, nullptr, 0);
    ID3D11ShaderResourceView* icon_view = renderer->icon_atlas.gpu.texture_view;
    context->PSSetShaderResources(0, 1, &icon_view);
    context->DrawInstanced(6, caption->paint.count, 0, 0);
    ++renderer->draw_calls_last_frame;
    if (caption->icon_view) {
        const UINT media_stride = sizeof(MediaQuad);
        context->IASetInputLayout(pipeline.media_input_layout);
        context->IASetVertexBuffers(0, 1, &caption->icon_quad, &media_stride, &offset);
        context->VSSetShader(pipeline.media_vertex_shader, nullptr, 0);
        context->PSSetShader(pipeline.media_pixel_shader, nullptr, 0);
        ID3D11ShaderResourceView* media_views[3]{caption->icon_view, nullptr, nullptr};
        context->PSSetShaderResources(0, 3, media_views);
        context->DrawInstanced(6, 1, 0, 0);
        ++renderer->draw_calls_last_frame;
    }
    if (glyph_count) {
        const UINT glyph_stride = sizeof(GlyphQuad);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->IASetInputLayout(pipeline.glyph_input_layout);
        context->IASetVertexBuffers(0, 1, &caption->glyph_buffer, &glyph_stride, &offset);
        context->VSSetShader(pipeline.glyph_vertex_shader, nullptr, 0);
        context->PSSetShader(pipeline.glyph_pixel_shader, nullptr, 0);
        ID3D11ShaderResourceView* glyph_views[]{renderer->glyph_text.gpu.texture_view,
                                                d3d11_glyph_text_color_view(renderer->glyph_text)};
        context->PSSetShaderResources(0, 2, glyph_views);
        context->DrawInstanced(4, glyph_count, 0, 0);
        ++renderer->draw_calls_last_frame;
    }
    ID3D11ShaderResourceView* null_views[3]{};
    context->PSSetShaderResources(0, 3, null_views);
    caption->drawn = true;
    return d3d11_draw_pipeline_update_transform(pipeline, context, renderer->width, renderer->height,
                                                renderer->dpi, renderer->target_origin_y);
}

u32 d3d11_renderer_caption_dirty_rect(const D3D11Renderer& renderer, RECT* rectangle) noexcept {
    if (!renderer.caption || !renderer.caption->drawn || !renderer.caption_height)
        return 0;
    *rectangle = {0, 0, static_cast<LONG>(renderer.width), static_cast<LONG>(renderer.caption_height)};
    return 1;
}

void d3d11_renderer_caption_presented(D3D11Renderer* renderer) noexcept {
    auto* caption = renderer->caption;
    if (!caption || !caption->drawn)
        return;
    caption->dirty_previous = caption->dirty;
    caption->dirty = false;
    caption->drawn = false;
}

void d3d11_renderer_caption_invalidate(D3D11Renderer* renderer) noexcept {
    if (renderer->caption)
        renderer->caption->dirty = true;
}

void d3d11_renderer_caption_release(D3D11Renderer* renderer) noexcept {
    auto* caption = renderer->caption;
    if (!caption)
        return;
    com_release(caption->paint_buffer);
    com_release(caption->glyph_buffer);
    com_release(caption->icon_view);
    com_release(caption->icon_quad);
    shaped_text_reset(&caption->shape);
    paint_stream_release(&caption->paint);
    VirtualFree(caption, 0, MEM_RELEASE);
    renderer->caption = nullptr;
}

} // namespace maicord
