#include "tooltip_ui_probe.hpp"
#include "probe_present.hpp"
#include "maicord/core/log.hpp"
#include "maicord/render/renderer_presentation.hpp"
#include "../../src/render/d3d11_renderer_interaction_state.hpp"

namespace maicord {
bool tooltip_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!renderer.ui_scene || !renderer.dpi || !renderer.content_text)
        return false;
    const auto& source = renderer.ui_scene->widgets;
    UiWidgetHandle settings = ui_widget_invalid_handle();
    for (u32 i = 0; i < source.count; ++i)
        if (source.storage[i].id == static_cast<u32>(UiSemantic::settings) + 1)
            settings = {static_cast<u16>(i), source.storage[i].generation};
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
