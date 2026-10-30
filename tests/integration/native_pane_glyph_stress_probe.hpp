#pragma once
#include "channel_overview_probe.hpp"
#include "maicord/ui/media_picker_presentation.hpp"
#include "maicord/ui/search_scene.hpp"
#include "../../src/render/d3d11_renderer_interaction_state.hpp"

namespace maicord {
namespace pane_glyph_stress_detail {
struct Source {
    u32 calls;
};
inline bool gallery_item(void* opaque, u32 index, UiMediaPickerItem* out) noexcept {
    ++static_cast<Source*>(opaque)->calls;
    if (!out || index >= 30)
        return false;
    *out = {index + 1ull,
            0,
            utf8_literal("Native garden"),
            utf8_literal("Original offline category"),
            0,
            96,
            64,
            UiMediaPickerItemKind::category,
            true,
            0};
    return true;
}
inline bool search_row(void* opaque, u32 index, UiSearchRow* out) noexcept {
    ++static_cast<Source*>(opaque)->calls;
    if (!out || index >= 30)
        return false;
    *out = {1,
            index + 100ull,
            utf8_literal("Native author"),
            utf8_literal("#native-garden"),
            utf8_literal("Original controlled search result. No account or provider data is used."),
            utf8_literal("Offline fixture"),
            true,
            true};
    return true;
}
inline bool category_label_clear(const UiMediaPickerScene& scene, const D3D11Renderer& renderer) noexcept {
    for (u32 i = 0; i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        if (!cell.item.enabled || cell.item.kind != UiMediaPickerItemKind::category ||
            !cell.item.label.bytes || cell.item.asset_id)
            continue;
        const float middle = (cell.rectangle.top + cell.rectangle.bottom) * .5f;
        const auto label =
            ui_rect_intersection({cell.rectangle.left + scene.style.metrics.gap, middle - 12,
                                  cell.rectangle.right - scene.style.metrics.gap, middle + 12},
                                 cell.clip);
        if (!ui_rect_has_area(label))
            continue;
        for (u32 q = 0; q < renderer.paint_stream.count; ++q) {
            const auto& quad = renderer.paint_stream.quads[q];
            const UiRect bounds{quad.left, quad.top, quad.right, quad.bottom};
            // Whole-card/panel fills and outlines belong behind the label.
            // Smaller foreground symbols must leave its actual line clear.
            // Fills covering the cell's visible part (it may run past the panel's edge).
            const bool background =
                bounds.left <= cell.clip.left && bounds.top <= cell.clip.top &&
                bounds.right >= cell.clip.right && bounds.bottom >= cell.clip.bottom;
            // Surfaces behind the picker (panes wider than its panel) are not its ink.
            const bool behind = bounds.left < scene.panel.left || bounds.right > scene.panel.right ||
                                bounds.top < scene.panel.top || bounds.bottom > scene.panel.bottom;
            if (!background && !behind && ui_rect_has_area(ui_rect_intersection(bounds, label)))
                return false;
        }
    }
    return true;
}
inline bool query_geometry(const UiMediaPickerScene& scene, const D3D11Renderer& renderer,
                           UiRect* rectangle) noexcept {
    UiTextCommand commands[64]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    if (!ui_text_command_stream_initialize(&stream, {commands, 64}) ||
        !ui_media_picker_scene_text_output(scene, kDefaultTheme, &output,
                                           &renderer.media_picker_edit_geometry))
        return false;
    for (u32 i = 0; i < stream.count; ++i)
        if (utf8_equal(commands[i].text, scene.content->edit.text)) {
            *rectangle = ui_rect_intersection(commands[i].rectangle, commands[i].clip);
            return ui_rect_has_area(*rectangle);
        }
    return false;
}
inline bool query_geometry(const UiSearchScene& scene, const D3D11Renderer& renderer,
                           UiRect* rectangle) noexcept {
    UiTextCommand commands[96]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    if (!ui_text_command_stream_initialize(&stream, {commands, 96}) ||
        !ui_search_scene_text_output(scene, kDefaultTheme, &output, &renderer.search_edit_geometry))
        return false;
    for (u32 i = 0; i < stream.count; ++i)
        if (utf8_equal(commands[i].text, scene.content->edit.text)) {
            *rectangle = ui_rect_intersection(commands[i].rectangle, commands[i].clip);
            return ui_rect_has_area(*rectangle);
        }
    return false;
}
} // namespace pane_glyph_stress_detail

inline bool native_pane_glyph_stress_probe(D3D11Renderer& renderer) noexcept {
    using namespace pane_glyph_stress_detail;
    using channel_overview_probe_detail::ink;
    u32 stage = 1;
    const auto fail = [&]() noexcept {
        log_u64(LogLevel::error, "probe", "pane_glyph_stress_failure_stage", stage);
        return false;
    };
    if (!renderer.ui_scene || !renderer.message_composer_scene || !renderer.content_text)
        return fail();
    UiMediaPickerScene gallery{};
    UiSearchScene search{};
    UiSettingsScene settings{};
    UiPageRegistry registry{};
    UiPage page{};
    const auto* previous_gallery = renderer.media_picker_scene;
    const auto* previous_search = renderer.search_scene;
    const auto* previous_settings = renderer.settings_scene;
    const auto* previous_popup = renderer.popup_scene;
    auto* previous_form = renderer.text_form;
    const auto* previous_focus = renderer.focus_owner;
    const auto previous_opacity = renderer.layer_opacity;
    struct Restore {
        D3D11Renderer& renderer;
        UiMediaPickerScene& gallery;
        UiSearchScene& search;
        UiSettingsScene& settings;
        const UiMediaPickerScene* old_gallery;
        const UiSearchScene* old_search;
        const UiSettingsScene* old_settings;
        const PopupScene* old_popup;
        TextFormSurface* old_form;
        const UiWidgetManager* old_focus;
        RendererLayerOpacity opacity;
        ~Restore() noexcept {
            renderer.media_picker_scene = old_gallery;
            renderer.search_scene = old_search;
            renderer_bind_settings_scene(&renderer, old_settings);
            renderer_bind_popup_scene(&renderer, old_popup);
            renderer_bind_text_form(&renderer, old_form);
            renderer_bind_focus_owner(&renderer, old_focus);
            renderer.layer_opacity = opacity;
            renderer.retained_presentation.valid = false;
            ui_media_picker_scene_release(&gallery);
            ui_search_scene_release(&search);
            ui_settings_scene_release(&settings);
        }
    } restore{renderer,        gallery,           search,         settings,      previous_gallery,
              previous_search, previous_settings, previous_popup, previous_form, previous_focus,
              previous_opacity};
    renderer_bind_popup_scene(&renderer, nullptr);
    renderer_bind_text_form(&renderer, nullptr);
    renderer_bind_settings_scene(&renderer, nullptr);
    renderer.layer_opacity = {1, 1, 1};
    const float scale = static_cast<float>(renderer.dpi) / 96.f;
    const UiRect viewport{0, 0, renderer.width / scale, renderer.height / scale};
    auto content_viewport = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::content);
    content_viewport.right = viewport.right;
    const auto* anchor =
        message_composer_scene_layout_node(*renderer.message_composer_scene, MessageComposerControl::gif);
    const auto* title =
        ui_scene_node(*renderer.ui_scene, UiSemantic::title_bar);
    if (!anchor || !title)
        return fail();
    Source gallery_source{}, search_source{};
    UiMediaPickerSource source{&gallery_source, 0x71, 1, 30, 1, {}, gallery_item};
    UiMediaPickerContent gallery_content{};
    gallery_content.edit = {utf8_literal("garden"), 6, 6, true, true, false};
    gallery_content.placeholder = utf8_literal("Search original fixtures");
    gallery_content.status = utf8_literal("Original controlled categories");
    gallery_content.account_scope = source.account_scope;
    gallery_content.query_generation = source.query_generation;
    gallery_content.revision = 1;
    gallery_content.kind = UiMediaPickerKind::gif;
    gallery_content.phase = UiMediaPickerPhase::ready;
    gallery_content.available_tabs = 7;
    UiSearchSource search_bank{&search_source, 30, 1, search_row};
    UiSearchContent search_content{};
    search_content.edit = {utf8_literal("native garden"), 13, 13, true, true, false};
    search_content.status = utf8_literal("Original offline results");
    search_content.total = 30;
    search_content.revision = 1;
    search_content.phase = UiSearchPhase::ready;
    search_content.panel = true;
    UiSettingsControl control{1,
                              utf8_literal("Retain native labels"),
                              utf8_literal("Original settings cover between pane publications."),
                              UiSettingsControlKind::toggle,
                              true,
                              true};
    UiSettingsPageContent settings_content{utf8_literal("Native appearance"),
                                           utf8_literal("Original offline cover fixture"),
                                           {&control, 1},
                                           1};
    if (!ui_page_registry_initialize(&registry, {&page, 1}) ||
        !ui_page_registry_register(&registry, {{0x72, 1}, utf8_literal("Appearance"), &settings_content})
             .ok())
        return fail();
    surface_motion_probe_detail::Readback readback;
    if (!readback.initialize(renderer) || renderer_refresh_interaction_text(nullptr, kDefaultTheme))
        return fail();
    const auto warm = [&]() noexcept {
        return renderer_refresh_interaction_text(&renderer, kDefaultTheme) &&
               d3d11_renderer_interaction_text_state_current(renderer) &&
               probe_present(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
    };
    const auto retained_sample = [&](u32 index) noexcept {
        const auto before = surface_motion_probe_detail::snapshot(renderer);
        const u32 gallery_calls = gallery_source.calls, search_calls = search_source.calls;
        const u32 media_edit_builds = renderer.media_picker_layout.builds;
        const u32 search_edit_builds = renderer.search_layout.builds;
        return renderer_refresh_interaction_text(&renderer, kDefaultTheme) &&
               surface_motion_probe_detail::draw_fixed(renderer) && readback.read(renderer, index) &&
               surface_motion_probe_detail::same(before, surface_motion_probe_detail::snapshot(renderer)) &&
               media_edit_builds == renderer.media_picker_layout.builds &&
               search_edit_builds == renderer.search_layout.builds &&
               gallery_calls == gallery_source.calls && search_calls == search_source.calls;
    };
    // Reproduce the host's original omission once. Focus publication is current
    // scene data but its old retained glyph revision must still be suppressed.
    stage = 2;
    renderer.search_scene = nullptr;
    if (!ui_media_picker_scene_update(&gallery, &source, &gallery_content, content_viewport,
                                      anchor->rectangle))
        return fail();
    renderer.media_picker_scene = &gallery;
    renderer_bind_focus_owner(&renderer, &gallery.widgets);
    if (!warm() || !retained_sample(0))
        return fail();
    UiRect query{};
    if (!query_geometry(gallery, renderer, &query) ||
        !ink(readback, 0, query, scale, kDefaultTheme.text_primary))
        return fail();
    gallery_content.edit.focused = false;
    ++gallery_content.revision;
    if (!ui_media_picker_scene_update(&gallery, &source, &gallery_content, content_viewport,
                                      anchor->rectangle) ||
        d3d11_renderer_interaction_text_state_current(renderer))
        return fail();
    const u32 suppressions = renderer.stale_interaction_text_suppressions;
    if (!surface_motion_probe_detail::draw_fixed(renderer) || !readback.read(renderer, 1) ||
        renderer.stale_interaction_text_suppressions != suppressions + 1 ||
        ink(readback, 1, query, scale, kDefaultTheme.text_primary))
        return fail();
    stage = 3;
    for (u32 cycle = 0; cycle < 6; ++cycle) {
        gallery_content.edit.focused = (cycle & 1) != 0;
        gallery_content.edit.anchor = gallery_content.edit.focused ? 0 : 6;
        ++gallery_content.revision;
        if (!ui_media_picker_scene_update(&gallery, &source, &gallery_content, content_viewport,
                                          anchor->rectangle))
            return fail();
        if (cycle == 2)
            d3d11_glyph_text_reset_atlas(&renderer.glyph_text);
        if (cycle & 1)
            ui_media_picker_scene_scroll(&gallery, 88);
        else
            ui_media_picker_scene_scroll(&gallery, -88);
        if (!warm() || !retained_sample(0) || !category_label_clear(gallery, renderer) ||
            !query_geometry(gallery, renderer, &query) ||
            !ink(readback, 0, query, scale, kDefaultTheme.text_primary))
            return fail();
        // Settings closes and reopens while the gallery's immutable source
        // remains borrowed. The adapter must reconstruct the correct tails.
        if (!ui_settings_scene_update(&settings, &registry, viewport))
            return fail();
        renderer_bind_settings_scene(&renderer, &settings);
        renderer_bind_focus_owner(&renderer, &settings.widgets);
        if (!warm())
            return fail();
        renderer_bind_settings_scene(&renderer, nullptr);
        ui_settings_scene_close(&settings);
        renderer_bind_focus_owner(&renderer, &gallery.widgets);
        if (!warm() || !retained_sample(0) || !category_label_clear(gallery, renderer) ||
            !query_geometry(gallery, renderer, &query) ||
            !ink(readback, 0, query, scale, kDefaultTheme.text_primary))
            return fail();
    }
    stage = 4;
    const auto packets = surface_motion_probe_detail::snapshot(renderer);
    if (!renderer_set_layer_opacity(&renderer, RendererLayerSurface::gallery, .5f) ||
        !surface_motion_probe_detail::draw_fixed(renderer) ||
        !renderer_set_layer_opacity(&renderer, RendererLayerSurface::gallery, 1) || !retained_sample(0) ||
        !ink(readback, 0, query, scale, kDefaultTheme.text_primary) ||
        !surface_motion_probe_detail::same(packets, surface_motion_probe_detail::snapshot(renderer)) ||
        !readback.capture(0, L"pane-glyph-stress-gallery.bmp"))
        return fail();
    stage = 5;
    renderer.media_picker_scene = nullptr;
    ui_media_picker_scene_release(&gallery);
    if (!ui_search_scene_update(&search, &search_bank, &search_content, content_viewport, title->rectangle))
        return fail();
    renderer.search_scene = &search;
    renderer_bind_focus_owner(&renderer, &search.widgets);
    if (!warm() || !retained_sample(0) || !query_geometry(search, renderer, &query) ||
        !ink(readback, 0, query, scale, kDefaultTheme.text_primary))
        return fail();
    search_content.edit.focused = false;
    ++search_content.revision;
    if (!ui_search_scene_update(&search, &search_bank, &search_content, content_viewport,
                                title->rectangle) ||
        d3d11_renderer_interaction_text_state_current(renderer))
        return fail();
    const u32 search_suppressions = renderer.stale_interaction_text_suppressions;
    if (!surface_motion_probe_detail::draw_fixed(renderer) || !readback.read(renderer, 1) ||
        renderer.stale_interaction_text_suppressions != search_suppressions + 1 ||
        ink(readback, 1, query, scale, kDefaultTheme.text_primary))
        return fail();
    stage = 6;
    for (u32 cycle = 0; cycle < 6; ++cycle) {
        search_content.edit.focused = (cycle & 1) != 0;
        search_content.edit.anchor = search_content.edit.focused ? 0 : 13;
        ++search_content.revision;
        if (!ui_search_scene_update(&search, &search_bank, &search_content, content_viewport,
                                    title->rectangle) ||
            !ui_search_scene_scroll(&search, cycle & 1 ? 112.f : 0.f) || !warm() || !retained_sample(0) ||
            !query_geometry(search, renderer, &query) ||
            !ink(readback, 0, query, scale, kDefaultTheme.text_primary))
            return fail();
        if (!ui_settings_scene_update(&settings, &registry, viewport))
            return fail();
        renderer_bind_settings_scene(&renderer, &settings);
        renderer_bind_focus_owner(&renderer, &settings.widgets);
        if (!warm())
            return fail();
        renderer_bind_settings_scene(&renderer, nullptr);
        ui_settings_scene_close(&settings);
        renderer_bind_focus_owner(&renderer, &search.widgets);
        if (!warm() || !retained_sample(0) || !query_geometry(search, renderer, &query) ||
            !ink(readback, 0, query, scale, kDefaultTheme.text_primary))
            return fail();
    }
    if (!readback.capture(0, L"pane-glyph-stress-search.bmp"))
        return fail();
    log_u64(LogLevel::info, "probe", "pane_glyph_stress_gallery_cycles", 6);
    log_u64(LogLevel::info, "probe", "pane_glyph_stress_search_cycles", 6);
    log_write(LogLevel::info, "probe",
              "pane_glyph_stress_stale_guard_repair_retention_native_pixels_passed");
    return true;
}
} // namespace maicord
