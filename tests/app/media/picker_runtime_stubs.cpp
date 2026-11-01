#include "picker_runtime_internal.hpp"

namespace maicord {
bool renderer_rebuild_content_text(D3D11Renderer*, const Theme&) noexcept {
    ++media_picker_runtime_test::text_builds;
    return true;
}
bool text_edit_layout_hit_test(const TextEditLayoutOwner&, float, float, u32* position) noexcept {
    if (!position)
        return false;
    *position = 0;
    return true;
}
namespace app_detail {
void reset_message_composer_caret_timing(bool) noexcept {}
bool stop_local_typing(u64) noexcept { return true; }
bool application_thread_header_activate_target(UiThreadHeader*, const UiThreadHeaderTarget&) noexcept { return false; }
float client_dip_x(LPARAM point) noexcept {
    return static_cast<short>(LOWORD(point)) * (96.f / maximum<u32>(g_app.renderer.dpi, 1));
}
float client_dip_y(LPARAM point) noexcept {
    return static_cast<short>(HIWORD(point)) * (96.f / maximum<u32>(g_app.renderer.dpi, 1));
}
bool handle_shell_click(LPARAM point) noexcept {
    const auto handle =
        ui_widget_manager_hit_test(g_app.shell_scene.widgets, client_dip_x(point), client_dip_y(point));
    const auto* widget = ui_widget_manager_resolve(g_app.shell_scene.widgets, handle);
    if (!widget || widget->id != static_cast<u32>(UiSemantic::member_toggle) + 1u)
        return false;
    ++media_picker_runtime_test::member_activations;
    g_app.member_pane_visible = !g_app.member_pane_visible;
    return true;
}
bool handle_composer_click(LPARAM point) noexcept {
    const auto control = message_composer_scene_hit_test(g_app.message_composer_scene, client_dip_x(point),
                                                         client_dip_y(point));
    UiMediaPickerKind kind{};
    if (control == MessageComposerControl::gif)
        kind = UiMediaPickerKind::gif;
    else if (control == MessageComposerControl::sticker)
        kind = UiMediaPickerKind::sticker;
    else if (control == MessageComposerControl::emoji)
        kind = UiMediaPickerKind::emoji;
    else
        return false;
    ++media_picker_runtime_test::launcher_activations;
    return application_media_picker_open(kind);
}
void application_surface_motion_retire(RendererLayerSurface) noexcept {
}
ApplicationMessageReactionEnqueueResult application_queue_message_reaction_emoji(u64 message_id, u64,
                                                                             Utf8View name, bool) noexcept {
    using namespace media_picker_runtime_test;
    if (reaction_calls < 4) {
        reaction_messages[reaction_calls] = message_id;
        reaction_names[reaction_calls] = name;
    }
    ++reaction_calls;
    return reaction_next;
}
Utf8View application_message_reaction_status(u64, bool* error) noexcept {
    if (error)
        *error = false;
    return {};
}
Utf8View application_message_reaction_admission_text(ApplicationMessageReactionEnqueueResult admission) noexcept {
    switch (admission) {
    case ApplicationMessageReactionEnqueueResult::capacity: return utf8_literal("Too many pending reactions.");
    case ApplicationMessageReactionEnqueueResult::permission: return utf8_literal("You cannot add reactions here.");
    case ApplicationMessageReactionEnqueueResult::storage: return utf8_literal("Unable to save this reaction.");
    default: return {};
    }
}
bool application_emoji_preferences_toggle_favorite(ReactionEmojiRef emoji) noexcept {
    return reaction_preferences_toggle_favorite_emoji(&g_app.account_ui->reaction_preferences, emoji).ok();
}
void application_emoji_preferences_open() noexcept {
}
bool application_gif_favorites_open() noexcept {
    media_picker_runtime_test::favorites_visible = media_picker_runtime_test::favorites_enabled;
    return media_picker_runtime_test::favorites_enabled;
}
void application_gif_favorites_set_visible(bool value) noexcept {
    media_picker_runtime_test::favorites_visible = value;
}
ApplicationGifFavoritesView application_gif_favorites_view() noexcept {
    return media_picker_runtime_test::favorites;
}
ApplicationGifFavoriteState application_gif_favorites_state(Utf8View) noexcept {
    return media_picker_runtime_test::favorite_state;
}
bool application_gif_favorites_toggle(Utf8View url, Utf8View src, u32, u32, GifFavoriteFormat format) noexcept {
    ++media_picker_runtime_test::favorite_toggles;
    media_picker_runtime_test::toggled_url = url;
    media_picker_runtime_test::toggled_src = src;
    media_picker_runtime_test::toggled_format = format;
    return true;
}
bool refresh_account_text(bool) noexcept {
    return true;
}
bool refresh_message_composer_ui() noexcept {
    return true;
}
bool composer_apply_result(MessageComposerResult result, bool) noexcept {
    if (result != MessageComposerResult::changed)
        return result == MessageComposerResult::unchanged || result == MessageComposerResult::capacity;
    return g_app.account_ui &&
           message_composer_controller_commit_draft(&g_app.account_ui->message_composer_controller).ok() &&
           refresh_message_composer_ui();
}
bool pump_message_history() noexcept {
    return true;
}
bool application_settings_active() noexcept {
    return media_picker_runtime_test::settings;
}
bool application_settings_commit_preferences() noexcept {
    ++media_picker_runtime_test::preference_saves;
    return true;
}
bool application_friends_active() noexcept {
    return false;
}
bool focus_message_composer_control(MessageComposerControl) noexcept {
    return true;
}
void application_composer_text_input_suspend() noexcept {
}
void application_composer_text_input_notify_layout() noexcept {
}
bool application_composer_text_input_synchronize() noexcept {
    return true;
}

} // namespace app_detail
} // namespace maicord
