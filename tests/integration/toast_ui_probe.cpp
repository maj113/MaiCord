#include "toast_ui_probe.hpp"
#include "probe_present.hpp"
#include "maicord/core/log.hpp"
#include "maicord/render/layer_presentation.hpp"
#include "maicord/render/renderer_presentation.hpp"
#include "../../src/render/d3d11_renderer_interaction_state.hpp"

namespace maicord {
bool toast_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!renderer.ui_scene || !renderer.dpi || !renderer.content_text)
        return false;
    UiWidget root_storage[1]{};
    UiLayoutNode root_nodes[1]{};
    UiWidgetManager overlay{};
    UiWidgetReference owner{};
    UiToastScene scene{};
    UiTextMeasureRecord measure_records[4]{};
    UiTextMeasureCache measure{};
    UiPage pages[1]{};
    UiPageRegistry registry{};
    UiSettingsScene modal{};
    UiSettingsPageContent content{
        utf8_literal("Toast Over Settings"), utf8_literal("Toasts draw above every layer"), {}, 1};
    const auto* original_settings = renderer.settings_scene;
    bool valid = false, restored = false;
    {
        struct Restore {
            D3D11Renderer& renderer;
            UiSettingsScene& modal;
            const UiSettingsScene* settings;
            bool& restored;
            ~Restore() noexcept {
                renderer.toast_scene = nullptr;
                renderer.settings_scene = settings;
                renderer.retained_presentation.valid = false;
                restored = renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                           probe_present(&renderer, kDefaultTheme) &&
                           !d3d11_renderer_toast_visible(renderer);
                ui_settings_scene_release(&modal);
            }
        } restore{renderer, modal, original_settings, restored};
        const UiRect viewport{0, 0, static_cast<float>(renderer.width) * 96.f / renderer.dpi,
                              static_cast<float>(renderer.height) * 96.f / renderer.dpi};
        if (!ui_widget_manager_initialize_fill_root(
                &overlay, {root_storage, 1}, {root_nodes, 1}, utf8_literal("Toast"),
                ui_layout_node_visible | ui_layout_node_allow_overlap | ui_layout_node_clip_children,
                &owner) ||
            !ui_widget_manager_layout_rectangle(&overlay, viewport).ok() ||
            !ui_toast_bind_text_owner(&scene, owner) ||
            !ui_text_measure_cache_initialize(&measure, {measure_records, 4}, renderer_text_measure_backend,
                                              &renderer))
            return false;
        const UiToastEnvironment environment{&kDefaultTheme, &measure, viewport, true, false};
        const auto present = [&]() noexcept {
            renderer_set_toast_opacity(&renderer, scene.progress);
            renderer.retained_presentation.valid = false;
            return renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                   probe_present(&renderer, kDefaultTheme) && d3d11_renderer_toast_visible(renderer) &&
                   d3d11_renderer_interaction_text_state_current(renderer) &&
                   renderer.toast_text_first_batch < renderer.content_text->batch_count &&
                   renderer.toast_paint_first_batch < renderer.paint_stream.batch_count;
        };
        renderer.toast_scene = &scene;
        const auto baseline = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
        // A queued toast adds nothing to the frame until its first update.
        valid = ui_toast_push(&scene, UiToastKind::success, utf8_literal("Link Copied!")) &&
                renderer_presentation_stamp_equal(
                    baseline, renderer_capture_presentation_stamp(renderer, kDefaultTheme)) &&
                ui_toast_update(&scene, 1'000, environment) &&
                ui_toast_update(&scene, 1'300, environment) && scene.progress == 1 && present() &&
                (!capture || capture(L"toast-link-copied.bmp"));
        if (!valid)
            log_u64(LogLevel::error, "probe", "toast_native_failure_stage", 1);
        // A failure toast halfway into its entrance.
        const bool success_passed = valid;
        ui_toast_clear(&scene);
        valid = valid &&
                ui_toast_push(&scene, UiToastKind::failure, utf8_literal("Unable to save image")) &&
                ui_toast_update(&scene, 2'000, environment) &&
                ui_toast_update(&scene, 2'150, environment) && scene.progress > .4f &&
                scene.progress < .6f && present() && (!capture || capture(L"toast-failure-entering.bmp"));
        if (success_passed && !valid)
            log_u64(LogLevel::error, "probe", "toast_native_failure_stage", 2);
        // Over an open settings page, where tooltips stay hidden.
        const bool failure_passed = valid;
        ui_toast_clear(&scene);
        valid =
            valid && ui_page_registry_initialize(&registry, {pages, 1}) &&
            ui_page_registry_register(&registry, {{1, 1}, utf8_literal("Toast Over Settings"), &content})
                .ok() &&
            ui_settings_scene_update(&modal, &registry, viewport);
        renderer.settings_scene = &modal;
        valid = valid && ui_toast_push(&scene, UiToastKind::success, utf8_literal("Invite link copied")) &&
                ui_toast_update(&scene, 3'000, environment) &&
                ui_toast_update(&scene, 3'300, environment) && present() &&
                (!capture || capture(L"toast-over-settings.bmp"));
        if (failure_passed && !valid)
            log_u64(LogLevel::error, "probe", "toast_native_failure_stage", 3);
        // Once it has left, the toast adds nothing and stops waking.
        const bool settings_passed = valid;
        ui_toast_update(&scene, 6'300, environment);
        ui_toast_update(&scene, 6'600, environment);
        valid = valid && !scene.count && !d3d11_renderer_toast_visible(renderer) &&
                !d3d11_renderer_toast_revision(renderer) && !ui_toast_next_wake(scene, 6'600, true);
        if (settings_passed && !valid)
            log_u64(LogLevel::error, "probe", "toast_native_failure_stage", 4);
    }
    log_u64(LogLevel::info, "probe", "toast_scene_bytes", sizeof(UiToastScene));
    log_write(valid && restored ? LogLevel::info : LogLevel::error, "probe",
              valid && restored ? "toast_native_passed" : "toast_native_failed");
    return valid && restored;
}
} // namespace maicord
