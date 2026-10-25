#include "tooltip_ui_probe.hpp"
#include "probe_present.hpp"
#include "maicord/core/log.hpp"
#include "maicord/render/renderer_presentation.hpp"
#include "maicord/ui/scene.hpp"
#include "../../src/render/d3d11_renderer_interaction_state.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {
namespace {
bool emoji_bounds_centered(UiRect ink, UiRect square) noexcept {
    const float dx = ink.left + ink.right - square.left - square.right;
    const float dy = ink.top + ink.bottom - square.top - square.bottom;
    return ui_rect_has_area(ink) && dx >= -.01f && dx <= .01f && dy >= -.01f && dy <= .01f &&
           ink.left >= square.left && ink.right <= square.right && ink.top >= square.top &&
           ink.bottom <= square.bottom;
}
bool native_emoji_centered(const D3D11Renderer& renderer, const UiTooltipScene& scene) noexcept {
    UiRect square{}, ink{};
    if (!ui_tooltip_emoji(scene, &square))
        return false;
    u32 count = 0;
    for (u32 b = renderer.form_text_first_batch; b < renderer.content_text->batch_count; ++b) {
        const auto& batch = renderer.content_text->batches[b];
        for (u32 i = 0; i < batch.quad_count; ++i) {
            const auto& q = renderer.content_text->quads[batch.first_quad + i];
            if (q.left >= square.right || q.right <= square.left || q.top >= scene.rectangle.bottom ||
                q.bottom <= scene.rectangle.top)
                continue;
            ink = count++ ? UiRect{minimum(ink.left, q.left), minimum(ink.top, q.top),
                                   maximum(ink.right, q.right), maximum(ink.bottom, q.bottom)}
                          : UiRect{q.left, q.top, q.right, q.bottom};
        }
    }
    return count && emoji_bounds_centered(ink, square);
}
} // namespace
bool tooltip_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!renderer.ui_scene || !renderer.dpi || !renderer.content_text)
        return false;
    const auto& source = renderer.ui_scene->widgets;
    UiWidgetHandle settings = ui_widget_invalid_handle();
    for (u32 i = 0; i < source.count; ++i)
        if (source.storage[i].id == static_cast<u32>(UiSemantic::settings) + 1)
            settings = {static_cast<u16>(i), source.generation};
    const auto* button = ui_widget_manager_layout_node(source, settings);
    if (!button || !ui_widget_manager_input_enabled(source, settings))
        return false;

    UiWidget root_storage[1]{};
    UiLayoutNode root_nodes[1]{};
    UiWidgetManager overlay{};
    UiTooltipScene scene{};
    UiTextMeasureRecord measure_records[4]{};
    UiTextMeasureCache measure{};
    UiPage pages[1]{};
    UiPageRegistry registry{};
    UiSettingsScene modal{};
    UiSettingsPageContent content{
        utf8_literal("Tooltip Modality"), utf8_literal("Opaque pages suppress underlying hints"), {}, 1};
    const auto* original_tooltip = renderer.tooltip_scene;
    const auto* original_settings = renderer.settings_scene;
    auto& focus_manager = const_cast<UiWidgetManager&>(source);
    const auto original_focus = focus_manager.focused;
    const auto* original_focus_owner = renderer.focus_owner;
    bool restored = false;
    struct Restore {
        D3D11Renderer& renderer;
        UiTooltipScene& scene;
        UiSettingsScene& modal;
        const UiTooltipScene* tooltip;
        const UiSettingsScene* settings;
        bool& restored;
        UiWidgetManager& focus_manager;
        UiWidgetHandle original_focus;
        const UiWidgetManager* original_focus_owner;
        ~Restore() noexcept {
            ui_tooltip_clear(&scene);
            renderer.tooltip_scene = tooltip;
            renderer.settings_scene = settings;
            focus_manager.focused = original_focus;
            renderer_bind_focus_owner(&renderer, original_focus_owner);
            renderer.retained_presentation.valid = false;
            restored = renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                       probe_present(&renderer, kDefaultTheme);
            ui_settings_scene_release(&modal);
        }
    };
    bool valid = false;
    {
        Restore restore{renderer, scene,         modal,          original_tooltip,    original_settings,
                        restored, focus_manager, original_focus, original_focus_owner};
        const UiRect viewport{0, 0, static_cast<float>(renderer.width) * 96.f / renderer.dpi,
                              static_cast<float>(renderer.height) * 96.f / renderer.dpi};
        auto root_style = ui_layout_style();
        root_style.width = ui_layout_fill();
        root_style.height = ui_layout_fill();
        if (!ui_widget_manager_initialize(&overlay, {root_storage, 1}, {root_nodes, 1}))
            return false;
        const auto root = ui_widget_manager_add(
            &overlay, ui_widget_invalid_handle(),
            {1, UiWidgetKind::container, ui_widget_none, {utf8_literal("Tooltip Overlay"), root_style}});
        if (!root.ok() || !ui_widget_manager_layout_rectangle(&overlay, viewport).ok() ||
            !ui_tooltip_bind_text_owner(&scene, {&overlay, root.handle}) ||
            !ui_text_measure_cache_initialize(&measure, {measure_records, 4}, renderer_text_measure_backend,
                                              &renderer))
            return false;
        renderer.tooltip_scene = &scene;
        UiTooltipTarget target{{{&source, settings}, 1, 1, 1, 1},
                               ui_rect_intersection(button->rectangle, button->clip_rectangle),
                               utf8_literal("User settings")};
        const auto baseline = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
        valid = ui_tooltip_offer(&scene, target, viewport, UiTooltipTrigger::pointer, 100) &&
                ui_tooltip_next_wake(scene) == 600 && !ui_tooltip_tick(&scene, 599, &measure) &&
                renderer_presentation_stamp_equal(
                    baseline, renderer_capture_presentation_stamp(renderer, kDefaultTheme));
        const auto present = [&]() noexcept {
            renderer.retained_presentation.valid = false;
            return renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                   probe_present(&renderer, kDefaultTheme) &&
                   d3d11_renderer_interaction_text_state_current(renderer) &&
                   renderer.form_text_first_batch < renderer.content_text->batch_count &&
                   renderer.form_paint_first_batch < renderer.paint_stream.batch_count;
        };
        valid = valid && ui_tooltip_tick(&scene, 600, &measure) && ui_tooltip_visible(scene) && present() &&
                (!capture || capture(L"tooltip-pointer-delayed.bmp"));
        if (!valid)
            log_u64(LogLevel::error, "probe", "tooltip_native_failure_stage", 1);
        ui_tooltip_clear(&scene);
        ++target.key.action;
        const bool pointer_passed = valid;
        renderer_bind_focus_owner(&renderer, &source);
        valid = valid && ui_widget_manager_focus(&focus_manager, settings) &&
                ui_tooltip_offer(&scene, target, viewport, UiTooltipTrigger::keyboard, 1000) &&
                ui_tooltip_next_wake(scene) == 1350 && ui_tooltip_tick(&scene, 1350, &measure) &&
                present() && (!capture || capture(L"tooltip-keyboard-focus.bmp"));
        if (pointer_passed && !valid)
            log_u64(LogLevel::error, "probe", "tooltip_native_failure_stage", 2);
        ui_tooltip_clear(&scene);
        ++target.key.action;
        target.label =
            utf8_literal("Keyboard focus and pointer hover share this bounded native tooltip presentation");
        target.placement = UiTooltipPlacement::right;
        UiTooltipStyle narrow{};
        narrow.maximum_width = 176;
        narrow.maximum_height = 140;
        const bool keyboard_passed = valid;
        valid = valid &&
                ui_tooltip_offer(&scene, target, viewport, UiTooltipTrigger::pointer, 2000, narrow) &&
                ui_tooltip_tick(&scene, 2500, &measure) && scene.text_size.height > 20 &&
                scene.rectangle.left >= viewport.left + narrow.margin &&
                scene.rectangle.right <= viewport.right - narrow.margin && present() &&
                (!capture || capture(L"tooltip-narrow-wrap.bmp"));
        if (keyboard_passed && !valid)
            log_u64(LogLevel::error, "probe", "tooltip_native_failure_stage", 3);
        // The broken heart regression uses real native font ink, not the
        // centered text command's line box. Include different color-layer
        // bounds and a joined emoji, then rebuild each unchanged presentation.
        const Utf8View emoji[]{utf8_literal("\xF0\x9F\x92\x94"), utf8_literal("\xF0\x9F\x94\xA5"),
                               utf8_literal("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB")};
        target.label = utf8_literal(":broken_heart: reacted by Ada");
        target.placement = UiTooltipPlacement::above;
        const bool wrapping_passed = valid;
        for (u32 i = 0; i < 3 && valid; ++i) {
            ui_tooltip_clear(&scene);
            ++target.key.action;
            target.emoji = {{}, emoji[i]};
            valid = ui_tooltip_offer(&scene, target, viewport, UiTooltipTrigger::pointer, 3000) &&
                    ui_tooltip_tick(&scene, 3500, &measure) && present() &&
                    native_emoji_centered(renderer, scene) && present() &&
                    native_emoji_centered(renderer, scene);
        }
        // Custom images use the same centered square while preserving their
        // aspect ratio. Original synthetic pixels avoid external asset data.
        u8 pixels[16 * 8 * 4];
        for (u32 i = 0; i < sizeof(pixels); ++i)
            pixels[i] = 255;
        for (u32 i = 0; i < 2 && valid; ++i) {
            const u16 width = i ? 8 : 16, height = i ? 16 : 8;
            const auto asset = media_asset_custom_emoji(0x746f6f6c + i, false);
            valid = renderer_upload_media_straight_rgba8(&renderer, asset, width, height, width * 4,
                                                         {pixels, sizeof(pixels)})
                        .ok();
            ui_tooltip_clear(&scene);
            ++target.key.action;
            target.emoji = {asset, {}};
            valid = valid && ui_tooltip_offer(&scene, target, viewport, UiTooltipTrigger::pointer, 3000) &&
                    ui_tooltip_tick(&scene, 3500, &measure) && present();
            UiRect square{};
            const auto allocation = media_atlas_peek(renderer.media_atlas, asset);
            u32 matches = 0;
            valid = valid && allocation.ok() && ui_tooltip_emoji(scene, &square);
            for (u32 q = 0; q < renderer.media_quad_count && valid; ++q) {
                const auto handle = renderer.media_draw.handle_data[q];
                if (renderer.media_draw.domain_data[q] || handle.slot != allocation.handle.slot ||
                    handle.generation != allocation.handle.generation)
                    continue;
                const auto& quad = renderer.media_draw.quad_data[q];
                const UiRect ink{quad.left, quad.top, quad.right, quad.bottom};
                valid = emoji_bounds_centered(ink, square) &&
                        ui_rect_width(ink) == scene.style.emoji_size * (i ? .5f : 1.f) &&
                        ui_rect_height(ink) == scene.style.emoji_size * (i ? 1.f : .5f);
                ++matches;
            }
            valid = valid && matches == 1;
        }
        if (wrapping_passed && !valid)
            log_u64(LogLevel::error, "probe", "tooltip_native_failure_stage", 5);
        const bool presentation_passed = valid;
        valid = valid && ui_page_registry_initialize(&registry, {pages, 1}) &&
                ui_page_registry_register(&registry, {{1, 1}, utf8_literal("Tooltip Modality"), &content})
                    .ok() &&
                ui_settings_scene_update(&modal, &registry, viewport);
        renderer.settings_scene = &modal;
        valid = valid && !d3d11_renderer_tooltip_visible(renderer) &&
                !d3d11_renderer_tooltip_revision(renderer) &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                probe_present(&renderer, kDefaultTheme);
        const auto modal_stamp = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
        ++scene.revision;
        valid = valid && renderer_presentation_stamp_equal(
                             modal_stamp, renderer_capture_presentation_stamp(renderer, kDefaultTheme));
        if (presentation_passed && !valid)
            log_u64(LogLevel::error, "probe", "tooltip_native_failure_stage", 4);
    }
    log_u64(LogLevel::info, "probe", "tooltip_scene_bytes", sizeof(UiTooltipScene));
    log_u64(LogLevel::info, "probe", "tooltip_renderer_bytes", sizeof(D3D11Renderer));
    log_write(valid && restored ? LogLevel::info : LogLevel::error, "probe",
              valid && restored ? "tooltip_native_passed" : "tooltip_native_failed");
    return valid && restored;
}
} // namespace maicord
