#include "application_shortcuts.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_attachments.hpp"
#include "application_composer.hpp"
#include "application_composer_text_input.hpp"
#include "application_content_input.hpp"
#include "application_cursor.hpp"
#include "application_downloads.hpp"
#include "application_inbox.hpp"
#include "application_keybinds_settings.hpp"
#include "application_media_picker.hpp"
#include "application_message_edit.hpp"
#include "application_message_forward.hpp"
#include "application_message_interaction.hpp"
#include "application_pinned_messages.hpp"
#include "application_plugins.hpp"
#include "application_quick_switcher.hpp"
#include "application_search.hpp"
#include "application_settings.hpp"
#include "application_shell_input.hpp"
#include "application_voice.hpp"
#include "application_voice_settings.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/cursor.hpp"
#include "maicord/ui/shortcuts_scene.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
constexpr u8 kModifierOrder[]{shortcut_ctrl, shortcut_alt, shortcut_shift};
constexpr u32 kKeyBytes = 24, kQueryBytes = 64;
struct Overlay {
    UiShortcutsScene scene;
    UiShortcutsContent content;
    UiShortcutsRow rows[kUiShortcutsMaximumRows];
    ShortcutListingRow listing[kUiShortcutsMaximumRows];
    char keys[kUiShortcutsMaximumRows][2][kKeyBytes];
    char query[kQueryBytes];
    u32 query_bytes, held_key;
    bool active;
};
Overlay overlay{};
struct GlobalKeys {
    u32 revision;
    bool armed;
    bool down[kCustomKeybindMaximum];
};
GlobalKeys global{};

bool focused_message(u64* id) noexcept {
    const u64 message = g_app.message_scene.focused_message_id;
    if (g_app.context_focus != &g_app.message_scene.widgets || !message || !g_app.keyboard_focus_visible)
        return false;
    if (id)
        *id = message;
    return true;
}
NavigationShortcut navigation_of(ShortcutAction action) noexcept {
    switch (action) {
    case ShortcutAction::server_previous: return {NavigationMode::guild, true};
    case ShortcutAction::server_next: return {NavigationMode::guild, false};
    case ShortcutAction::channel_previous: return {NavigationMode::channel, true};
    case ShortcutAction::channel_next: return {NavigationMode::channel, false};
    case ShortcutAction::unread_previous: return {NavigationMode::unread, true};
    case ShortcutAction::unread_next: return {NavigationMode::unread, false};
    case ShortcutAction::mention_previous: return {NavigationMode::mention, true};
    case ShortcutAction::mention_next: return {NavigationMode::mention, false};
    default: return {};
    }
}
bool in_call() noexcept {
    if (!g_app.account_ui)
        return false;
    const auto& session = g_app.account_ui->voice_session;
    return session.channel_id && session.phase != VoiceSetupPhase::idle;
}
bool run_custom(CustomKeybindAction action) noexcept {
    switch (action) {
    case CustomKeybindAction::toggle_mute: return application_voice_toggle_mute();
    case CustomKeybindAction::toggle_deafen: return application_voice_toggle_deafen();
    default: return false; // Push to talk is held by the capture thread.
    }
}
bool chord_down(ShortcutChord chord) noexcept {
    if (!chord.key || !(GetAsyncKeyState(chord.key) & 0x8000))
        return false;
    const u8 held = static_cast<u8>(((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? shortcut_ctrl : 0) |
                                    ((GetAsyncKeyState(VK_MENU) & 0x8000) ? shortcut_alt : 0) |
                                    ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? shortcut_shift : 0));
    return held == chord.modifiers;
}

// Overlay content from the registry listing and the current query.
// A second chord with the first one's modifiers shows only its key
// ("Ctrl Alt ↑ / ↓"), as the official client lists previous/next pairs.
u32 chord_keys(const ShortcutChord chord, bool key_only, u32 row, u32 slot, Utf8View* keys) noexcept {
    if (!chord.key)
        return 0;
    u32 count = 0;
    for (const u8 modifier : kModifierOrder)
        if ((chord.modifiers & modifier) && !key_only)
            keys[count++] = shortcut_modifier_label(modifier);
    char* text = overlay.keys[row][slot];
    keys[count++] = {text, shortcut_key_label(chord.key, {text, kKeyBytes})};
    return count;
}
void project() noexcept {
    const u32 listed = shortcut_listing({overlay.query, overlay.query_bytes}, {overlay.listing, kUiShortcutsMaximumRows});
    for (u32 i = 0; i < listed; ++i) {
        const auto& source = overlay.listing[i];
        auto& row = overlay.rows[i];
        row = {};
        row.label = source.label;
        row.header = source.header;
        const bool shared = source.chords[0].key && source.chords[1].modifiers &&
                            source.chords[1].modifiers == source.chords[0].modifiers;
        for (u32 chord = 0; chord < 2 && !source.header; ++chord)
            row.key_counts[chord] =
                static_cast<u8>(chord_keys(source.chords[chord], chord == 1 && shared, i, chord, row.keys[chord]));
    }
    overlay.content.title = utf8_literal("Keyboard shortcuts");
    overlay.content.placeholder = utf8_literal("Search shortcuts");
    overlay.content.empty = utf8_literal("No shortcuts match your search.");
    overlay.content.query = {overlay.query, overlay.query_bytes};
    overlay.content.rows = {overlay.rows, listed};
    ++overlay.content.revision;
}
bool append_query(u32 character) noexcept {
    if (character < 0x20 || character == 0x7f || (character >= 0xd800 && character <= 0xdfff) || character > 0xffff)
        return false;
    char encoded[4];
    const u32 bytes = utf8_encode(character, encoded);
    if (overlay.query_bytes + bytes > kQueryBytes)
        return false;
    copy_bytes(overlay.query + overlay.query_bytes, encoded, bytes);
    overlay.query_bytes += bytes;
    return true;
}
bool pop_query() noexcept {
    if (!overlay.query_bytes)
        return false;
    do
        --overlay.query_bytes;
    while (overlay.query_bytes && (static_cast<u8>(overlay.query[overlay.query_bytes]) & 0xc0) == 0x80);
    return true;
}
bool refresh() noexcept {
    project();
    return application_shortcuts_overlay_synchronize() && (application_repaint(), true);
}
} // namespace

ShortcutContext application_shortcut_context() noexcept {
    ShortcutContext context{ShortcutFocus::content, false};
    const auto& composer = g_app.message_composer;
    if (application_search_edit_focused() || application_media_picker_edit_focused() ||
        application_quick_switcher_edit_focused())
        context.focus = ShortcutFocus::text;
    else if (composer.focused &&
             (!g_app.context_focus || g_app.context_focus == &g_app.message_composer_scene.widgets))
        context.focus = ShortcutFocus::composer;
    else if (focused_message(nullptr))
        context.focus = ShortcutFocus::message;
    context.composer_empty = composer.bytes == 0 && !g_app.message_edit.editor_active &&
                             !application_composer_text_input_composing();
    return context;
}

ShortcutAction application_shortcut_action(u32 key) noexcept {
    return shortcut_first(application_shortcut_context(), shortcut_chord_from_keyboard(key));
}

static bool run_shortcut(ShortcutAction action, bool repeated) noexcept {
    using A = ShortcutAction;
    const auto navigation = navigation_of(action);
    if (navigation.mode != NavigationMode::none)
        return handle_navigation_shortcut(navigation);
    // Owners that see keys before the content route match their chords through
    // the registry themselves: the quick switcher, search, downloads and
    // settings (they also close or refocus their own surfaces). The Escape
    // steps run in the content route, ordered with link cancellation.
    switch (action) {
    case A::none:
    case A::quick_switcher:
    case A::search:
    case A::downloads:
    case A::open_settings:
    case A::mark_server_read:
    case A::mark_channel_read:
        return false;
    default:
        break;
    }
    u64 message = 0;
    // One-shot actions swallow auto-repeat once they own the chord.
    if (repeated)
        return true;
    switch (action) {
    case A::toggle_pins:
        return application_pinned_messages_available() && application_pinned_messages_toggle();
    case A::toggle_inbox:
        return application_inbox_available() && application_inbox_toggle();
    case A::toggle_members:
        // Ctrl+U: the recipient's profile in a one-to-one DM, as in the official client.
        if (g_app.shell_scene.side_panel_profile)
            return g_app.shell_scene.side_panel_unavailable ||
                   application_preferences_dm_profile_visibility(!g_app.preferences.dm_profile_visible);
        return application_preferences_member_visibility(!g_app.member_pane_visible);
    case A::emoji_picker:
        return application_media_picker_open(UiMediaPickerKind::emoji);
    case A::gif_picker:
        return application_media_picker_open(UiMediaPickerKind::gif);
    case A::sticker_picker:
        return application_media_picker_open(UiMediaPickerKind::sticker);
    case A::upload_file:
        return application_pick_attachments();
    case A::edit_last_message: {
        // An empty composer edits your newest message, as in the official client.
        const u64 latest = application_message_edit_latest_own();
        return latest && begin_message_edit(latest) && refresh_message_composer_ui();
    }
    case A::message_reply:
        return focused_message(&message) && begin_message_reply(message) && refresh_message_composer_ui();
    case A::message_edit:
        return focused_message(&message) && begin_message_edit(message) && refresh_message_composer_ui();
    case A::message_forward:
        return focused_message(&message) && application_message_forward_open(message);
    case A::message_react: {
        UiRect anchor{};
        return focused_message(&message) && ui_widget_manager_context_anchor(g_app.message_scene.widgets, &anchor) &&
               open_reaction_picker(message, anchor);
    }
    case A::save_attachment:
        return focused_message(nullptr) && application_downloads_save_focused();
    case A::toggle_mute:
        return application_voice_toggle_mute();
    case A::toggle_deafen:
        return application_voice_toggle_deafen();
    case A::shortcuts_help:
        return application_shortcuts_overlay_open();
    default:
        return false;
    }
}

bool handle_shortcut_key(u32 key, bool repeated) noexcept {
    if (!g_app.account_ui || g_app.login.visible || shortcut_key_is_modifier(key))
        return false;
    const auto chord = shortcut_chord_from_keyboard(key);
    if (!chord.key)
        return false;
    ShortcutAction candidates[4]{};
    const u32 count = shortcut_resolve(application_shortcut_context(), chord, {candidates, 4});
    for (u32 i = 0; i < count; ++i)
        if (run_shortcut(candidates[i], repeated))
            return true;
    return false;
}

bool application_custom_keybind_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    if ((kind != UiInputKind::key_down && kind != UiInputKind::system_key_down) || !g_app.account_ui ||
        g_app.login.visible || shortcut_key_is_modifier(code) || application_keybinds_settings_recording() ||
        application_voice_settings_recording())
        return false;
    const auto& custom = application_custom_keybinds();
    const u32 index = custom.count ? custom_keybinds_find(custom, shortcut_chord_from_keyboard(code)) : kCustomKeybindMaximum;
    // Plugin chords never match a custom keybind; they apply only outside overlays.
    if (index >= kCustomKeybindMaximum)
        return application_plugins_keybind_input(kind, code, repeated);
    if (!repeated)
        run_custom(custom.items[index].action);
    return true;
}

void application_shortcuts_pump() noexcept {
    application_plugins_keybinds_synchronize();
    const auto& keybinds = application_custom_keybinds();
    const bool background = g_app.window && GetForegroundWindow() != g_app.window;
    if (!background || !keybinds.count || !in_call()) {
        global.armed = false;
        return;
    }
    const u32 revision = application_custom_keybinds_revision();
    // Rebase on (re)arming: keys already held when focus left never fire.
    const bool rebase = !global.armed || global.revision != revision;
    global.armed = true;
    global.revision = revision;
    for (u32 i = 0; i < keybinds.count; ++i) {
        const bool down = chord_down(keybinds.items[i].chord);
        if (down && !global.down[i] && !rebase)
            run_custom(keybinds.items[i].action);
        global.down[i] = down;
    }
}

u64 application_shortcuts_next_wake() noexcept {
    // The capture thread samples push to talk every 20 ms; toggles match it.
    return global.armed ? GetTickCount64() + 20 : 0;
}

bool application_shortcuts_overlay_active() noexcept {
    return overlay.active;
}
bool application_shortcuts_overlay_open() noexcept {
    if (overlay.active || !g_app.account_ui || g_app.login.visible)
        return overlay.active;
    if (g_app.popup_scene.active)
        dismiss_popup();
    overlay.active = true;
    overlay.query_bytes = 0;
    overlay.scene.scroll = 0;
    if (!refresh()) {
        application_shortcuts_overlay_close();
        return false;
    }
    return true;
}
void application_shortcuts_overlay_close() noexcept {
    if (!overlay.active)
        return;
    overlay.active = false;
    overlay.query_bytes = 0;
    overlay.scene.active = false;
    g_app.renderer.shortcuts_scene = nullptr;
    application_repaint();
}
void application_shortcuts_overlay_release() noexcept {
    overlay.active = false;
    overlay.held_key = 0;
    g_app.renderer.shortcuts_scene = nullptr;
    ui_shortcuts_scene_release(&overlay.scene);
}
bool application_shortcuts_overlay_synchronize() noexcept {
    if (!overlay.active) {
        g_app.renderer.shortcuts_scene = nullptr;
        return true;
    }
    if (!g_app.account_ui || g_app.login.visible ||
        !ui_shortcuts_scene_update(&overlay.scene, &overlay.content, application_viewport_dips())) {
        application_shortcuts_overlay_close();
        return true;
    }
    g_app.renderer.shortcuts_scene = &overlay.scene;
    return true;
}

bool application_shortcuts_overlay_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    // The key that opened or closed the overlay keeps its character and release.
    if (overlay.held_key) {
        if (kind == UiInputKind::key_up && code == overlay.held_key) {
            overlay.held_key = 0;
            return true;
        }
        if (kind == UiInputKind::character || kind == UiInputKind::system_character ||
            ((kind == UiInputKind::key_down || kind == UiInputKind::system_key_down) && code == overlay.held_key &&
             repeated))
            return true;
        overlay.held_key = 0;
    }
    if (!overlay.active)
        return false;
    if (kind == UiInputKind::character) {
        if (append_query(code))
            refresh();
        return true;
    }
    if (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down)
        return true;
    if (kind == UiInputKind::system_key_down && code == VK_F4)
        return false;
    const auto chord = shortcut_chord_from_keyboard(code);
    if ((code == VK_ESCAPE && !chord.modifiers) || shortcut_matches(ShortcutAction::shortcuts_help, chord)) {
        if (!repeated) {
            overlay.held_key = code;
            if (code == VK_ESCAPE && overlay.query_bytes) {
                overlay.query_bytes = 0; // Escape clears the query first.
                refresh();
            } else
                application_shortcuts_overlay_close();
        }
        return true;
    }
    float delta = 0;
    switch (code) {
    case VK_BACK:
        if (pop_query())
            refresh();
        return true;
    case VK_UP: delta = -40; break;
    case VK_DOWN: delta = 40; break;
    case VK_PRIOR: delta = -ui_rect_height(overlay.scene.body) * .8f; break;
    case VK_NEXT: delta = ui_rect_height(overlay.scene.body) * .8f; break;
    case VK_HOME: delta = -overlay.scene.content_height; break;
    case VK_END: delta = overlay.scene.content_height; break;
    default: return true;
    }
    if (ui_shortcuts_scene_scroll(&overlay.scene, delta))
        application_repaint();
    return true;
}

bool application_shortcuts_overlay_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (!overlay.active || !ui_shortcuts_scene_current(overlay.scene))
        return false;
    switch (message) {
    case WM_MOUSEWHEEL:
        if (ui_shortcuts_scene_scroll(&overlay.scene, -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 80))
            application_repaint();
        return true;
    case WM_MOUSEMOVE: {
        bool changed = false;
        ui_shortcuts_scene_set_hover(&overlay.scene, client_dip_x(lparam), client_dip_y(lparam), &changed);
        if (changed)
            application_repaint();
        return true;
    }
    case WM_SETCURSOR: {
        UiPoint point{};
        if (LOWORD(lparam) != HTCLIENT || !application_cursor_point(g_app.window, g_app.renderer.dpi, lparam, &point))
            return false;
        application_set_cursor(ui_action_cursor(ui_shortcuts_scene_hit_test(overlay.scene, point.x, point.y) ==
                                                UiShortcutsTarget::close));
        return true;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (ui_shortcuts_scene_hit_test(overlay.scene, client_dip_x(lparam), client_dip_y(lparam)) ==
            UiShortcutsTarget::close)
            application_shortcuts_overlay_close();
        return true;
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_MOUSEHWHEEL:
    case WM_CONTEXTMENU:
        return true;
    default:
        return false;
    }
}
} // namespace maicord::app_detail
