// Partial frames against full renders. After every present, the image the
// swap chain just showed must equal a full redraw of the same state over the
// content rows, so damage that misses a changed pixel (in this frame or in the
// frame the other flip buffer last showed) fails. Covers timeline and picker
// scrolling, hover, media pixels and an animated emoji changing under a
// retained frame, background effect ticks, a theme switch and a resize.
#include "ui_probe_internal.hpp"
#include "probe_media_upload.hpp"
#include "probe_present.hpp"
#include "maicord/ui/chat/composer/message_composer_scene.hpp"
#include "../../src/render/d3d11/renderer_draw.hpp"
#include "maicord/app/chat/messages/view_runtime.hpp"
#include "maicord/media/image/gif.hpp"
#include "maicord/ui/media/picker.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord::ui_probe_detail {
namespace {

struct Gate {
    D3D11Renderer& r;
    ID3D11Texture2D* reference = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    ID3D11Texture2D* staging[2]{};
    u32 width = 0, height = 0;
    u32 frames = 0, partial = 0;
    explicit Gate(D3D11Renderer& renderer) noexcept : r(renderer) {}
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;
    ~Gate() noexcept {
        release();
    }
    void release() noexcept {
        com_release(target);
        com_release(reference);
        com_release(staging[0]);
        com_release(staging[1]);
        width = height = 0;
    }
    bool ensure() noexcept {
        const u32 w = r.graphics.surface.buffer_width, h = r.graphics.surface.buffer_height;
        if (target && w == width && h == height)
            return true;
        release();
        D3D11_TEXTURE2D_DESC description{};
        description.Width = w;
        description.Height = h;
        description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(r.graphics.device->CreateTexture2D(&description, nullptr, &reference)) ||
            FAILED(r.graphics.device->CreateRenderTargetView(reference, nullptr, &target)))
            return false;
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto*& texture : staging)
            if (FAILED(r.graphics.device->CreateTexture2D(&description, nullptr, &texture)))
                return false;
        width = w;
        height = h;
        return true;
    }
    // A full draw of the retained stream into `target`, at the content's own
    // origin, leaving the renderer's counters and deadlines as the frame left them.
    bool draw_reference(const Theme& theme) noexcept {
        const u32 draws = r.draw_calls_last_frame, media = r.media_draws_last_frame, total = r.media_draws_total;
        const u64 effect_next = r.text_effect_next_ms;
        ID3D11RenderTargetView* presented = r.graphics.surface.target;
        r.graphics.surface.target = target;
        // At the frame's own effect clock: draw_stream set the next deadline 42 ms past it.
        const bool drawn = d3d11_renderer_draw_stream(&r, theme, false, false, nullptr, effect_next - 42);
        r.graphics.surface.target = presented;
        r.graphics.context->OMSetRenderTargets(1, &r.graphics.surface.target, nullptr);
        r.text_effect_next_ms = effect_next;
        r.draw_calls_last_frame = draws;
        r.media_draws_last_frame = media;
        r.media_draws_total = total;
        return drawn;
    }
    bool check(const Theme& theme, u32 stage) noexcept {
        const u32 full = r.full_present_draws, parts = r.partial_present_draws;
        if (!probe_present(&r, theme) || !ensure())
            return fail(stage, 1);
        ++frames;
        partial += r.partial_present_draws != parts ? 1u : 0u;
        if (r.full_present_draws == full && r.partial_present_draws == parts)
            return true; // nothing changed: nothing presented
        // Flip-sequential: buffer 1 is the image Present just queued.
        ID3D11Texture2D* shown = nullptr;
        if (FAILED(r.graphics.surface.swap_chain->GetBuffer(1, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&shown))))
            return fail(stage, 2);
        // Pixels no quad paints (outside the root's corners) keep whatever the
        // buffer held; the full draw replaces every other pixel, the opaque
        // root first.
        r.graphics.context->CopyResource(staging[0], shown);
        r.graphics.context->CopyResource(reference, shown);
        com_release(shown);
        if (!draw_reference(theme))
            return fail(stage, 3);
        r.graphics.context->CopyResource(staging[1], reference);
        D3D11_MAPPED_SUBRESOURCE a{}, b{};
        if (FAILED(r.graphics.context->Map(staging[0], 0, D3D11_MAP_READ, 0, &a)))
            return fail(stage, 4);
        if (FAILED(r.graphics.context->Map(staging[1], 0, D3D11_MAP_READ, 0, &b))) {
            r.graphics.context->Unmap(staging[0], 0);
            return fail(stage, 4);
        }
        u32 wrong = 0, first_x = 0, first_y = 0, delta = 0;
        for (u32 y = r.target_origin_y; y < r.target_origin_y + r.height && y < height; ++y) {
            const auto* p = static_cast<const u8*>(a.pData) + y * a.RowPitch;
            const auto* q = static_cast<const u8*>(b.pData) + y * b.RowPitch;
            for (u32 x = 0; x < r.width && x < width; ++x) {
                for (u32 c = 0; c < 3; ++c) {
                    const u32 d = p[x * 4 + c] > q[x * 4 + c] ? p[x * 4 + c] - q[x * 4 + c]
                                                              : q[x * 4 + c] - p[x * 4 + c];
                    if (d) {
                        if (!wrong++)
                            first_x = x, first_y = y;
                        delta = d > delta ? d : delta;
                        break;
                    }
                }
            }
        }
        r.graphics.context->Unmap(staging[0], 0);
        r.graphics.context->Unmap(staging[1], 0);
        if (!wrong)
            return true;
        log_u64(LogLevel::error, "probe", "frame_damage_wrong_pixels", wrong);
        log_u64(LogLevel::error, "probe", "frame_damage_first_x", first_x);
        log_u64(LogLevel::error, "probe", "frame_damage_first_y", first_y);
        log_u64(LogLevel::error, "probe", "frame_damage_max_delta", delta);
        log_u64(LogLevel::error, "probe", "frame_damage_partial", r.partial_present_draws != parts);
        return fail(stage, 5);
    }
    static bool fail(u32 stage, u32 step) noexcept {
        log_u64(LogLevel::error, "probe", "frame_damage_failed_stage", stage);
        log_u64(LogLevel::error, "probe", "frame_damage_failed_step", step);
        return false;
    }
};

struct PickerSource {
    u32 count;
};
bool resolve_emoji(void* opaque, u32 i, UiMediaPickerItem* out) noexcept {
    if (!out || i >= static_cast<PickerSource*>(opaque)->count)
        return false;
    *out = {};
    out->id = i + 9000ull;
    out->label = utf8_literal("Original emoji fixture");
    out->enabled = true;
    out->kind = UiMediaPickerItemKind::emoji;
    out->width = out->height = 16;
    if (i % 4 == 1)
        out->asset_id = 0x7a11;
    else
        out->glyph = i % 4 ? utf8_literal("🌿") : utf8_literal("😀");
    return true;
}

bool media_pixels(D3D11Renderer& r, u8 shade) noexcept {
    u8 pixels[16 * 16 * 4];
    for (u32 i = 0; i < 16 * 16; ++i) {
        pixels[i * 4] = shade;
        pixels[i * 4 + 1] = static_cast<u8>(255 - shade);
        pixels[i * 4 + 2] = 96;
        pixels[i * 4 + 3] = 255;
    }
    return renderer_upload_media_rgba8(&r, media_asset_custom_emoji(0x7a11, false), 16, 16, 64,
                                       {pixels, sizeof(pixels)})
        .ok();
}

} // namespace

bool frame_damage_ui_probe() noexcept {
    D3D11Renderer& r = g_probe.renderer;
    Gate gate{r};
    const Theme theme = kDefaultTheme;
    u32 stage = 1;
    // A timeline long enough to scroll; the probe scene holds 32 rows.
    constexpr u64 base = 1'280'000'000'100'000'000ull;
    for (u32 i = 0; i < 24; ++i) {
        const u64 author = base + 10000 + (i % 3 == 0);
        const auto stored = message_store_upsert(
            &g_probe.message_view.store,
            {base + i, 111, author, snowflake_timestamp_ms(base + i), 0,
             utf8_literal("Original native row that wraps across the timeline width so that scrolling moves "
                          "several glyph runs and headers at once, line after line, through the message view."),
             0});
        if (!stored.ok() || !user_store_upsert(&g_probe.users, author, utf8_literal("Native Author")).ok()) {
            log_u64(LogLevel::error, "probe", "frame_damage_store_error", static_cast<u32>(stored.error));
            return Gate::fail(stage, 10);
        }
    }
    if (!message_view_runtime_reindex(&g_probe.message_view,
                                      ui_extent_units(kUiBootstrapMessageRowHeight), 1, false)
             .ok())
        return Gate::fail(stage, 11);
    if (!media_pixels(r, 40))
        return Gate::fail(stage, 12);
    if (!rebuild_probe_content_text())
        return Gate::fail(stage, 13);
    if (!gate.check(theme, stage))
        return false;
    // Timeline scroll, down then back up.
    stage = 2;
    auto& timeline = g_probe.message_view.timeline;
    const u64 maximum = timeline.list->total_units > timeline.viewport_units
                            ? timeline.list->total_units - timeline.viewport_units
                            : 0;
    if (!maximum)
        return Gate::fail(stage, 0);
    u64 position = timeline.scroll_units;
    for (u32 step = 0; step < 12; ++step) {
        const u64 stride = 23 * kUiExtentUnitsPerDip / 2;
        position = step < 6 ? (position + stride < maximum ? position + stride : maximum)
                            : (position > stride ? position - stride : 0);
        if (!message_timeline_set_scroll(&timeline, position, false) || !rebuild_probe_content_text() ||
            !gate.check(theme, stage))
            return false;
    }
    // Hover over rows.
    stage = 3;
    auto& scene = g_probe.message_scene;
    for (u32 i = 0; i < 6 && scene.count; ++i) {
        ui_widget_manager_hover(&scene.widgets, scene.rows[(i * 3) % scene.count].widget);
        scene.widgets.hover_started_ms = 2000 + i;
        if (!rebuild_probe_content_text() || !gate.check(theme, stage))
            return false;
    }
    ui_widget_manager_hover(&scene.widgets, ui_widget_invalid_handle());
    if (!rebuild_probe_content_text() || !gate.check(theme, stage))
        return false;
    // The emoji picker: open, scroll down and up, media under it changing.
    stage = 4;
    const auto* anchor = r.message_composer_scene
                             ? message_composer_scene_layout_node(*r.message_composer_scene,
                                                                  MessageComposerControl::gif)
                             : nullptr;
    if (!anchor)
        return Gate::fail(stage, 0);
    const float scale = static_cast<float>(r.dpi) / 96.f;
    PickerSource fixture{480};
    UiMediaPickerSection sections[6]{};
    for (u32 i = 0; i < 6; ++i)
        sections[i] = {i + 1ull, i * 80, 80, utf8_literal("Original Emoji"), true};
    UiMediaPickerSource source{&fixture, 0x93, 1, fixture.count, 1, {sections, 6}, resolve_emoji};
    UiMediaPickerContent content{};
    content.account_scope = source.account_scope;
    content.query_generation = source.query_generation;
    content.revision = 1;
    content.available_tabs = 7;
    content.kind = UiMediaPickerKind::emoji;
    content.edit = {utf8_literal(""), 0, 0, true, false, false};
    content.placeholder = utf8_literal("Original offline query");
    content.status = utf8_literal("Original fixture footer");
    content.phase = UiMediaPickerPhase::ready;
    UiMediaPickerScene picker{};
    struct Restore {
        D3D11Renderer& r;
        UiMediaPickerScene& picker;
        const UiMediaPickerScene* previous;
        ~Restore() noexcept {
            r.media_picker_scene = previous;
            r.retained_presentation.valid = false;
            ui_media_picker_scene_release(&picker);
        }
    } restore{r, picker, r.media_picker_scene};
    if (!ui_media_picker_scene_update(&picker, &source, &content, {0, 0, r.width / scale, r.height / scale},
                                      anchor->rectangle))
        return Gate::fail(stage, 0);
    r.media_picker_scene = &picker;
    r.scrollbar_viewport = picker.body;
    r.retained_presentation.valid = false;
    if (!renderer_refresh_interaction_text(&r, theme) || !gate.check(theme, stage))
        return false;
    // The pointer over cells: hover highlights and the footer preview.
    for (u32 i = 0; i < 8; ++i) {
        ui_media_picker_scene_hover(&picker, picker.body.left + 18.f + 34.f * static_cast<float>(i % 4),
                                    picker.body.top + 18.f + 34.f * static_cast<float>(i / 4), 3000 + i);
        if (!renderer_refresh_interaction_text(&r, theme) || !gate.check(theme, stage))
            return false;
    }
    for (u32 step = 0; step < 16; ++step) {
        if (!ui_media_picker_scene_scroll(&picker, step < 10 ? 8.5f : -13.f))
            return Gate::fail(stage, 1);
        r.scrollbar_viewport = picker.body;
        if (step == 5 && !media_pixels(r, 200))
            return Gate::fail(stage, 2);
        if (!renderer_refresh_interaction_text(&r, theme) || !gate.check(theme, stage))
            return false;
    }
    stage = 5;
    if (!media_pixels(r, 120) || !gate.check(theme, stage))
        return false;
    // The emoji turns animated: its frames advance in place under the
    // retained frame, so only the cells showing it repaint.
    const MediaAssetKey animated = media_asset_custom_emoji(0x7a11, false);
    GifFrame gif{};
    gif.width = gif.height = 16;
    gif.dispose = GifDispose::none;
    GifPalette palette{};
    palette.count = 2;
    palette.rgba[0] = 0xff2050c0u;
    palette.rgba[1] = 0xffc0a020u;
    u8 indices[16 * 16];
    for (u32 i = 0; i < 16 * 16; ++i)
        indices[i] = static_cast<u8>(((i >> 4) + i) & 1);
    if (!renderer_begin_gif(&r, animated, 16, 16, gif, palette, {indices, sizeof(indices)}).ok() ||
        !gate.check(theme, stage))
        return Gate::fail(stage, 3);
    for (u32 step = 0; step < 8; ++step) {
        palette.rgba[0] = 0xff000000u | (step * 0x1f1f1fu);
        if (!renderer_advance_gif(&r, animated, gif, gif, palette, {indices, sizeof(indices)}).ok() ||
            !gate.check(theme, stage))
            return Gate::fail(stage, 4);
    }
    r.media_picker_scene = restore.previous;
    r.retained_presentation.valid = false;
    if (!rebuild_probe_content_text() || !gate.check(theme, stage))
        return false;
    // Background effect ticks, including frames where several ticks land
    // before one present.
    stage = 6;
    Theme stars = theme;
    stars.background = {{0, 0, 0, 0, 0}, 0, 0, 0, BackgroundEffect::stars_and_comets, 100, 70, 30};
    float phase = 3.f;
    if (!renderer_set_background_phase(&r, phase) || !rebuild_probe_content_text() || !gate.check(stars, stage))
        return false;
    for (u32 tick = 0; tick < 24; ++tick) {
        phase += tick % 5 == 4 ? .9f : .042f;
        if (!renderer_set_background_phase(&r, phase) ||
            (tick % 7 == 3 && !renderer_set_background_phase(&r, phase += .042f)) || !gate.check(stars, stage))
            return false;
    }
    // A theme switch, then ticks again; a resize, then ticks again.
    stage = 7;
    Theme light = kLightTheme;
    light.background = stars.background;
    if (!rebuild_probe_content_text() || !gate.check(light, stage))
        return false;
    for (u32 tick = 0; tick < 4; ++tick)
        if (!renderer_set_background_phase(&r, phase += .042f) || !gate.check(light, stage))
            return false;
    stage = 8;
    const u32 width = r.width, height = r.height;
    if (!probe_resize_surface(width - 37, height - 23) || !rebuild_probe_content_text() || !gate.check(stars, stage))
        return false;
    for (u32 tick = 0; tick < 4; ++tick)
        if (!renderer_set_background_phase(&r, phase += .042f) || !gate.check(stars, stage))
            return false;
    if (!probe_resize_surface(width, height) || !renderer_set_background_phase(&r, 0) ||
        !rebuild_probe_content_text() || !gate.check(theme, stage))
        return false;
    log_u64(LogLevel::info, "probe", "frame_damage_frames", gate.frames);
    log_u64(LogLevel::info, "probe", "frame_damage_partial_frames", gate.partial);
    // At least the animated emoji's frames must have been partial frames to judge.
    return gate.partial >= 8;
}

} // namespace maicord::ui_probe_detail
