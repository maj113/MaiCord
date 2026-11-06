// Plugin keybinds: a key-down that matches a KEYBIND setting of an enabled plugin calls its hook. The
// application owns the keyboard whenever a dialog, menu or overlay is open or the window is in the
// background, so a chord never reaches a plugin then.
#include "plugins.hpp"
#include "maicord/app/plugins/dispatch.hpp"
#include "../shell/state.hpp"
#include "../shell/window_state.hpp"
#include "../media/downloads.hpp"
#include "../chat/threads/forum_form.hpp"
#include "../guild/lifecycle.hpp"
#include "../account/profile_editor.hpp"
#include "../guild/settings/settings.hpp"
#include "../settings/keybinds_settings.hpp"
#include "../media/picker.hpp"
#include "../chat/messages/forward.hpp"
#include "../chat/polls/poll.hpp"
#include "../chat/polls/create.hpp"
#include "../account/profile.hpp"
#include "../shell/quick_switcher.hpp"
#include "../chat/report.hpp"
#include "../settings/settings.hpp"
#include "../shell/shortcuts.hpp"
#include "../settings/theme_editor.hpp"
#include "../chat/threads/browser.hpp"
#include "../voice/settings.hpp"
#include "../account/custom_status.hpp"
#include "../account/friends.hpp"

#include "maicord/platform/window/window.hpp"

namespace maicord::app_detail {
namespace {

// Anything that owns the keyboard or covers the window.
bool keybinds_blocked() noexcept {
    return !g_app.account_ui || g_app.login.visible || g_app.text_form || g_app.popup_scene.active ||
           g_app.reaction_viewer.active || g_app.image_viewer.scene.active ||
           (g_app.profile && g_app.profile->scene.active) || application_settings_active() ||
           application_forum_form_active() || application_downloads_active() ||
           application_shortcuts_overlay_active() || application_message_forward_active() ||
           application_quick_switcher_active() || application_theme_editor_active() ||
           application_custom_status_active() || application_poll_create_active() ||
           application_poll_voters_active() || application_report_active() || application_thread_browser_active() ||
           application_guild_settings_active() || application_profile_editor_active() ||
           application_guild_lifecycle_active() || application_media_picker_active() ||
           application_friends_requests_active() || application_keybinds_settings_recording() ||
           application_voice_settings_recording();
}
} // namespace

bool application_plugins_keybind_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    if ((kind != UiInputKind::key_down && kind != UiInputKind::system_key_down) || code > 0xff || repeated ||
        shortcut_key_is_modifier(code) || keybinds_blocked() || (g_app.window && !application_window_foreground()))
        return false;
    const auto pressed = shortcut_chord_from_keyboard(code);
    const auto focus = application_shortcut_context().focus;
    const bool text_focus = focus == ShortcutFocus::text || focus == ShortcutFocus::composer;
    if (text_focus && (pressed.modifiers & (shortcut_ctrl | shortcut_alt)) == (shortcut_ctrl | shortcut_alt) &&
        window_layout_types_text(nullptr, pressed.key, pressed.modifiers))
        return false;
    return plugin_dispatch_keybind(pressed.key | (pressed.modifiers & shortcut_ctrl ? kPluginKeyControl : 0) |
                                   (pressed.modifiers & shortcut_shift ? kPluginKeyShift : 0) |
                                   (pressed.modifiers & shortcut_alt ? kPluginKeyAlt : 0));
}

} // namespace maicord::app_detail
