#pragma once
#include "channel_overview_probe.hpp"
#include "maicord/ui/search_scene.hpp"
#include "maicord/ui/search_avatar_media.hpp"
#include "maicord/app/client_theme.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "../../src/render/d3d11_renderer_interaction_state.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace scoped_search_probe_detail {
inline bool resolve(void*, u32 index, UiSearchRow* out) noexcept {
    if (!out || index >= 6)
        return false;
    *out = {
        index < 2 ? 710ull : 720ull,
        810ull + index,
        index % 2 ? utf8_literal("Mira") : utf8_literal("Aster"),
        index < 2 ? utf8_literal("#release-planning") : utf8_literal("#native-ui"),
        utf8_literal("The release review includes original native controls and clear search results. "
                     "This longer synthetic excerpt wraps inside its own card without escaping the pane."),
        utf8_literal("10/03 12:30"),
        true,
        index == 0 || index == 2,
        0,
        0x771000 + index % 3,
        utf8_literal("0123456789abcdef0123456789abcdef")};
    return true;
}
inline bool text_ink(const UiSearchScene& scene, const D3D11Renderer& renderer,
                     const surface_motion_probe_detail::Readback& readback, u32 image, Utf8View expected,
                     bool require_wrap = false, const Theme& theme = kDefaultTheme) noexcept {
    UiTextCommand commands[128]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    if (!ui_text_command_stream_initialize(&stream, {commands, 128}) ||
        !ui_search_scene_text_output(scene, theme, &output, &renderer.search_edit_geometry))
        return false;
    for (u32 i = 0; i < stream.count; ++i) {
        const auto& command = commands[i];
        if (utf8_equal(command.text, expected) &&
            (!require_wrap ||
             (command.wrapping == UiTextWrapping::wrap && ui_rect_height(command.clip) > 40)) &&
            channel_overview_probe_detail::ink(readback, image,
                                               ui_rect_intersection(command.rectangle, command.clip),
                                               static_cast<float>(renderer.dpi) / 96.f, command.color, 6))
            return true;
    }
    return false;
}
inline bool centered_pages(const UiSearchScene& scene, const D3D11Renderer& renderer,
                           const surface_motion_probe_detail::Readback& rb, float scale,
                           const Theme& theme) noexcept {
    UiTextCommand commands[128]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    if (!ui_text_command_stream_initialize(&stream, {commands, 128}) ||
        !ui_search_scene_text_output(scene, theme, &output, &renderer.search_edit_geometry))
        return false;
    for (u32 i = 0; i < scene.page_count; ++i) {
        const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.pages[i]);
        if (!node)
            return false;
        const UiTextCommand* command = nullptr;
        for (u32 c = 0; c < stream.count; ++c)
            if (commands[c].owner.manager == &scene.widgets &&
                commands[c].owner.handle.slot == scene.pages[i].slot &&
                commands[c].owner.handle.generation == scene.pages[i].generation) {
                command = &commands[c];
                break;
            }
        if (!command || command->horizontal_alignment != UiTextAlignment::center ||
            command->vertical_alignment != UiTextAlignment::center)
            return false;
        const u8 ink[]{static_cast<u8>(command->color.blue * 255.f + .5f),
                       static_cast<u8>(command->color.green * 255.f + .5f),
                       static_cast<u8>(command->color.red * 255.f + .5f)};
        // Uncut labels may share a viewport batch; validate their visible glyphs.
        bool native_color = false;
        if (renderer.content_text)
            for (u32 b = 0; b < renderer.content_text->batch_count; ++b) {
                const auto& batch = renderer.content_text->batches[b];
                for (u32 q = 0; q < batch.quad_count; ++q) {
                    const auto& quad = renderer.content_text->quads[batch.first_quad + q];
                    const UiRect bounds{quad.left, quad.top, quad.right, quad.bottom};
                    const UiRect visible = ui_rect_intersection(bounds, batch.clip);
                    if (!ui_rect_has_area(ui_rect_intersection(visible, command->rectangle)))
                        continue;
                    const auto& color = quad.color;
                    if (!ui_rect_equal(ui_rect_intersection(visible, command->clip), visible) ||
                        color.red != command->color.red || color.green != command->color.green ||
                        color.blue != command->color.blue || color.alpha != command->color.alpha)
                        return false;
                    native_color = true;
                }
            }
        if (!native_color) {
            log_u64(LogLevel::error, "probe", "search_center_native_color_slot", i);
            return false;
        }
        const auto clip = raster_clip_rectangle(node->rectangle, scale, rb.width, rb.height);
        i32 left = clip.right, right = clip.left, top = clip.bottom, bottom = clip.top;
        u32 pixels = 0, pixels30 = 0, pixels45 = 0;
        for (i32 y = maximum(clip.top, 0); y < clip.bottom; ++y)
            for (i32 x = maximum(clip.left, 0); x < clip.right; ++x) {
                const auto* p = rb.images + (y * rb.width + x) * 4;
                const auto* underlay = rb.images + 2 * rb.bytes + (y * rb.width + x) * 4;
                const u32 distance = surface_motion_probe_detail::distance(p, ink);
                pixels30 += distance <= 30;
                pixels45 += distance <= 45;
                // Thin antialiased digits need not have six fully covered
                // foreground pixels. Compare against identical native paint
                // with only page glyphs omitted, including the gradient.
                if (surface_motion_probe_detail::distance(p, underlay) > 6) {
                    left = minimum(left, x);
                    right = maximum(right, x + 1);
                    top = minimum(top, y);
                    bottom = maximum(bottom, y + 1);
                    ++pixels;
                }
            }
        // Glyph bearings are asymmetric; the measured advance is centered,
        // and the actual native ink must stay within 2 DIPs of that center.
        const float delta = static_cast<float>(left + right - clip.left - clip.right);
        const float vertical_delta = static_cast<float>(top + bottom - clip.top - clip.bottom);
        if (pixels < 6 || delta < -4.f * scale || delta > 4.f * scale || vertical_delta < -6.f * scale ||
            vertical_delta > 6.f * scale) {
            log_u64(LogLevel::error, "probe", "search_center_page_slot", i);
            log_u64(LogLevel::error, "probe", "search_center_ink_pixels", pixels);
            log_u64(LogLevel::error, "probe", "search_center_ink_pixels30", pixels30);
            log_u64(LogLevel::error, "probe", "search_center_ink_pixels45", pixels45);
            for (u32 channel = 0; channel < 3; ++channel) {
                log_u64(LogLevel::error, "probe", "search_center_color_channel", channel);
                log_u64(LogLevel::error, "probe", "search_center_color", ink[channel]);
            }
            if (renderer.content_text)
                for (u32 b = 0; b < renderer.content_text->batch_count; ++b) {
                    const auto& batch = renderer.content_text->batches[b];
                    if (!ui_rect_equal(batch.clip, command->clip))
                        continue;
                    log_u64(LogLevel::error, "probe", "search_center_glyph_quads", batch.quad_count);
                    if (batch.quad_count) {
                        const auto& quad = renderer.content_text->quads[batch.first_quad];
                        log_u64(LogLevel::error, "probe", "search_center_quad_red",
                                static_cast<u32>(quad.color.red * 255.f + .5f));
                        log_u64(LogLevel::error, "probe", "search_center_quad_green",
                                static_cast<u32>(quad.color.green * 255.f + .5f));
                        log_u64(LogLevel::error, "probe", "search_center_quad_blue",
                                static_cast<u32>(quad.color.blue * 255.f + .5f));
                    }
                    break;
                }
            log_u64(LogLevel::error, "probe", "search_center_ink_left", static_cast<u32>(left));
            log_u64(LogLevel::error, "probe", "search_center_ink_right", static_cast<u32>(right));
            log_u64(LogLevel::error, "probe", "search_center_ink_top", static_cast<u32>(top));
            log_u64(LogLevel::error, "probe", "search_center_ink_bottom", static_cast<u32>(bottom));
            log_u64(LogLevel::error, "probe", "search_center_button_left", static_cast<u32>(clip.left));
            log_u64(LogLevel::error, "probe", "search_center_button_right", static_cast<u32>(clip.right));
            log_u64(LogLevel::error, "probe", "search_center_button_top", static_cast<u32>(clip.top));
            log_u64(LogLevel::error, "probe", "search_center_button_bottom", static_cast<u32>(clip.bottom));
            return false;
        }
    }
    return scene.page_count != 0;
}
} // namespace scoped_search_probe_detail

// Offline scene/renderer fixture. Exactly two optional CREATE_NEW BMP receipts;
// no controller, account, network, external art or persistent application owner.
// Avatars are tiny original premultiplied color tiles, never provider artwork.
inline bool scoped_search_ui_probe(D3D11Renderer& renderer) noexcept {
    using namespace scoped_search_probe_detail;
    u32 stage = 1;
    const auto fail = [&]() noexcept {
        log_u64(LogLevel::error, "probe", "scoped_search_failure_stage", stage);
        return false;
    };
    if (!renderer.ui_scene || !renderer.content_text || !renderer.dpi)
        return fail();
    UiSearchScene scene{};
    struct Restore {
        D3D11Renderer& renderer;
        UiSearchScene& scene;
        const UiSearchScene* search;
        const UiMediaPickerScene* gallery;
        const UiSettingsScene* settings;
        const PopupScene* popup;
        TextFormSurface* form;
        const UiWidgetManager* focus;
        ~Restore() noexcept {
            renderer.search_scene = search;
            renderer.media_picker_scene = gallery;
            renderer_bind_settings_scene(&renderer, settings);
            renderer_bind_popup_scene(&renderer, popup);
            renderer_bind_text_form(&renderer, form);
            renderer_bind_focus_owner(&renderer, focus);
            renderer.retained_presentation.valid = false;
            ui_search_scene_release(&scene);
        }
    } restore{renderer,
              scene,
              renderer.search_scene,
              renderer.media_picker_scene,
              renderer.settings_scene,
              renderer.popup_scene,
              renderer.text_form,
              renderer.focus_owner};
    renderer.media_picker_scene = nullptr;
    renderer_bind_settings_scene(&renderer, nullptr);
    renderer_bind_popup_scene(&renderer, nullptr);
    renderer_bind_text_form(&renderer, nullptr);
    renderer_bind_focus_owner(&renderer, nullptr);
    const auto* title =
        ui_scene_resolve(*renderer.ui_scene, ui_scene_find(*renderer.ui_scene, UiSemantic::title_bar));
    if (!title)
        return fail();
    auto viewport = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content);
    viewport.right = static_cast<float>(renderer.width) * 96.f / renderer.dpi;
    UiSearchSource source{nullptr, 6, 1, resolve};
    UiSearchContent content{};
    content.edit = {utf8_literal("release has:image"), 17, 17, true, false, false};
    content.heading = utf8_literal("Search Native Garden");
    content.placeholder = utf8_literal("Search Native Garden");
    content.filter_summary = utf8_literal("Filtered search");
    content.panel = content.previous = content.next = content.total_estimated = true;
    content.total = 67;
    content.offset = 25;
    content.page_size = 25;
    content.phase = UiSearchPhase::ready;
    content.revision = 1;
    surface_motion_probe_detail::Readback readback;
    if (!readback.initialize(renderer))
        return fail();
    auto preferences = client_preferences_defaults();
    preferences.theme = ClientTheme::custom_gradient;
    preferences.gradient.angle_degrees = 90;
    preferences.gradient.intensity = 60;
    const Theme theme = client_theme_projection(preferences);
    const Color avatar_colors[]{{48.f / 255, 184.f / 255, 128.f / 255, 1},
                                {164.f / 255, 64.f / 255, 208.f / 255, 1},
                                {216.f / 255, 152.f / 255, 48.f / 255, 1}};
    for (u32 author = 0; author < 3; ++author) {
        u8 pixels[16 * 16 * 4]{};
        for (u32 p = 0; p < 16 * 16; ++p) {
            if (p / 16 >= 4) {
                pixels[p * 4] = static_cast<u8>(avatar_colors[author].red * 255.f + .5f);
                pixels[p * 4 + 1] = static_cast<u8>(avatar_colors[author].green * 255.f + .5f);
                pixels[p * 4 + 2] = static_cast<u8>(avatar_colors[author].blue * 255.f + .5f);
                pixels[p * 4 + 3] = 255;
            }
        }
        const auto key = media_asset_user_avatar(
            0x771000 + author,
            user_avatar_variant(utf8_literal("0123456789abcdef0123456789abcdef")), false);
        if (!renderer_upload_media_rgba8(&renderer, key, 16, 16, 64, {pixels, sizeof(pixels)}).ok())
            return fail();
    }
    const auto render = [&](u32 image) noexcept {
        stage = 201 + image * 200;
        if (!ui_search_scene_update(&scene, &source, &content, viewport, title->rectangle))
            return false;
        renderer.search_scene = &scene;
        renderer.retained_presentation.valid = false;
        ++stage;
        if (!renderer_rebuild_content_text(&renderer, theme))
            return false;
        ++stage;
        if (!probe_present(&renderer, theme))
            return false;
        ++stage;
        if (!probe_present(&renderer, theme))
            return false;
        const float clear[]{theme.base_lowest.red, theme.base_lowest.green, theme.base_lowest.blue, 1};
        renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target, clear);
        ++stage;
        if (!d3d11_renderer_draw_stream(&renderer, theme, false, false, nullptr, 10000))
            return false;
        ++stage;
        return readback.read(renderer, image);
    };
    if (!render(0))
        return fail();
    stage = 207;
    if (scene.count < 3 || scene.page_count != 3) {
        log_u64(LogLevel::error, "probe", "search_visible_count", scene.count);
        log_u64(LogLevel::error, "probe", "search_page_count", scene.page_count);
        return fail();
    }
    u8 saved_page_bytes[5]{};
    CopyMemory(saved_page_bytes, scene.page_bytes, sizeof(saved_page_bytes));
    ZeroMemory(scene.page_bytes, sizeof(scene.page_bytes));
    ++scene.revision;
    const bool underlay_ready = render(2);
    CopyMemory(scene.page_bytes, saved_page_bytes, sizeof(saved_page_bytes));
    ++scene.revision;
    if (!underlay_ready || !render(0))
        return fail();
    const auto assert_ink = [&](u32 image, u32 numeric_stage, Utf8View value, bool wrap = false) noexcept {
        stage = numeric_stage;
        return text_ink(scene, renderer, readback, image, value, wrap, theme);
    };
    if (!assert_ink(0, 210, content.heading) || !assert_ink(0, 211, content.edit.text) ||
        !assert_ink(0, 212, utf8_literal("About 67 results")) ||
        !assert_ink(0, 213, utf8_literal("#release-planning")) ||
        !assert_ink(0, 214, utf8_literal("#native-ui")) || !assert_ink(0, 215, scene.visible[0].author) ||
        !assert_ink(0, 216, scene.visible[0].body, true) || !assert_ink(0, 217, utf8_literal("2")))
        return fail();
    stage = 218;
    if (!centered_pages(scene, renderer, readback, static_cast<float>(renderer.dpi) / 96.f, theme))
        return fail();
    for (u32 slot = 0; slot < minimum(scene.count, 3u); ++slot) {
        stage = 220 + slot * 4;
        SearchAvatarMedia media{};
        if (!search_avatar_media_resolve(scene, slot, &media))
            return fail();
        const auto r = media.rectangle;
        const float scale = static_cast<float>(renderer.dpi) / 96.f;
        const u32 x = static_cast<u32>((r.left + 16) * scale);
        const u32 y = static_cast<u32>((r.top + 3) * scale);
        const u32 under_y = static_cast<u32>((r.top - 4) * scale);
        ++stage;
        if (x >= readback.width || y >= readback.height || under_y >= readback.height)
            return fail();
        ++stage;
        if (!channel_overview_probe_detail::ink(readback, 0,
                                                {r.left + 12, r.top + 16, r.left + 20, r.top + 24}, scale,
                                                avatar_colors[slot % 3], 12)) {
            log_u64(LogLevel::error, "probe", "search_avatar_color_slot", slot);
            return fail();
        }
        ++stage;
        const auto* actual = readback.images + (y * readback.width + x) * 4;
        const auto* underlay = readback.images + (under_y * readback.width + x) * 4;
        if (surface_motion_probe_detail::distance(actual, underlay) > 8) {
            log_u64(LogLevel::error, "probe", "search_avatar_underlay_slot", slot);
            for (u32 channel = 0; channel < 3; ++channel) {
                log_u64(LogLevel::error, "probe", "search_avatar_underlay_channel", channel);
                log_u64(LogLevel::error, "probe", "search_avatar_actual", actual[channel]);
                log_u64(LogLevel::error, "probe", "search_avatar_underlay", underlay[channel]);
            }
            return fail();
        }
    }
    const auto retained = surface_motion_probe_detail::snapshot(renderer);
    const u32 source_revision = source.revision, widget_revision = scene.widgets.layout.revision;
    stage = 260;
    for (u32 frame = 0; frame < 16; ++frame)
        if (!probe_present(&renderer, theme) ||
            !surface_motion_probe_detail::same(retained, surface_motion_probe_detail::snapshot(renderer)) ||
            source.revision != source_revision || scene.widgets.layout.revision != widget_revision) {
            log_u64(LogLevel::error, "probe", "search_retained_frame", frame);
            return fail();
        }
    const auto card = scene.row_rectangles[0];
    stage = 261;
    if (!ui_background_active(theme) || !ui_rect_has_area(card))
        return fail();
    stage = 3;
    for (u32 i = 0; i < scene.page_count; ++i) {
        const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.pages[i]);
        if (!node)
            return fail();
        const auto target =
            ui_search_scene_hit_test(scene, (node->rectangle.left + node->rectangle.right) * .5f,
                                     (node->rectangle.top + node->rectangle.bottom) * .5f);
        if (target.action != UiSearchAction::page || target.page_offset != i * 25)
            return fail();
    }
    const auto first = scene.row_rectangles[0];
    const auto result = ui_search_scene_hit_test(scene, first.left + 60, first.top + 70);
    if (result.action != UiSearchAction::result || result.channel_id != 710 || result.message_id != 810)
        return fail();
    // Receipt creation follows assertions; a failure never produces a misleading PASS image.
    if (!readback.capture(0, L"search-scoped-results-native.bmp"))
        return fail();
    content.filters = true;
    ++content.revision;
    if (!render(1))
        return fail();
    if (!assert_ink(1, 410, content.edit.text) ||
        !assert_ink(1, 411, utf8_literal("From a specific user")) ||
        !assert_ink(1, 412, utf8_literal("from:")) ||
        !assert_ink(1, 413, utf8_literal("Sent in a channel")) ||
        !assert_ink(1, 414, utf8_literal("has:image")) || !assert_ink(1, 415, utf8_literal("2")))
        return fail();
    stage = 416;
    if (scene.chooser.top != scene.field.bottom + 8 || scene.chooser.right > viewport.right)
        return fail();
    const auto covered = ui_search_scene_hit_test(scene, first.left + 60, first.top + 70);
    if (covered.action == UiSearchAction::result ||
        !readback.capture(1, L"search-scoped-filters-native.bmp"))
        return fail();
    log_write(LogLevel::info, "probe", "scoped_search_native_passed");
    return true;
}
} // namespace maicord
