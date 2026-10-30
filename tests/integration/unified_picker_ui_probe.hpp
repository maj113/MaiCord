#pragma once
#include "channel_overview_probe.hpp"
#include "maicord/ui/media_picker_presentation.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/app/client_theme.hpp"
#include "../../src/render/d3d11_renderer_interaction_state.hpp"

#include "picker_text_capacity_probe.hpp"

namespace maicord {
namespace unified_picker_probe_detail {
struct Source {
    UiMediaPickerKind kind;
    u32 calls;
    u32 flag;
};
inline bool resolve(void* opaque, u32 i, UiMediaPickerItem* out) noexcept {
    auto& source = *static_cast<Source*>(opaque);
    ++source.calls;
    if (!out || i >= 200)
        return false;
    *out = {};
    out->id = i + 1000ull;
    out->label = utf8_literal("Original native fixture");
    out->detail = utf8_literal("Offline synthetic item");
    out->enabled = true;
    out->width = out->height = 16;
    out->kind = source.kind == UiMediaPickerKind::gif       ? UiMediaPickerItemKind::gif
                : source.kind == UiMediaPickerKind::sticker ? UiMediaPickerItemKind::sticker
                                                            : UiMediaPickerItemKind::emoji;
    if (source.kind == UiMediaPickerKind::emoji) {
        if (i % 3 == 0)
            out->glyph = utf8_literal("😀");
        else if (i % 3 == 1)
            out->asset_id = 0x71337;
        else {
            out->glyph = utf8_literal("🇺🇸");
            out->regional_flag = source.flag;
        }
    }
    return true;
}
inline bool text_ink(const UiMediaPickerScene& scene, const D3D11Renderer& r,
                     const surface_motion_probe_detail::Readback& rb, u32 image,
                     Utf8View expected) noexcept {
    UiTextCommand commands[160]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    if (!ui_text_command_stream_initialize(&stream, {commands, 160}) ||
        !ui_media_picker_scene_text_output(scene, kDefaultTheme, &output, &r.media_picker_edit_geometry))
        return false;
    for (u32 i = 0; i < stream.count; ++i)
        if (utf8_equal(commands[i].text, expected) &&
            channel_overview_probe_detail::ink(
                rb, image, ui_rect_intersection(commands[i].rectangle, commands[i].clip),
                static_cast<float>(r.dpi) / 96.f, commands[i].color, 6))
            return true;
    return false;
}
inline bool colored_glyph_ink(const surface_motion_probe_detail::Readback& rb, u32 image, UiRect area,
                              float scale) noexcept {
    const auto clip = raster_clip_rectangle(area, scale, rb.width, rb.height);
    const auto c = kDefaultTheme.base_low;
    const u8 background[]{static_cast<u8>(c.blue * 255), static_cast<u8>(c.green * 255),
                          static_cast<u8>(c.red * 255), 255};
    u32 pixels = 0;
    for (i32 y = clip.top; y < clip.bottom; ++y)
        for (i32 x = clip.left; x < clip.right; ++x) {
            const auto* p =
                rb.images + image * rb.bytes + (static_cast<u32>(y) * rb.width + static_cast<u32>(x)) * 4;
            if (surface_motion_probe_detail::distance(p, background) > 80 && ++pixels >= 8)
                return true;
        }
    return false;
}
inline bool clips_current(const UiMediaPickerScene& scene) noexcept {
    // Cells clip below a pinned section header.
    auto grid = scene.body;
    grid.top = maximum(grid.top, scene.pinned_bottom);
    for (u32 i = 0; i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        if (!ui_rect_equal(cell.clip, ui_rect_intersection(cell.rectangle, grid)) ||
            cell.clip.top < scene.body.top || cell.clip.bottom > scene.body.bottom)
            return false;
    }
    for (u32 i = 0; i < scene.header_count; ++i) {
        const auto& slot = scene.projection->headers[i];
        const auto* n = ui_widget_manager_layout_node(scene.widgets, slot.widget);
        if (!n || !ui_rect_equal(n->rectangle, ui_rect_intersection(slot.rectangle, scene.body)))
            return false;
    }
    for (u32 i = 0; i < scene.rail_count; ++i) {
        const auto& slot = scene.projection->rail[i];
        const auto* n = ui_widget_manager_layout_node(scene.widgets, slot.widget);
        if (!n || !ui_rect_equal(n->rectangle, ui_rect_intersection(slot.rectangle, scene.rail)))
            return false;
    }
    return true;
}
inline bool distinct_rail_ink(const UiMediaPickerScene& scene,
                              const surface_motion_probe_detail::Readback& rb, float scale) noexcept {
    if (scene.rail_count < 2)
        return false;
    const auto a = raster_clip_rectangle(scene.projection->rail[0].rectangle, scale, rb.width, rb.height);
    const auto b = raster_clip_rectangle(scene.projection->rail[1].rectangle, scale, rb.width, rb.height);
    const i32 width = minimum(a.right - a.left, b.right - b.left);
    const i32 height = minimum(a.bottom - a.top, b.bottom - b.top);
    u32 differences = 0;
    for (i32 y = 0; y < height; ++y)
        for (i32 x = 0; x < width; ++x) {
            const auto* left =
                rb.images + (static_cast<u32>(a.top + y) * rb.width + static_cast<u32>(a.left + x)) * 4;
            const auto* right =
                rb.images + (static_cast<u32>(b.top + y) * rb.width + static_cast<u32>(b.left + x)) * 4;
            if (surface_motion_probe_detail::distance(left, right) > 40 && ++differences >= 8)
                return true;
        }
    return false;
}
// The composer's inline /gif strip and /sticker list (the official client's
// integration autocomplete rows) from one original fixture: GIF tiles as
// catalog previews (solid green), sticker rows with their name, a
// "Similar to:" note and the pack, the heading with its bold query.
struct InlineSource {
    UiMediaPickerKind kind;
};
inline bool inline_resolve(void* opaque, u32 i, UiMediaPickerItem* out) noexcept {
    const auto& source = *static_cast<InlineSource*>(opaque);
    if (!out || i >= 6)
        return false;
    *out = {};
    out->id = 0x7100 + i;
    // The catalog preview key the host draws (its format lives in the flags).
    const auto key = media_asset_catalog_preview(0x7100 + i, 1, source.kind == UiMediaPickerKind::gif
                                                                    ? MediaAttachmentFormat::webp
                                                                    : MediaAttachmentFormat::png);
    out->asset_id = key.id;
    out->variant = key.variant;
    out->asset_flags = key.flags;
    out->enabled = true;
    if (source.kind == UiMediaPickerKind::gif) {
        out->kind = UiMediaPickerItemKind::gif;
        out->label = utf8_literal("Original fixture GIF");
        out->width = 48 + i * 16;
        out->height = 32;
    } else {
        out->kind = UiMediaPickerItemKind::sticker;
        out->label = i ? utf8_literal("Original sticker") : utf8_literal("Original wave");
        out->detail = utf8_literal("Original pack");
        if (i == 1)
            out->note = utf8_literal("Similar to: original pack");
        out->width = out->height = 32;
    }
    return true;
}
} // namespace unified_picker_probe_detail
inline bool inline_results_ui_probe(D3D11Renderer& renderer, surface_motion_probe_detail::Readback& rb,
                                    bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept {
    using namespace unified_picker_probe_detail;
    using channel_overview_probe_detail::ink;
    const auto bad = [](u32 step) noexcept {
        log_u64(LogLevel::error, "probe", "inline_results_failure_step", step);
        return false;
    };
    const auto* composer = renderer.message_composer_scene
                               ? ui_widget_manager_layout_node(renderer.message_composer_scene->widgets,
                                                              renderer.message_composer_scene->surface)
                               : nullptr;
    if (!composer)
        return bad(1);
    const float scale = static_cast<float>(renderer.dpi) / 96.f;
    const UiRect viewport{0, 0, renderer.width / scale, renderer.height / scale};
    // The commands popup's option bar sits on the composer; results float above it.
    const UiRect anchor{composer->rectangle.left, composer->rectangle.top - 48, composer->rectangle.right,
                        composer->rectangle.top};
    u8 green[16 * 16 * 4]{};
    for (u32 i = 0; i < 16 * 16; ++i) {
        green[i * 4] = 48;
        green[i * 4 + 1] = 184;
        green[i * 4 + 2] = 128;
        green[i * 4 + 3] = 255;
    }
    const MediaAttachmentFormat formats[]{MediaAttachmentFormat::webp, MediaAttachmentFormat::png};
    for (u32 i = 0; i < 6; ++i)
        for (const auto format : formats)
            if (!renderer_upload_media_straight_rgba8(&renderer, media_asset_catalog_preview(0x7100 + i, 1, format),
                                                      16, 16, 64, {green, sizeof(green)})
                     .ok())
                return bad(2);
    UiMediaPickerScene scene{};
    struct Restore {
        D3D11Renderer& r;
        UiMediaPickerScene& scene;
        const UiMediaPickerScene* gallery;
        ~Restore() noexcept {
            r.media_picker_scene = gallery;
            r.retained_presentation.valid = false;
            ui_media_picker_scene_release(&scene);
        }
    } restore{renderer, scene, renderer.media_picker_scene};
    InlineSource fixture{UiMediaPickerKind::gif};
    UiMediaPickerSource source{&fixture, 0x72, 1, 6, 1, {}, inline_resolve};
    UiMediaPickerContent content{};
    content.account_scope = source.account_scope;
    content.query_generation = source.query_generation;
    content.revision = 1;
    content.kind = UiMediaPickerKind::gif;
    content.available_tabs = ui_media_picker_tab_bit(UiMediaPickerKind::gif);
    content.variant = UiMediaPickerVariant::inline_results;
    content.phase = UiMediaPickerPhase::loading;
    content.heading = utf8_literal("Gif content matching original");
    content.heading_plain = 21;
    const auto render = [&](u32 image) noexcept {
        renderer.retained_presentation.valid = false;
        return renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
               probe_present(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
               surface_motion_probe_detail::draw_fixed(renderer) && rb.read(renderer, image);
    };
    // Loading: only the spinner, 48 DIP above the option bar's 8 DIP gap.
    if (!ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) ||
        !ui_media_picker_scene_spinner_visible(scene) || ui_rect_height(scene.panel) != 48 ||
        scene.panel.bottom != anchor.top - kUiFloatingSurfaceInset || scene.panel.left != anchor.left ||
        scene.panel.right != anchor.right)
        return bad(3);
    renderer.media_picker_scene = &scene;
    if (!render(0) || (capture && !capture(L"inline-gif-loading.bmp", kDefaultTheme)))
        return bad(4);
    content.phase = UiMediaPickerPhase::ready;
    ++content.revision;
    if (!ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) ||
        !ui_media_picker_scene_select(&scene, 0) || !scene.cell_count ||
        ui_rect_height(scene.panel) !=
            kUiInlineResultsHeadingHeight + kUiInlineResultsGifExtent + kUiInlineResultsPadding * 3)
        return bad(5);
    const auto& first = scene.projection->cells[0];
    if (ui_rect_height(first.rectangle) != kUiInlineResultsGifExtent ||
        ui_rect_width(first.rectangle) != kUiInlineResultsGifExtent * 48 / 32 || !render(1) ||
        !text_ink(scene, renderer, rb, 1, content.heading) ||
        !ink(rb, 1, ui_rect_inset(first.clip, {8, 8, 8, 8}), scale, {48.f / 255, 184.f / 255, 128.f / 255, 1}, 8) ||
        (capture && !capture(L"inline-gif.bmp", kDefaultTheme)))
        return bad(6);
    // Paged by its arrow: the strip eases there (captured halfway), and the
    // back arrow appears with the strip scrolled.
    if (!scene.page_widgets[1].generation || !ui_media_picker_scene_page(&scene, true, 1'000, 1, false) ||
        !ui_media_picker_scene_paging(scene) ||
        !ui_media_picker_scene_page_tick(&scene, 1'000 + scene.page_duration_ms / 2) ||
        !(scene.scroll_dips > 0 && scene.scroll_dips < scene.page_to) || !scene.page_widgets[0].generation ||
        !render(2) || (capture && !capture(L"inline-gif-paging.bmp", kDefaultTheme)))
        return bad(7);
    if (!ui_media_picker_scene_page_tick(&scene, 1'000 + scene.page_duration_ms) ||
        ui_media_picker_scene_paging(scene) || scene.scroll_dips != scene.page_to || !render(2) ||
        (capture && !capture(L"inline-gif-paged.bmp", kDefaultTheme)))
        return bad(9);
    fixture.kind = content.kind = UiMediaPickerKind::sticker;
    content.available_tabs = ui_media_picker_tab_bit(UiMediaPickerKind::sticker);
    content.heading = utf8_literal("Stickers matching original");
    content.heading_plain = 18;
    ++source.revision;
    ++source.query_generation;
    content.query_generation = source.query_generation;
    ++content.revision;
    if (!ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) ||
        !ui_media_picker_scene_select(&scene, 0) || scene.cell_count != 6 ||
        ui_rect_height(scene.panel) != kUiInlineResultsHeadingHeight + 6 * 48 + kUiInlineResultsPadding ||
        !render(3) || !text_ink(scene, renderer, rb, 3, utf8_literal("Original wave")) ||
        !text_ink(scene, renderer, rb, 3, utf8_literal("Similar to: original pack")) ||
        !text_ink(scene, renderer, rb, 3, utf8_literal("Original pack")) ||
        (capture && !capture(L"inline-sticker.bmp", kDefaultTheme)))
        return bad(8);
    log_write(LogLevel::info, "probe", "inline_results_native_gpu_passed");
    return true;
}

// Native GPU fixture with optional original-content capture. Uses borrowed
// production adapters and solid RGBA samples, never account/provider resources.
inline bool unified_picker_ui_probe(D3D11Renderer& renderer,
                                    bool (*capture)(const wchar_t*,
                                                    const Theme&) noexcept = nullptr) noexcept {
    using namespace unified_picker_probe_detail;
    using channel_overview_probe_detail::ink;
    u32 stage = 1;
    const auto fail = [&]() noexcept {
        log_u64(LogLevel::error, "probe", "unified_picker_failure_stage", stage);
        const char* categories[]{"owner_admission",
                                 "common_geometry",
                                 "upload_and_chrome_ink",
                                 "typed_glyph_media_domains",
                                 "native_scrollbar_feedback",
                                 "fractional_scroll_clips_background",
                                 "failed_phase_no_phantom_thumb"};
        log_write(LogLevel::error, "probe", categories[minimum(stage, 7u) - 1]);
        return false;
    };
    if (!renderer.ui_scene || !renderer.message_composer_scene || !renderer.content_text || !renderer.dpi)
        return fail();
    UiMediaPickerScene scene{};
    struct Restore {
        D3D11Renderer& r;
        UiMediaPickerScene& scene;
        const UiMediaPickerScene* gallery;
        const UiSearchScene* search;
        const UiSettingsScene* settings;
        const PopupScene* popup;
        TextFormSurface* form;
        const UiWidgetManager* focus;
        UiRect scrollbar;
        RendererLayerOpacity opacity;
        ~Restore() noexcept {
            r.media_picker_scene = gallery;
            r.search_scene = search;
            renderer_bind_settings_scene(&r, settings);
            renderer_bind_popup_scene(&r, popup);
            renderer_bind_text_form(&r, form);
            renderer_bind_focus_owner(&r, focus);
            r.scrollbar_viewport = scrollbar;
            r.layer_opacity = opacity;
            r.retained_presentation.valid = false;
            ui_media_picker_scene_release(&scene);
        }
    } restore{renderer,
              scene,
              renderer.media_picker_scene,
              renderer.search_scene,
              renderer.settings_scene,
              renderer.popup_scene,
              renderer.text_form,
              renderer.focus_owner,
              renderer.scrollbar_viewport,
              renderer.layer_opacity};
    renderer.search_scene = nullptr;
    renderer_bind_settings_scene(&renderer, nullptr);
    renderer_bind_popup_scene(&renderer, nullptr);
    renderer_bind_text_form(&renderer, nullptr);
    renderer_bind_focus_owner(&renderer, nullptr);
    renderer.layer_opacity = {1, 1, 1};
    const auto* anchor =
        message_composer_scene_layout_node(*renderer.message_composer_scene, MessageComposerControl::gif);
    if (!anchor)
        return fail();
    const float scale = static_cast<float>(renderer.dpi) / 96.f;
    const UiRect viewport{0, 0, renderer.width / scale, renderer.height / scale};
    Source fixture{UiMediaPickerKind::gif, 0, regional_flag_sequence(utf8_literal("🇺🇸"))};
    UiMediaPickerSection sections[20]{};
    for (u32 i = 0; i < 20; ++i) {
        sections[i] = {i + 1ull, i * 10, 10, utf8_literal("Original Emoji"), true};
        sections[i].visual = static_cast<UiMediaPickerSectionVisual>(1 + i % 10);
    }
    UiMediaPickerSource source{&fixture, 0x71, 1, 200, 1, {sections, 20}, resolve};
    UiMediaPickerContent content{};
    content.account_scope = source.account_scope;
    content.query_generation = source.query_generation;
    content.revision = 1;
    content.available_tabs = 7;
    content.edit = {utf8_literal("garden"), 6, 6, true, false, false};
    content.placeholder = utf8_literal("Original offline query");
    content.status = utf8_literal("Original fixture footer");
    content.phase = UiMediaPickerPhase::ready;
    UiRect panel{}, body{}, field{}, footer{};
    stage = 2;
    for (u32 kind = 0; kind < 3; ++kind) {
        fixture.kind = content.kind = static_cast<UiMediaPickerKind>(kind);
        ++source.revision;
        ++source.query_generation;
        content.query_generation = source.query_generation;
        ++content.revision;
        if (!ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor->rectangle) ||
            !scene.tab_widgets[0].generation || !scene.tab_widgets[1].generation ||
            !scene.tab_widgets[2].generation || scene.body.bottom != scene.footer.top ||
            !clips_current(scene))
            return fail();
        // GIFs have no inspector footer (Discord); Stickers and Emoji share it.
        if (!kind) {
            panel = scene.panel;
            field = scene.field;
            if (ui_rect_has_area(scene.footer) || scene.body.bottom != scene.panel.bottom)
                return fail();
        } else if (kind == 1) {
            body = scene.body;
            footer = scene.footer;
        }
        if (!ui_rect_equal(panel, scene.panel) || !ui_rect_equal(field, scene.field) ||
            (kind && (!ui_rect_equal(body, scene.body) || !ui_rect_equal(footer, scene.footer))))
            return fail();
    }
    renderer.media_picker_scene = &scene;
    surface_motion_probe_detail::Readback rb;
    if (!rb.initialize(renderer))
        return fail();
    const auto render = [&](u32 image) noexcept {
        renderer.scrollbar_viewport = scene.body;
        renderer.retained_presentation.valid = false;
        return renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
               probe_present(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
               surface_motion_probe_detail::draw_fixed(renderer) && rb.read(renderer, image);
    };
    stage = 3;
    u8 green[16 * 16 * 4]{}, purple[16 * 16 * 4]{};
    for (u32 i = 0; i < 16 * 16; ++i) {
        green[i * 4] = 48;
        green[i * 4 + 1] = 184;
        green[i * 4 + 2] = 128;
        green[i * 4 + 3] = 255;
        purple[i * 4] = 164;
        purple[i * 4 + 1] = 64;
        purple[i * 4 + 2] = 208;
        purple[i * 4 + 3] = 255;
    }
    if (!renderer_upload_media_rgba8(&renderer, media_asset_custom_emoji(0x71337, false), 16, 16, 64,
                                     {green, sizeof(green)})
             .ok() ||
        !renderer_upload_media_rgba8(&renderer, media_asset_regional_flag(static_cast<u16>(fixture.flag)),
                                     16, 16, 64, {purple, sizeof(purple)})
             .ok() ||
        !render(0) || !text_ink(scene, renderer, rb, 0, utf8_literal("GIFs")) ||
        !text_ink(scene, renderer, rb, 0, utf8_literal("Stickers")) ||
        !text_ink(scene, renderer, rb, 0, utf8_literal("Emoji")) ||
        !text_ink(scene, renderer, rb, 0, content.edit.text) ||
        !text_ink(scene, renderer, rb, 0, ui_media_picker_scene_footer_title(scene)) ||
        !distinct_rail_ink(scene, rb, scale))
        return fail();
    stage = 4;
    bool unicode = false, custom = false, flag = false;
    for (u32 i = 0; i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        UiMediaPickerMediaSlot slot{};
        if (cell.item.glyph.bytes && !cell.item.regional_flag)
            unicode = unicode || (channel_overview_probe_detail::glyphs_in(
                                      renderer, renderer.media_picker_text_first_batch,
                                      renderer.media_picker_text_end_batch, cell.clip) &&
                                  colored_glyph_ink(rb, 0, ui_rect_inset(cell.clip, {4, 4, 4, 4}), scale));
        if (!ui_media_picker_scene_media(scene, i, &slot))
            continue;
        if (slot.asset_id == 0x71337)
            custom = custom || ink(rb, 0, slot.clip, scale, {48.f / 255, 184.f / 255, 128.f / 255, 1}, 8);
        if (slot.regional_flag == fixture.flag)
            flag = flag || ink(rb, 0, slot.clip, scale, {164.f / 255, 64.f / 255, 208.f / 255, 1}, 8);
    }
    bool ordinary_domain = renderer.media_picker_media_end_batch > renderer.media_picker_media_first_batch;
    for (u32 i = renderer.media_picker_media_first_batch;
         ordinary_domain && i < renderer.media_picker_media_end_batch; ++i) {
        const auto& batch = renderer.media_draw.batch_data[i];
        // Cells clip to the body; the inspected item's preview to the footer.
        ordinary_domain = batch.clip.top >= scene.body.top && batch.clip.bottom <= scene.footer.bottom;
        for (u32 q = batch.first_quad; ordinary_domain && q < batch.first_quad + batch.quad_count; ++q)
            ordinary_domain = q < renderer.media_quad_count && renderer.media_draw.domain_data[q] == 0;
    }
    if (!unicode || !custom || !flag || !ordinary_domain)
        return fail();
    stage = 5;
    const auto bar = ui_media_picker_scene_scrollbar(scene);
    const auto railbar = ui_media_picker_scene_scrollbar(scene, true);
    const float wanted =
        maximum(24.f, (ui_rect_height(scene.body) - 8) * ui_rect_height(scene.body) / scene.content_height);
    const float difference = ui_rect_height(bar.thumb) - wanted;
    if (!bar.visible || !railbar.visible || difference < -.01f || difference > .01f ||
        bar.thumb.top != scene.body.top + 4 || bar.thumb.bottom > scene.body.bottom - 4 ||
        !ink(rb, 0, bar.thumb, scale, kDefaultTheme.interactive, 8) ||
        !ui_media_picker_scene_set_scrollbar_state(&scene, false, UiScrollbarVisualState::hovered) ||
        !render(1) || !ink(rb, 1, bar.thumb, scale, kDefaultTheme.interactive_hover, 8) ||
        !ui_media_picker_scene_set_scrollbar_state(&scene, false, UiScrollbarVisualState::pressed) ||
        !render(2) || !ink(rb, 2, bar.thumb, scale, kDefaultTheme.text_secondary, 8))
        return fail();
    stage = 6;
    const auto before_burst = surface_motion_probe_detail::snapshot(renderer);
    const u32 text_batches_before_burst = renderer.media_picker_text_end_batch;
    for (u32 update = 0; update < 64; ++update)
        if (!ui_media_picker_scene_scroll(&scene, 19.f / 64) ||
            !ui_media_picker_scene_scroll(&scene, 13.f / 64, true) || !clips_current(scene))
            return fail();
    if (!surface_motion_probe_detail::same(before_burst, surface_motion_probe_detail::snapshot(renderer)) ||
        renderer.media_picker_text_end_batch != text_batches_before_burst ||
        d3d11_renderer_interaction_text_state_current(renderer) || !ui_rect_equal(field, scene.field) ||
        !ui_rect_equal(footer, scene.footer))
        return fail();
    renderer.scrollbar_viewport = scene.body;
    renderer.retained_presentation.valid = false;
    // Production publishes deferred pointer changes at the final-frame guard.
    // Do not use render(), whose explicit rebuild would conceal this regression.
    if (!renderer_refresh_interaction_text(&renderer, kDefaultTheme) ||
        !d3d11_renderer_interaction_text_state_current(renderer) ||
        !probe_present(&renderer, kDefaultTheme) || !probe_present(&renderer, kDefaultTheme) ||
        !surface_motion_probe_detail::draw_fixed(renderer) || !rb.read(renderer, 3) ||
        !text_ink(scene, renderer, rb, 3, content.edit.text) ||
        !text_ink(scene, renderer, rb, 3, ui_media_picker_scene_footer_title(scene)) ||
        !text_ink(scene, renderer, rb, 3, sections[0].label))
        return fail();
    unicode = custom = flag = false;
    for (u32 i = 0; i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        UiMediaPickerMediaSlot slot{};
        if (cell.item.glyph.bytes && !cell.item.regional_flag)
            unicode = unicode || (channel_overview_probe_detail::glyphs_in(
                                      renderer, renderer.media_picker_text_first_batch,
                                      renderer.media_picker_text_end_batch, cell.clip) &&
                                  colored_glyph_ink(rb, 3, ui_rect_inset(cell.clip, {4, 4, 4, 4}), scale));
        if (!ui_media_picker_scene_media(scene, i, &slot))
            continue;
        if (slot.asset_id == 0x71337)
            custom = custom || ink(rb, 3, slot.clip, scale, {48.f / 255, 184.f / 255, 128.f / 255, 1}, 8);
        if (slot.regional_flag == fixture.flag)
            flag = flag || ink(rb, 3, slot.clip, scale, {164.f / 255, 64.f / 255, 208.f / 255, 1}, 8);
    }
    if (!unicode || !custom || !flag || !clips_current(scene))
        return fail();
    auto background = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content);
    background.left += 48;
    background.right = minimum(background.right, background.left + 280);
    background.top += 8;
    background.bottom = minimum(background.top + 100, scene.panel.top);
    if (!ui_rect_has_area(background) || !ink(rb, 3, background, scale, kDefaultTheme.text_primary, 6))
        return fail();
    stage = 7;
    content.phase = UiMediaPickerPhase::failed;
    content.status = utf8_literal("Offline catalog unavailable");
    ++content.revision;
    if (!ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor->rectangle) ||
        scene.content_height != 0 || scene.cell_count || ui_media_picker_scene_scrollbar(scene).visible ||
        ui_media_picker_scene_scrollbar(scene, true).visible || !render(3) ||
        !text_ink(scene, renderer, rb, 3, content.status) ||
        ink(rb, 3, bar.thumb, scale, kDefaultTheme.text_secondary, 8))
        return fail();
    // Exercise the same actual picker under a color theme; its opaque global
    // panel material must cover prior content and keep square rail glyphs.
    content.phase = UiMediaPickerPhase::ready;
    content.status = utf8_literal("Original themed picker fixture");
    ++content.revision;
    auto preferences = client_preferences_defaults();
    preferences.theme = ClientTheme::custom_gradient;
    preferences.gradient = {{0xff0099, 0x00bbdd, 0, 0, 0}, 180, 2, 60, BackgroundEffect::none, 50, 25, 35};
    const Theme themed = client_theme_projection(preferences);
    if (!ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor->rectangle) ||
        !renderer_rebuild_content_text(&renderer, themed) || !probe_present(&renderer, themed) ||
        !probe_present(&renderer, themed) ||
        (capture && !capture(L"unified-picker-gradient-emoji.bmp", themed)))
        return fail();
    // Fixed cells at two drawer widths: the wider pane fits more columns of
    // the same cells (Emoji switches to Discord's large size from 538 DIP).
    const UiMediaPickerKind captured[]{UiMediaPickerKind::emoji, UiMediaPickerKind::sticker};
    const wchar_t* names[2][2]{{L"picker-emoji-498.bmp", L"picker-emoji-760.bmp"},
                               {L"picker-sticker-498.bmp", L"picker-sticker-760.bmp"}};
    const float widths[]{0, 760};
    for (u32 k = 0; k < 2; ++k) {
        u16 columns = 0;
        float cell = 0;
        for (u32 w = 0; w < 2; ++w) {
            fixture.kind = content.kind = captured[k];
            ++source.revision;
            ++source.query_generation;
            content.query_generation = source.query_generation;
            ++content.revision;
            if (!ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor->rectangle,
                                              widths[w]) ||
                (w && (scene.columns <= columns ||
                       (captured[k] == UiMediaPickerKind::sticker && scene.cell_width != cell))) ||
                !renderer_rebuild_content_text(&renderer, kDefaultTheme) || !probe_present(&renderer, kDefaultTheme) ||
                !probe_present(&renderer, kDefaultTheme) || (capture && !capture(names[k][w], kDefaultTheme)))
                return fail();
            columns = scene.columns;
            cell = scene.cell_width;
        }
    }
    if (!inline_results_ui_probe(renderer, rb, capture)) return fail();
    if (!picker_text_capacity_probe(renderer)) return fail();
    log_write(LogLevel::info, "probe", "unified_picker_native_gpu_passed");
    return true;
}
} // namespace maicord
