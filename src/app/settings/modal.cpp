#include "../shell/state.hpp"
#include "../shell/window_state.hpp"
#include "../account/account_presentation.hpp"
#include "../chat/composer/attachments.hpp"
#include "../chat/composer/text_input.hpp"
#include "../media/picker.hpp"
#include "../chat/messages/interaction.hpp"
#include "../account/profile.hpp"
#include "settings.hpp"
#include "modal.hpp"

namespace maicord::app_detail {
namespace {
// The dialog playing its exit (the renderer shows one settings scene at a time) and its progress.
SettingsModal* g_exiting;
UiMotionTransition g_exit_motion;
u64 g_exit_generation;
constexpr u64 kExitMotionOwner = 0x534d'4f44'4558'4954ull; // "SMODEXIT"
__forceinline bool exit_allowed(const SettingsModal& modal) noexcept {
    return g_app.window && !application_reduced_motion() && !g_app.renderer.live_resize &&
           application_window_shown() && g_app.renderer.settings_scene == &modal.scene && modal.scene.active &&
           modal.scene.presentation != UiSettingsPresentation::layer;
}
} // namespace

void settings_modal_yield() noexcept {
    application_settings_close(false);
    application_media_picker_close();
    application_profile_close();
}

void settings_modal_take_input() noexcept {
    application_composer_text_input_suspend();
    dismiss_popup();
    ui_scroll_host_cancel(&g_app.scroll_host);
    g_app.message_composer.focused = false;
}

bool settings_dialog_begin(SettingsDialog& dialog, UiPageKey page, Utf8View title) noexcept {
    settings_modal_finish_exit();
    settings_layer_reset(&dialog.layer);
    if (!ui_page_registry_initialize(&dialog.registry, {&dialog.page, 1}))
        return false;
    dialog.content = {title, {}, {}, 1, kUiSettingsPageFormat, &kUiDefaultSettingsStyle, nullptr};
    if (!ui_page_registry_register(&dialog.registry, {page, dialog.content.title, &dialog.content}).ok())
        return false;
    settings_modal_take_input();
    ui_settings_scene_set_presentation(&dialog.scene, UiSettingsPresentation::dialog);
    return true;
}

void settings_modal_bind(SettingsModal& modal) noexcept {
    settings_modal_finish_exit();
    renderer_bind_settings_scene(&g_app.renderer, &modal.scene);
    settings_layer_begin_entrance(&modal.layer);
    g_app.context_focus = &modal.scene.widgets;
}

bool settings_dialog_show(SettingsDialog& dialog, UiPageKey page, u64 focus, bool focus_layer,
                          bool (*project)() noexcept) noexcept {
    dialog.scene.focus = {UiSettingsTargetKind::control, page, focus};
    // project() requires an active scene; the first update activates it.
    if (!ui_settings_scene_update(&dialog.scene, &dialog.registry, application_viewport_dips()) || !project())
        return false;
    settings_modal_bind(dialog);
    if (focus && focus_layer)
        settings_layer_focus(&dialog.layer, {UiSettingsTargetKind::control, page, focus});
    application_sync_attachment_drop_target();
    return true;
}

void settings_modal_suspend(SettingsModal& modal) noexcept {
    application_composer_text_input_suspend();
    settings_layer_close_choice(&modal.layer);
    settings_layer_cancel_pointer(&modal.layer);
}

void settings_modal_unbind(SettingsModal& modal) noexcept {
    renderer_bind_settings_scene(&g_app.renderer, nullptr);
    modal.layer.entrance_started = 0;
    renderer_set_settings_modal_progress(&g_app.renderer, 1);
    ui_settings_scene_release(&modal.scene);
}

void settings_modal_restore_focus(bool restore_focus) noexcept {
    if (restore_focus)
        g_app.context_focus = &g_app.shell_scene.widgets;
    application_sync_attachment_drop_target();
}

void settings_modal_repaint_shell() noexcept {
    if (g_app.renderer_ready)
        refresh_account_text(true);
    application_request_frame();
}

void settings_modal_leave(SettingsModal& modal, bool restore_focus) noexcept {
    settings_modal_unbind(modal);
    settings_modal_restore_focus(restore_focus);
    settings_modal_repaint_shell();
}

void settings_modal_close(SettingsModal& modal, bool restore_focus) noexcept {
    if (g_exiting == &modal)
        return;
    settings_modal_finish_exit();
    if (!exit_allowed(modal)) {
        settings_modal_leave(modal, restore_focus);
        return;
    }
    // The scene stays bound and drawn, without its entrance, while it fades and shrinks back from where it is.
    g_exiting = &modal;
    modal.layer.entrance_started = 0;
    ui_motion_transition_reset(&g_exit_motion, {kExitMotionOwner, ++g_exit_generation},
                               g_app.renderer.settings_modal_progress);
    settings_modal_restore_focus(restore_focus);
    application_request_frame();
}

void settings_modal_finish_exit(const SettingsModal* modal) noexcept {
    auto* exiting = g_exiting;
    if (!exiting || (modal && modal != exiting))
        return;
    g_exiting = nullptr;
    // Another surface (Settings itself) may have taken the renderer meanwhile: then only the scene closes.
    if (g_app.renderer.settings_scene == &exiting->scene) {
        settings_modal_unbind(*exiting);
    } else {
        exiting->layer.entrance_started = 0;
        ui_settings_scene_release(&exiting->scene);
    }
    settings_modal_repaint_shell();
}

void settings_modal_exit_synchronize(u64 now_ms) noexcept {
    if (!g_exiting)
        return;
    if (g_app.renderer.settings_scene != &g_exiting->scene) {
        settings_modal_finish_exit();
        return;
    }
    const float shown =
        application_motion_tick(&g_exit_motion, g_exit_motion.scope, 0, now_ms, kUiMotionModalExit);
    renderer_set_settings_modal_progress(&g_app.renderer, shown);
    if (shown <= 0 && !g_exit_motion.active)
        settings_modal_finish_exit();
}

bool settings_modal_present(bool projected) noexcept {
    if (!projected)
        return false;
    application_request_frame();
    return !g_app.renderer_ready || renderer_rebuild_content_text(&g_app.renderer, application_theme());
}

bool settings_modal_update(SettingsModal& modal) noexcept {
    ui_settings_scene_set_focus_visible(&modal.scene, g_app.keyboard_focus_visible);
    return ui_settings_scene_update(&modal.scene, &modal.registry, application_viewport_dips());
}

bool settings_modal_sync_text(const SettingsModal& modal) noexcept {
    return g_app.renderer.interaction_text_settings_revision == modal.scene.revision ||
           renderer_rebuild_content_text(&g_app.renderer, application_theme());
}

bool settings_modal_refresh(SettingsModal& modal) noexcept {
    return ui_settings_scene_update(&modal.scene, &modal.registry, application_viewport_dips()) &&
           settings_modal_sync_text(modal);
}

} // namespace maicord::app_detail
