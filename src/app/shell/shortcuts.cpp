#include "shortcuts.hpp"
#include "state.hpp"
#include "window_events.hpp"
#include "window_state.hpp"
#include "../account/account_presentation.hpp"
#include "../chat/composer/attachments.hpp"
#include "../chat/composer/composer.hpp"
#include "../chat/composer/text_input.hpp"
#include "content_input.hpp"
#include "cursor.hpp"
#include "../media/downloads.hpp"
#include "../chat/inbox.hpp"
#include "../settings/keybinds_settings.hpp"
#include "../media/picker.hpp"
#include "../chat/messages/edit.hpp"
#include "../chat/messages/forward.hpp"
#include "../chat/messages/interaction.hpp"
#include "../chat/messages/pinned_messages.hpp"
#include "../plugins/plugins.hpp"
#include "quick_switcher.hpp"
#include "../chat/search/search.hpp"
#include "../settings/settings.hpp"
#include "input.hpp"
#include "../voice/voice.hpp"
#include "../voice/settings.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/core/cursor.hpp"
#include "maicord/ui/shell/shortcuts_scene.hpp"
#include "../account/account_ui_runtime.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/platform/window/global_keys.hpp"
#include "maicord/ui/media/picker.hpp"

namespace maicord::app_detail {
namespace {
constexpr u8 kModifierOrder[]{shortcut_ctrl, shortcut_alt, shortcut_shift};
constexpr u32 kModifiersUnread = ~0u;
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
// Whether the chord is held. The modifier keys are read on the first held
// key and shared by the chords checked after it in the same pump.
bool chord_down(ShortcutChord chord, u32* held) noexcept {
    if (!chord.key || !global_key_down(chord.key))
        return false;
    if (*held == kModifiersUnread)
        *held = (global_key_down(key::ctrl) ? shortcut_ctrl : 0) | (global_key_down(key::alt) ? shortcut_alt : 0) |
                (global_key_down(key::shift) ? shortcut_shift : 0);
    return *held == chord.modifiers;
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
        reset_value(&row);
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
    const auto navigation = navigation_shortcut(action);
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
    const auto& keybinds = application_custom_keybinds();
    const bool background = g_app.window && !application_window_foreground();
    if (!background || !keybinds.count || !in_call()) {
        global.armed = false;
        return;
    }
    const u32 revision = application_custom_keybinds_revision();
    // Rebase on (re)arming: keys already held when focus left never fire.
    const bool rebase = !global.armed || global.revision != revision;
    global.armed = true;
    global.revision = revision;
    u32 held = kModifiersUnread;
    for (u32 i = 0; i < keybinds.count; ++i) {
        const bool down = chord_down(keybinds.items[i].chord, &held);
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
    if (kind == UiInputKind::system_key_down && code == key::f4)
        return false;
    const auto chord = shortcut_chord_from_keyboard(code);
    if ((code == key::escape && !chord.modifiers) || shortcut_matches(ShortcutAction::shortcuts_help, chord)) {
        if (!repeated) {
            overlay.held_key = code;
            if (code == key::escape && overlay.query_bytes) {
                overlay.query_bytes = 0; // Escape clears the query first.
                refresh();
            } else
                application_shortcuts_overlay_close();
        }
        return true;
    }
    float delta = 0;
    switch (code) {
    case key::backspace:
        if (pop_query())
            refresh();
        return true;
    case key::up: delta = -40; break;
    case key::down: delta = 40; break;
    case key::page_up: delta = -ui_rect_height(overlay.scene.body) * .8f; break;
    case key::page_down: delta = ui_rect_height(overlay.scene.body) * .8f; break;
    case key::home: delta = -overlay.scene.content_height; break;
    case key::end: delta = overlay.scene.content_height; break;
    default: return true;
    }
    if (ui_shortcuts_scene_scroll(&overlay.scene, delta))
        application_repaint();
    return true;
}

bool application_shortcuts_overlay_window_event(const WindowEvent& event) noexcept {
    if (!overlay.active || !ui_shortcuts_scene_current(overlay.scene))
        return false;
    if (input_vertical_wheel(event)) {
        if (ui_shortcuts_scene_scroll(&overlay.scene, -static_cast<float>(event.wheel.dy120) / 120.0f * 80))
            application_repaint();
        return true;
    }
    if (event.type == EventType::pointer_move) {
        bool changed = false;
        ui_shortcuts_scene_set_hover(&overlay.scene, client_dip_x(event), client_dip_y(event), &changed);
        if (changed)
            application_repaint();
        return true;
    }
    if (event.type == EventType::cursor_query) {
        UiPoint point{};
        if (!client_cursor_dip(event, &point))
            return false;
        application_set_cursor(ui_action_cursor(ui_shortcuts_scene_hit_test(overlay.scene, point.x, point.y) ==
                                                UiShortcutsTarget::close));
        return true;
    }
    if (input_down(event)) {
        if (ui_shortcuts_scene_hit_test(overlay.scene, client_dip_x(event), client_dip_y(event)) ==
            UiShortcutsTarget::close)
            application_shortcuts_overlay_close();
        return true;
    }
    // The other releases, the other buttons' presses and releases, tilts and the context menu are swallowed.
    return input_release(event) || input_other_button(event) || input_horizontal_wheel(event) ||
           event.type == EventType::context_menu;
}
} // namespace maicord::app_detail
