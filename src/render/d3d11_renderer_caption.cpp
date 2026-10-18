#include "d3d11_renderer_caption.hpp"

#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/tooltip.hpp"

namespace maicord {

inline constexpr u32 kCaptionPaintCapacity = 64;
inline constexpr u32 kCaptionGlyphCapacity = 128;
inline constexpr u32 kCaptionRunCapacity = 8;
inline constexpr u32 kCaptionTitleCapacity = 256;
// The effect pass's canvas index for the caption band (content quads use theirs).
inline constexpr u32 kCaptionEffectCanvas = ~0u;

// One shaped line: the title, or the tooltip's label.
struct CaptionLine {
    ShapedText shape;
    TextGlyph glyphs[kCaptionGlyphCapacity];
    TextRun runs[kCaptionRunCapacity];
    u16 clusters[kCaptionGlyphCapacity];
    wchar_t utf16[kCaptionGlyphCapacity];
    bool shaped;
};

struct RendererCaption {
    PaintStream paint;
    CaptionLine title_line;
    CaptionLine tooltip_line;
    // Title glyphs, then the tooltip's.
    GlyphQuad glyphs[kCaptionGlyphCapacity * 2];
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
    // The tooltip below the bar: its quads follow the bar's, and the target
    // pixels it covers (with shadow and arrow) when drawn, else empty.
    UiCaptionPart tooltip_part;
    u32 tooltip_first_quad;
    // The content's full-window scrims of the current draw, composited: the
    // band dims under them, the window buttons stay above.
    Color scrim;
    u32 buttons_first_quad;
    u32 scrim_quad;
    RECT tooltip_pixels;
    // The uploaded quads, reused while appearance, theme and the atlas cells
    // they sample are unchanged.
    Theme built_theme;
    u64 built_key;
    u32 builds, draws;
    u32 title_glyphs, tooltip_glyphs;
    bool built;
    // Appearance changed since the last Present, and before it (the other
    // back buffer still shows the older caption).
    bool dirty;
    bool dirty_previous;
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
    const u32 state[8]{static_cast<u32>(s.hovered),
                       static_cast<u32>(s.pressed),
                       s.active,
                       s.maximized,
                       static_cast<u32>(s.update),
                       s.settings,
                       static_cast<u32>(s.frame * 96.0f),
                       static_cast<u32>(s.tooltip)};
    hash = hash64_bytes(hash, state, sizeof(state));
    // Whole percent steps of download progress.
    hash = hash64_mix(hash, static_cast<u64>(s.progress * 100.0f));
    hash = hash64_counted_bytes(hash, caption.title, caption.title_bytes);
    // The backdrop gradient spans caption and content.
    const u32 geometry[5]{renderer.width, renderer.caption_height, renderer.height, renderer.dpi,
                          caption.icon_extent};
    hash = hash64_bytes(hash, geometry, sizeof(geometry));
    hash = hash64_bytes(hash, &theme.background, sizeof(theme.background));
    hash = hash64_bytes(hash, &theme.tokens.caption, sizeof(theme.tokens.caption));
    hash = hash64_bytes(hash, &theme.base_lowest, sizeof(theme.base_lowest));
    hash = hash64_bytes(hash, &theme.text_primary, sizeof(theme.text_primary));
    const u32 scrim[4]{static_cast<u32>(caption.scrim.red * 1000.0f), static_cast<u32>(caption.scrim.green * 1000.0f),
                       static_cast<u32>(caption.scrim.blue * 1000.0f), static_cast<u32>(caption.scrim.alpha * 1000.0f)};
    hash = hash64_bytes(hash, scrim, sizeof(scrim));
    hash = hash64_mix(hash, static_cast<u64>(ui_icon_theme_weight(theme) * 1000.0f));
    return hash64_bytes(hash, &theme.scale, sizeof(theme.scale));
}

u64 build_key(const D3D11Renderer& renderer, const RendererCaption& caption) noexcept {
    const u32 atlases[5]{
        renderer.glyph_text.allocator.evicted_entries, renderer.glyph_text.allocator.full_resets,
        renderer.icon_atlas.allocator.evicted_entries, renderer.icon_atlas.allocator.full_resets,
        static_cast<u32>(renderer.glyph_text.ready) | static_cast<u32>(renderer.icon_atlas.ready) << 1};
    return hash64_bytes(hash64_mix(kHash64Seed, caption.stamp), atlases, sizeof(atlases));
}

bool same_theme(const Theme& a, const Theme& b) noexcept {
    static_assert(sizeof(Theme) % sizeof(u64) == 0 && alignof(Theme) >= alignof(u64));
    const auto* x = reinterpret_cast<const u64*>(&a);
    const auto* y = reinterpret_cast<const u64*>(&b);
    u64 difference = 0;
    for (u32 i = 0; i < sizeof(Theme) / sizeof(u64); ++i)
        difference |= x[i] ^ y[i];
    return !difference;
}

u64 caption_text_seed(const RendererCaption& caption) noexcept {
    return hash64_counted_bytes(kHash64Seed, caption.title, caption.title_bytes);
}

// The band is exactly the reserved rows, whatever the scale rounded to.
UiCaptionBarLayout caption_layout(const D3D11Renderer& renderer, const RendererCaption& caption,
                                  const Theme& theme) noexcept {
    return ui_caption_bar_layout(theme, dips(renderer.width, renderer.dpi), caption.state,
                                 dips(renderer.caption_height, renderer.dpi),
                                 static_cast<float>(renderer.dpi) / 96.0f);
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

bool shape_line(D3D11Renderer* renderer, CaptionLine* line, Utf8View text, u32 flags, u8 size, float width,
                float height) noexcept {
    if (line->shaped)
        return true;
    const TextStyleSpan style{0, text.bytes, flags, size, {}};
    const TextShapeResult shaped = text_shape_utf8_styled(
        &renderer->glyph_text.system, text, ConstSlice<TextStyleSpan>{&style, 1}, width, height,
        Slice<wchar_t>{line->utf16, kCaptionGlyphCapacity}, &line->shape, TextWrapping::no_wrap,
        TextOverflow::ellipsis);
    line->shaped = shaped.ok();
    return line->shaped;
}

bool append_line(D3D11Renderer* renderer, const CaptionLine& line, GlyphQuad* output, Color color,
                 float origin_x, float origin_y, u32* count) noexcept {
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        bool invalidated = false;
        *count = 0;
        const D3D11GlyphTextAppendRequest request{
            &line.shape,
            output,
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

// One title line, centred in the band but kept inside the title span.
bool append_title(D3D11Renderer* renderer, RendererCaption* caption, const UiCaptionBarLayout& layout,
                  Color color, u32* count) noexcept {
    *count = 0;
    if (!caption->title_bytes || !renderer->glyph_text.ready)
        return true;
    const float span = layout.title.right - layout.title.left;
    if (span <= 0)
        return true;
    CaptionLine& line = caption->title_line;
    if (!shape_line(renderer, &line, {caption->title, caption->title_bytes}, text_style_none, text_size_sm, span,
                    layout.height))
        return false;
    const float width = line.shape.width;
    const float origin_x =
        minimum(maximum((layout.width - width) * .5f, layout.title.left), layout.title.right - width);
    return append_line(renderer, line, caption->glyphs, color, origin_x, (layout.height - line.shape.height) * .5f,
                       count);
}

// The content repaints the pixels a tooltip leaves; before the frame's
// content draw, which commits its damage.
void release_tooltip_pixels(D3D11Renderer* renderer, RendererCaption* caption) noexcept {
    const RECT& r = caption->tooltip_pixels;
    if (r.right <= r.left || r.bottom <= r.top)
        return;
    const float top = static_cast<float>(renderer->caption_height);
    const float scale = 96.0f / static_cast<float>(maximum<u32>(renderer->dpi, 1));
    const PresentationDamageRect rectangle{static_cast<float>(r.left) * scale, (r.top - top) * scale,
                                           static_cast<float>(r.right) * scale, (r.bottom - top) * scale};
    const PresentationDamageRect clip{0, 0, dips(renderer->width, renderer->dpi),
                                      dips(renderer->height, renderer->dpi)};
    if (!presentation_damage_add(&renderer->presentation_damage, rectangle, clip))
        presentation_damage_invalidate_full(&renderer->presentation_damage);
    caption->tooltip_pixels = {};
}

// The hovered part's name in a tooltip below the bar, inside the window.
bool append_tooltip(D3D11Renderer* renderer, RendererCaption* caption, const Theme& theme,
                    const UiCaptionBarLayout& layout, u32 title_count, u32* count) noexcept {
    *count = 0;
    caption->tooltip_first_quad = caption->paint.count;
    const UiCaptionPart part = caption->state.tooltip;
    const Utf8View label = ui_caption_bar_part_name(part, caption->state);
    const UiRect anchor = ui_caption_bar_part_rect(layout, part);
    if (!ui_caption_bar_part_tooltip(part) || !label.bytes || !ui_rect_has_area(anchor) ||
        !renderer->glyph_text.ready) {
        caption->tooltip_pixels = {};
        return true;
    }
    const UiTooltipStyle style{};
    CaptionLine& line = caption->tooltip_line;
    if (caption->tooltip_part != part)
        line.shaped = false;
    caption->tooltip_part = part;
    if (!shape_line(renderer, &line, label, text_style_subtext, 0, style.maximum_width - style.padding_x * 2,
                    style.maximum_height))
        return false;
    // Always below the band (caption DIPs continue into the content), kept
    // inside the window's sides.
    const float box_width = maximum(12.0f, line.shape.width) + style.padding_x * 2;
    const float box_height = line.shape.height + style.padding_y * 2;
    const float left = maximum(style.margin, minimum((anchor.left + anchor.right - box_width) * .5f,
                                                     layout.width - style.margin - box_width));
    const float top = anchor.bottom + style.gap;
    const UiRect box{left, top, left + box_width, top + box_height};
    if (!ui_tooltip_paint_box(&caption->paint, theme, box, anchor, UiTooltipPlacement::below, style) ||
        caption->paint.rejected)
        return false;
    const float scale = static_cast<float>(renderer->dpi) / 96.0f;
    // Shadow and arrow reach beyond the box.
    const float reach = style.arrow_size + 2;
    const RECT pixels{static_cast<LONG>((box.left - reach) * scale), static_cast<LONG>((box.top - reach) * scale),
                      static_cast<LONG>((box.right + reach) * scale + 1),
                      static_cast<LONG>((box.bottom + reach) * scale + 1)};
    caption->tooltip_pixels = pixels;
    return append_line(renderer, line, caption->glyphs + title_count, theme.text_primary,
                       (box.left + box.right - line.shape.width) * .5f,
                       (box.top + box.bottom - line.shape.height) * .5f, count);
}

// The scrim quad over the whole band, after the window buttons in the stream
// but drawn before them.
bool append_scrim(RendererCaption* caption, const UiCaptionBarLayout& layout) noexcept {
    if (!(caption->scrim.alpha > 0))
        return true;
    caption->scrim_quad = caption->paint.count;
    return paint_stream_push(&caption->paint, 0, 0, layout.width, layout.height, caption->scrim) &&
           caption->paint.count == caption->scrim_quad + 1;
}

// This frame's content draw covered part of the shown tooltip.
bool content_covers_tooltip(const D3D11Renderer& renderer, const RendererCaption& caption) noexcept {
    const RECT& t = caption.tooltip_pixels;
    if (t.right <= t.left || t.bottom <= t.top)
        return false;
    if (renderer.present_full)
        return true;
    const RECT& d = *renderer.present_dirty_rect.native();
    const LONG top = static_cast<LONG>(renderer.caption_height);
    return d.left < t.right && d.right > t.left && d.top + top < t.bottom && d.bottom + top > t.top;
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
            !shaped_text_initialize(&caption->title_line.shape,
                                    Slice<TextGlyph>{caption->title_line.glyphs, kCaptionGlyphCapacity},
                                    Slice<TextRun>{caption->title_line.runs, kCaptionRunCapacity},
                                    Slice<u16>{caption->title_line.clusters, kCaptionGlyphCapacity}) ||
            !shaped_text_initialize(&caption->tooltip_line.shape,
                                    Slice<TextGlyph>{caption->tooltip_line.glyphs, kCaptionGlyphCapacity},
                                    Slice<TextRun>{caption->tooltip_line.runs, kCaptionRunCapacity},
                                    Slice<u16>{caption->tooltip_line.clusters, kCaptionGlyphCapacity})) {
            if (caption) {
                paint_stream_release(&caption->paint);
                VirtualFree(caption, 0, MEM_RELEASE);
            }
            return false;
        }
        caption->dirty = true;
        renderer->caption = caption;
    }
    // A hidden, moved or relabelled tooltip; geometry and theme changes
    // repaint the whole target.
    const UiCaptionBarState& old = caption->state;
    if (old.tooltip != state.tooltip || old.update != state.update || old.maximized != state.maximized ||
        old.settings != state.settings)
        release_tooltip_pixels(renderer, caption);
    caption->state = state;
    const u32 bytes = minimum<u32>(title.bytes, kCaptionTitleCapacity);
    if (bytes != caption->title_bytes || !bytes_equal(caption->title, title.data, bytes)) {
        copy_bytes(caption->title, title.data, bytes);
        caption->title_bytes = bytes;
        caption->title_line.shaped = false;
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
    caption->built = false;
    return true;
}

bool d3d11_renderer_caption_pending(D3D11Renderer* renderer, const Theme& theme) noexcept {
    auto* caption = renderer->caption;
    if (!renderer->caption_height || !caption)
        return false;
    const u64 stamp = appearance_stamp(*renderer, *caption, theme);
    // Any other theme change (tooltip, backdrop tiers) restyles the bar too.
    if (stamp != caption->stamp || (caption->built && !same_theme(caption->built_theme, theme))) {
        caption->stamp = stamp;
        caption->dirty = true;
        const u64 text_key =
            hash64_mix(hash64_mix(hash64_mix(caption_text_seed(*caption), renderer->width), renderer->dpi),
                       hash64_bytes(kHash64Seed, &theme.scale, sizeof(theme.scale)));
        if (text_key != caption->text_key) {
            caption->text_key = text_key;
            caption->title_line.shaped = caption->tooltip_line.shaped = false;
        }
    }
    return caption->dirty || caption->dirty_previous;
}

bool d3d11_renderer_caption_draw(D3D11Renderer* renderer, const Theme& theme) noexcept {
    auto* caption = renderer->caption;
    if (!renderer->caption_height || !caption || !renderer->graphics.target)
        return true;
    // The back buffer still holds the bar unless it changed since that buffer
    // was presented (scrims of this frame's content draw included), the
    // content covered its tooltip or an animated backdrop moved under it.
    const bool animated = renderer->present_full && renderer->background_fx.source != BackgroundFxSource::none;
    if (!d3d11_renderer_caption_pending(renderer, theme) && !animated &&
        !content_covers_tooltip(*renderer, *caption))
        return true;
    auto* context = renderer->graphics.context;
    auto& pipeline = renderer->pipeline;
    const UiCaptionBarLayout layout = caption_layout(*renderer, *caption, theme);
    const float scale = static_cast<float>(renderer->dpi) / 96.0f;
    if (!caption->built || caption->built_key != build_key(*renderer, *caption) ||
        !same_theme(caption->built_theme, theme)) {
        caption->built = false;
        u32 glyph_count = 0, tooltip_glyphs = 0;
        caption->buttons_first_quad = 0;
        caption->scrim_quad = ~0u;
        // The stream culls to its viewport: the band and the content a tooltip
        // hangs into.
        if (!paint_stream_reset(&caption->paint, layout.width,
                                layout.height + dips(renderer->height, renderer->dpi)) ||
            !ui_caption_bar_paint(&caption->paint, theme, layout, caption->state, scale,
                                  &caption->buttons_first_quad) ||
            caption->paint.rejected || !append_scrim(caption, layout) ||
            !append_title(renderer, caption, layout, ui_caption_bar_title_color(theme, caption->state),
                          &glyph_count) ||
            !append_tooltip(renderer, caption, theme, layout, glyph_count, &tooltip_glyphs) ||
            !create_buffer(renderer->graphics.device,
                           static_cast<u32>(sizeof(PaintQuad)) * kCaptionPaintCapacity,
                           &caption->paint_buffer) ||
            !create_buffer(renderer->graphics.device,
                           static_cast<u32>(sizeof(GlyphQuad)) * kCaptionGlyphCapacity * 2,
                           &caption->glyph_buffer) ||
            !upload_paint(renderer, caption, ui_icon_theme_weight(theme)) ||
            !upload_icon_quad(renderer, caption, layout) ||
            (glyph_count + tooltip_glyphs &&
             !upload(context, caption->glyph_buffer, caption->glyphs,
                     (glyph_count + tooltip_glyphs) * static_cast<u32>(sizeof(GlyphQuad))))) {
            log_write(LogLevel::error, "render", "caption_build_failed");
            return false;
        }
        caption->title_glyphs = glyph_count;
        caption->tooltip_glyphs = tooltip_glyphs;
        // Keyed after the build: its own glyph and icon rasters may evict.
        caption->built_key = build_key(*renderer, *caption);
        copy_bytes(&caption->built_theme, &theme, sizeof(Theme));
        caption->built = true;
        ++caption->builds;
    }
    const u32 glyph_count = caption->title_glyphs, tooltip_glyphs = caption->tooltip_glyphs;

    // Caption rows: their own viewport and frame transform, then the content's again.
    const D3D11_VIEWPORT viewport{
        0, 0, static_cast<float>(renderer->width), static_cast<float>(renderer->caption_height), 0, 1};
    const D3D11_RECT scissor{0, 0, static_cast<LONG>(renderer->width),
                             static_cast<LONG>(renderer->caption_height)};
    const auto frame = [&](bool content_space) noexcept {
        return d3d11_draw_pipeline_update_caption(pipeline, context, renderer->width, renderer->caption_height,
                                                  renderer->height, renderer->dpi, content_space);
    };
    const float blend_factor[4]{};
    const UINT offset = 0;
    const UINT paint_stride = sizeof(PaintQuad);
    // The paint pipeline over the shared state (target, rasterizer, blend,
    // frame constants, sampler), which other pipelines here leave bound.
    const auto bind_paint_pipeline = [&]() noexcept {
        ID3D11Buffer* pixel_constants[2]{pipeline.ui_frame_buffer,
                                         pipeline.background_active ? pipeline.background_buffer : nullptr};
        context->PSSetConstantBuffers(0, 2, pixel_constants);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(pipeline.ui_input_layout);
        context->IASetVertexBuffers(0, 1, &caption->paint_buffer, &paint_stride, &offset);
        context->VSSetShader(pipeline.ui_vertex_shader, nullptr, 0);
        context->PSSetShader(pipeline.ui_pixel_shader, nullptr, 0);
        ID3D11ShaderResourceView* icon_view = renderer->icon_atlas.gpu.texture_view;
        context->PSSetShaderResources(0, 1, &icon_view);
    };
    const auto bind_paint = [&]() noexcept {
        context->OMSetRenderTargets(1, &renderer->graphics.target, nullptr);
        context->RSSetState(pipeline.ui_rasterizer_state);
        context->RSSetViewports(1, &viewport);
        context->RSSetScissorRects(1, &scissor);
        context->OMSetBlendState(pipeline.ui_blend_state, blend_factor, 0xFFFFFFFFu);
        context->VSSetConstantBuffers(0, 1, &pipeline.ui_frame_buffer);
        context->PSSetSamplers(0, 1, &pipeline.glyph_sampler);
        bind_paint_pipeline();
    };
    if (!frame(false))
        return false;
    bind_paint();
    // The backdrop canvas first; the content's background effect continues
    // over it in content DIPs, then the bar's own items.
    u32 first = 0;
    if (caption->paint.count && ui_background_canvas_quad(caption->paint.quads[0])) {
        context->DrawInstanced(6, 1, 0, 0);
        ++renderer->draw_calls_last_frame;
        first = 1;
        const float top = -layout.height;
        if (renderer->background_fx.source != BackgroundFxSource::none && frame(true)) {
            // The effect binds its shaders, input, pixel constants, view and blend.
            if (d3d11_background_fx_draw(&renderer->background_fx, context, {0, top, layout.width, 0},
                                         kCaptionEffectCanvas)) {
                ++renderer->draw_calls_last_frame;
                context->OMSetBlendState(pipeline.ui_blend_state, blend_factor, 0xFFFFFFFFu);
                bind_paint_pipeline();
            }
            if (!frame(false))
                return false;
        }
    }
    const u32 bar_end = caption->tooltip_first_quad;
    const bool scrim = caption->scrim_quad < bar_end;
    const u32 buttons = scrim ? caption->buttons_first_quad : bar_end;
    if (buttons > first) {
        context->DrawInstanced(6, buttons - first, 0, first);
        ++renderer->draw_calls_last_frame;
    }
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
    const UINT glyph_stride = sizeof(GlyphQuad);
    ID3D11ShaderResourceView* glyph_views[]{renderer->glyph_text.gpu.texture_view,
                                            d3d11_glyph_text_color_view(renderer->glyph_text)};
    const auto draw_glyphs = [&](u32 first_glyph, u32 count) noexcept {
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->IASetInputLayout(pipeline.glyph_input_layout);
        context->IASetVertexBuffers(0, 1, &caption->glyph_buffer, &glyph_stride, &offset);
        context->VSSetShader(pipeline.glyph_vertex_shader, nullptr, 0);
        context->PSSetShader(pipeline.glyph_pixel_shader, nullptr, 0);
        context->PSSetShaderResources(0, 2, glyph_views);
        context->DrawInstanced(4, count, 0, first_glyph);
        ++renderer->draw_calls_last_frame;
    };
    if (glyph_count)
        draw_glyphs(0, glyph_count);
    if (scrim) {
        // The scrim over band, icon, title and items; the window buttons
        // follow undimmed.
        bind_paint_pipeline();
        context->DrawInstanced(6, 1, 0, caption->scrim_quad);
        if (caption->scrim_quad > buttons)
            context->DrawInstanced(6, caption->scrim_quad - buttons, 0, buttons);
        renderer->draw_calls_last_frame += 2;
    }
    if (caption->paint.count > bar_end) {
        // The tooltip hangs below the band: caption DIPs over the whole target.
        const u32 target_height = renderer->caption_height + renderer->height;
        const D3D11_VIEWPORT surface{0, 0, static_cast<float>(renderer->width), static_cast<float>(target_height),
                                     0, 1};
        const D3D11_RECT clip{maximum<LONG>(0, caption->tooltip_pixels.left),
                              maximum<LONG>(0, caption->tooltip_pixels.top),
                              minimum<LONG>(static_cast<LONG>(renderer->width), caption->tooltip_pixels.right),
                              minimum<LONG>(static_cast<LONG>(target_height), caption->tooltip_pixels.bottom)};
        if (!d3d11_draw_pipeline_update_caption(pipeline, context, renderer->width, target_height, 0,
                                                renderer->dpi, false))
            return false;
        bind_paint_pipeline();
        context->RSSetViewports(1, &surface);
        context->RSSetScissorRects(1, &clip);
        context->DrawInstanced(6, caption->paint.count - bar_end, 0, bar_end);
        ++renderer->draw_calls_last_frame;
        if (tooltip_glyphs)
            draw_glyphs(glyph_count, tooltip_glyphs);
    }
    ID3D11ShaderResourceView* null_views[3]{};
    context->PSSetShaderResources(0, 3, null_views);
    caption->drawn = true;
    ++caption->draws;
    return d3d11_draw_pipeline_update_transform(pipeline, context, renderer->width, renderer->height,
                                                renderer->dpi, renderer->target_origin_y);
}

u32 d3d11_renderer_caption_dirty_rect(const D3D11Renderer& renderer, RECT* rectangle) noexcept {
    if (!renderer.caption || !renderer.caption->drawn || !renderer.caption_height)
        return 0;
    // The band and the tooltip below it, as one rectangle.
    const RECT& tooltip = renderer.caption->tooltip_pixels;
    const LONG bottom = maximum(static_cast<LONG>(renderer.caption_height),
                                minimum(tooltip.bottom, static_cast<LONG>(renderer.caption_height + renderer.height)));
    *rectangle = {0, 0, static_cast<LONG>(renderer.width), bottom};
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

void d3d11_renderer_caption_begin_scrim(D3D11Renderer* renderer) noexcept {
    if (renderer->caption)
        renderer->caption->scrim = {};
}

void d3d11_renderer_caption_note_scrim(D3D11Renderer* renderer, Color color, float opacity) noexcept {
    auto* caption = renderer->caption;
    if (!caption || !(opacity > 0 && opacity <= 1) || !(color.alpha > 0 && color.alpha < 1))
        return;
    // Source over the scrims before it, in straight alpha.
    const float a = color.alpha * opacity;
    Color& s = caption->scrim;
    const float alpha = a + s.alpha * (1 - a);
    const auto mix = [&](float top, float below) noexcept { return (top * a + below * s.alpha * (1 - a)) / alpha; };
    s = {mix(color.red, s.red), mix(color.green, s.green), mix(color.blue, s.blue), alpha};
}

void d3d11_renderer_caption_invalidate(D3D11Renderer* renderer) noexcept {
    if (!renderer->caption)
        return;
    // Both buffers draw the bar, and its tooltip, again.
    renderer->caption->dirty = true;
}

u32 d3d11_renderer_caption_builds(const D3D11Renderer& renderer) noexcept {
    return renderer.caption ? renderer.caption->builds : 0;
}

u32 d3d11_renderer_caption_draws(const D3D11Renderer& renderer) noexcept {
    return renderer.caption ? renderer.caption->draws : 0;
}

void d3d11_renderer_caption_release(D3D11Renderer* renderer) noexcept {
    auto* caption = renderer->caption;
    if (!caption)
        return;
    com_release(caption->paint_buffer);
    com_release(caption->glyph_buffer);
    com_release(caption->icon_view);
    com_release(caption->icon_quad);
    shaped_text_reset(&caption->title_line.shape);
    shaped_text_reset(&caption->tooltip_line.shape);
    paint_stream_release(&caption->paint);
    VirtualFree(caption, 0, MEM_RELEASE);
    renderer->caption = nullptr;
}

} // namespace maicord
