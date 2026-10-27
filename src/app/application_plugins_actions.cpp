// Host-executed plugin actions (mc.actions) and plugin keybind routing. The SDK
// layer has already validated, permission-gated, gesture-gated and rate-limited
// every request; this file maps them onto MaiCord's ordinary paths.
#include "application_plugins.hpp"
#include "application_plugins_runtime.hpp"
#include "application_plugins_actions.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_activity.hpp"
#include "application_channel_authority.hpp"
#include "application_composer.hpp"
#include "application_content_input.hpp"
#include "application_custom_status.hpp"
#include "application_downloads.hpp"
#include "application_forum_form.hpp"
#include "application_friends.hpp"
#include "application_guild_lifecycle.hpp"
#include "application_profile_editor.hpp"
#include "application_guild_settings.hpp"
#include "application_keybinds_settings.hpp"
#include "application_media_picker.hpp"
#include "application_message_edit.hpp"
#include "application_message_forward.hpp"
#include "application_message_history.hpp"
#include "application_message_reaction.hpp"
#include "application_poll.hpp"
#include "application_poll_create.hpp"
#include "application_profile.hpp"
#include "application_quick_switcher.hpp"
#include "application_read_state.hpp"
#include "application_report.hpp"
#include "application_settings.hpp"
#include "application_shortcuts.hpp"
#include "application_text_form.hpp"
#include "application_theme_editor.hpp"
#include "application_thread_actions.hpp"
#include "application_thread_browser.hpp"
#include "application_voice.hpp"
#include "application_voice_settings.hpp"
#include "maicord/app/message_composer_controller.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/self_status_settings.hpp"
#include "maicord/ui/clipboard.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/sys/winsock.hpp"

namespace maicord::app_detail {
namespace {
constexpr u64 kNoticeMs = 8000;

ApplicationPluginsSdk* sdk_owner() noexcept { return g_app.plugins ? g_app.plugins->sdk : nullptr; }
bool account_ready() noexcept {
    return g_app.account_ui && g_app.account.instance && !g_app.login.visible &&
           g_app.account.status.required_action == AccountRequirement::none;
}
const MessageRecord* cached_message(u64 channel_id, u64 message_id) noexcept {
    const auto* record = application_message_record(message_id);
    return record && record->channel_id == channel_id ? record : nullptr;
}

// ---- Messages -----------------------------------------------------------------------

// Straight to the send queue: no pre-send hooks, local commands or @mention
// confirmation run for plugin messages.
mc_status send_message(void*, u64, const NativePluginSdkSend& send) noexcept {
    if (!account_ready())
        return MC_E_BUSY;
    auto& ui = *g_app.account_ui;
    if (!chat_state_store_find_channel(ui.chat_state, send.channel_id))
        return MC_E_NOT_FOUND;
    if (!application_thread_actions_channel_sendable(send.channel_id))
        return MC_E_DENIED;
    if (send.reply_to && !cached_message(send.channel_id, send.reply_to))
        return MC_E_NOT_FOUND;
    if (send.reply_to && !application_channel_readable(g_app, send.channel_id))
        return MC_E_DENIED;
    char storage[kPluginSdkMessageBytes + 16];
    MessageComposer composer{};
    if (!message_composer_initialize(&composer, {storage, sizeof(storage)}) ||
        !message_composer_restore(&composer, send.channel_id, send.text, send.text.bytes, send.text.bytes))
        return MC_E_INVALID;
    const auto queued = message_composer_controller_queue_text(
        &ui.message_composer_controller, &composer, ui.chat_state.self_user_id, clock_unix_milliseconds(),
        clock_monotonic_milliseconds(), send.reply_to, (send.flags & MC_SEND_MENTION_REPLY) != 0,
        (send.flags & MC_SEND_SILENT) != 0);
    if (!queued.ok())
        return queued.error == MessageComposerControllerError::invalid_input ? MC_E_INVALID : MC_E_BUSY;
    pump_message_history();
    return MC_OK;
}

mc_status edit_message(void*, u64, u64 channel, u64 message, Utf8View text) noexcept {
    if (!account_ready())
        return MC_E_BUSY;
    const auto* record = cached_message(channel, message);
    // Edits go through the open channel's message view.
    if (!record || channel != g_app.account_ui->active_channel_id)
        return MC_E_NOT_FOUND;
    if (record->author_id != g_app.account_ui->chat_state.self_user_id ||
        !application_thread_actions_channel_sendable(channel))
        return MC_E_DENIED;
    if (g_app.message_edit.message_id || g_app.message_edit.editor_active)
        return MC_E_BUSY;
    return queue_message_edit_content(message, text) ? MC_OK : MC_E_DENIED;
}

mc_status react(void*, u64, u64 channel, u64 message, u64 emoji_id, Utf8View name) noexcept {
    if (!account_ready())
        return MC_E_BUSY;
    if (channel != g_app.account_ui->active_channel_id || !cached_message(channel, message))
        return MC_E_NOT_FOUND;
    const auto* emoji = emoji_id ? chat_state_store_find_emoji(g_app.account_ui->chat_state, emoji_id) : nullptr;
    const bool animated = emoji && (emoji->flags & custom_emoji_animated);
    switch (application_queue_message_reaction_emoji(message, emoji_id, name, animated)) {
    case ApplicationMessageReactionEnqueueResult::accepted:
    case ApplicationMessageReactionEnqueueResult::coalesced: return MC_OK;
    case ApplicationMessageReactionEnqueueResult::permission: return MC_E_DENIED;
    case ApplicationMessageReactionEnqueueResult::stale: return MC_E_NOT_FOUND;
    case ApplicationMessageReactionEnqueueResult::invalid: return MC_E_INVALID;
    default: return MC_E_BUSY;
    }
}

mc_status mark_read(void*, u64, u64 channel) noexcept {
    if (!account_ready())
        return MC_E_BUSY;
    if (!chat_state_store_find_channel(g_app.account_ui->chat_state, channel))
        return MC_E_NOT_FOUND;
    return application_read_state_mark(channel, true) ? MC_OK : MC_E_DENIED;
}

// ---- Presence ---------------------------------------------------------------------------

mc_status custom_status(void*, u64, Utf8View text, Utf8View emoji, u64 expires_ms) noexcept {
    if (!account_ready())
        return MC_E_BUSY;
    const u64 now = clock_unix_milliseconds();
    if (expires_ms && expires_ms <= now)
        return MC_E_INVALID;
    SelfCustomStatus status{};
    status.text_bytes = static_cast<u16>(utf8_copy_truncated(status.text, text));
    // "name:id" is a custom emoji; anything else is a Unicode emoji.
    u32 colon = emoji.bytes;
    for (u32 i = 0; i < emoji.bytes; ++i)
        if (emoji.data[i] == ':') {
            colon = i;
            break;
        }
    if (colon < emoji.bytes) {
        for (u32 i = colon + 1; i < emoji.bytes; ++i)
            status.emoji_id = status.emoji_id * 10 + static_cast<u64>(emoji.data[i] - '0');
        status.emoji_name_bytes = static_cast<u8>(utf8_copy_truncated(status.emoji_name, {emoji.data, colon}));
    } else {
        status.emoji_name_bytes = static_cast<u8>(utf8_copy_truncated(status.emoji_name, emoji));
    }
    status.present = status.text_bytes || status.emoji_name_bytes || status.emoji_id;
    if (status.present) {
        status.created_at_ms = now;
        status.expires_at_ms = expires_ms;
    }
    return application_custom_status_request(&status) ? MC_OK : MC_E_BUSY;
}

mc_status activity(void*, u64 owner, const NativePluginSdkActivity* source) noexcept {
    if (!source)
        return application_activity_set_plugin(owner, nullptr) ? MC_OK : MC_E_NOT_FOUND;
    const ApplicationPluginActivity value{source->application_id, source->start_ms, source->end_ms,
                                          static_cast<u8>(source->type), source->name, source->details,
                                          source->state};
    return application_activity_set_plugin(owner, &value) ? MC_OK : MC_E_BUSY;
}

// ---- Navigation ---------------------------------------------------------------------------

mc_status open_channel(void*, u64, u64 channel, u64 message) noexcept {
    if (!account_ready())
        return MC_E_BUSY;
    if (!chat_state_store_find_channel(g_app.account_ui->chat_state, channel))
        return MC_E_NOT_FOUND;
    return application_navigate_known_message(channel, message) ? MC_OK : MC_E_DENIED;
}

mc_status open_profile(void*, u64, u64 user, u64) noexcept {
    if (!account_ready())
        return MC_E_BUSY;
    // No on-screen source: the card opens at the middle of the window.
    const UiRect viewport = application_viewport_dips();
    const float x = (viewport.left + viewport.right) * 0.5f, y = (viewport.top + viewport.bottom) * 0.5f;
    return application_profile_open(user, {x - 1.0f, y - 1.0f, x + 1.0f, y + 1.0f}, ProfileAnchor::point) ? MC_OK
                                                                                                          : MC_E_BUSY;
}

mc_status open_page(void*, UiPageKey page) noexcept {
    return application_settings_open_plugin_page(page) ? MC_OK : MC_E_NOT_FOUND;
}

mc_status copy_text(void*, u64, Utf8View text) noexcept {
    return g_app.window && clipboard_write_utf8(g_app.window, text) ? MC_OK : MC_E_BUSY;
}

// Goes through the link trust rules and the "Leaving MaiCord" confirmation.
mc_status open_url(void*, u64, Utf8View url) noexcept {
    if (!account_ready())
        return MC_E_BUSY;
    return application_open_content_url(url) ? MC_OK : MC_E_BUSY;
}

mc_status notice(void*, u64, Utf8View text, u32 tone) noexcept {
    auto* sdk = sdk_owner();
    if (!sdk || !g_app.account_ui)
        return MC_E_BUSY;
    sdk->composer_notice_bytes = utf8_copy_truncated(sdk->composer_notice, text);
    sdk->composer_notice_until = clock_monotonic_milliseconds() + kNoticeMs;
    sdk->composer_notice_tone = static_cast<u8>(tone <= MC_TONE_CRITICAL ? tone : MC_TONE_NEUTRAL);
    sdk->composer_refresh = refresh_message_composer_ui;
    refresh_message_composer_ui();
    return MC_OK;
}

// ---- Modal ------------------------------------------------------------------------------

// The one plugin modal shown through the shared text form. Exactly one result
// is delivered per opened modal, from the form's closed callback.
struct PluginModal {
    u64 owner_id, modal_id, account_scope, serial;
    bool open, accepted;
    u32 text_bytes;
    char text[kPluginSdkMessageBytes];
    char body[2048];
};
PluginModal g_modal;
u64 g_modal_serial;

bool modal_valid(TextFormOwnerKey key) noexcept {
    const auto* rt = application_plugins_runtime();
    return g_modal.open && key.scope == g_modal.serial && key.collection == g_modal.owner_id && rt &&
           g_app.plugin_account_scope_serial == g_modal.account_scope && native_plugin_sdk_find(*rt, g_modal.owner_id);
}
bool modal_save(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!modal_valid(key))
        return false;
    g_modal.text_bytes = utf8_copy_truncated(g_modal.text, text);
    g_modal.accepted = true;
    return true;
}
void modal_closed(TextFormOwnerKey key) noexcept {
    if (!g_modal.open || key.scope != g_modal.serial)
        return;
    const u64 owner = g_modal.owner_id, modal = g_modal.modal_id;
    const bool accepted = g_modal.accepted;
    char text[kPluginSdkMessageBytes];
    const u32 bytes = accepted ? g_modal.text_bytes : 0;
    copy_bytes(text, g_modal.text, bytes);
    SecureZeroMemory(g_modal.text, sizeof(g_modal.text));
    g_modal.open = g_modal.accepted = false;
    g_modal.text_bytes = 0;
    if (auto* rt = application_plugins_runtime())
        native_plugin_sdk_modal_result(rt, owner, modal, accepted, {text, bytes});
    SecureZeroMemory(text, sizeof(text));
}

mc_status open_modal(void*, const NativePluginSdkModal& modal) noexcept {
    if (g_app.text_form || g_modal.open || !account_ready())
        return MC_E_BUSY;
    ApplicationTextFormRequest request{};
    // The text form holds 2048 bytes of labels; the body gets what's left.
    const u32 others = 64 + 256 + 256 + 16;
    const auto form = application_plugins_modal_form(modal, {g_modal.body, static_cast<u32>(sizeof(g_modal.body)) - others});
    request.title = form.title;
    request.context = form.context;
    request.label = form.label;
    request.accept = form.accept;
    request.initial = form.initial;
    request.maximum_bytes = form.maximum_bytes;
    request.confirmation_only = form.confirmation_only;
    request.single_line = form.single_line;
    request.danger = form.danger;
    const u64 serial = ++g_modal_serial;
    request.owner = {serial, modal.owner_id, modal.modal_id};
    request.valid = modal_valid;
    request.save = modal_save;
    request.closed = modal_closed;
    g_modal.owner_id = modal.owner_id;
    g_modal.modal_id = modal.modal_id;
    g_modal.account_scope = g_app.plugin_account_scope_serial;
    g_modal.serial = serial;
    g_modal.accepted = false;
    g_modal.text_bytes = 0;
    g_modal.open = true;
    if (!application_text_form_open(request)) {
        g_modal.open = false;
        return MC_E_BUSY;
    }
    return MC_OK;
}

// ---- Keybinds -----------------------------------------------------------------------------

u8 push_to_talk_key() noexcept {
    const auto& audio = application_voice_preferences().audio;
    return audio.mode == VoiceInputMode::push_to_talk ? audio.push_to_talk_key : 0;
}
bool keybind_check(void*, NativePluginSdkChord chord, Utf8View* conflict) noexcept {
    const Utf8View found =
        shortcut_plugin_chord_conflict(application_custom_keybinds(), {chord.key, chord.modifiers}, push_to_talk_key());
    if (conflict)
        *conflict = found;
    return found.bytes == 0;
}
struct KeybindWatch {
    const NativePluginSdkRuntime* runtime;
    u32 revision;
    u8 push_to_talk;
};
KeybindWatch g_keybind_watch;

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
// Ctrl+Alt is AltGr on many layouts; in a text field it types instead.
bool altgr_types(u8 key) noexcept {
    BYTE state[256]{};
    state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_MENU] = state[VK_RMENU] = 0x80;
    if (GetKeyState(VK_SHIFT) & 0x8000)
        state[VK_SHIFT] = 0x80;
    wchar_t output[4];
    // Flag 4: leave the keyboard's dead-key state untouched.
    const int produced = ToUnicodeEx(key, MapVirtualKeyW(key, MAPVK_VK_TO_VSC), state, output, 4, 4,
                                     GetKeyboardLayout(0));
    return produced < 0 || (produced > 0 && output[0] >= 0x20);
}
} // namespace

void application_plugins_actions_install(NativePluginSdkRuntime& runtime) noexcept {
    runtime.actions = {nullptr,     send_message, edit_message, react,     mark_read, custom_status, activity,
                       open_channel, open_profile, open_page,   copy_text, open_url,  notice,        open_modal};
    runtime.keybind_check = keybind_check;
    runtime.keybind_context = nullptr;
}

u8 application_plugins_composer_notice_tone() noexcept {
    const auto* sdk = sdk_owner();
    return sdk && sdk->composer_notice_bytes ? sdk->composer_notice_tone : u8{0};
}

bool application_plugins_keybind_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    auto* rt = application_plugins_runtime();
    if (!rt || (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down) || code > 0xff ||
        shortcut_key_is_modifier(code) || keybinds_blocked() || application_plugins_busy() ||
        (g_app.window && GetForegroundWindow() != g_app.window))
        return false;
    application_plugins_keybinds_synchronize();
    const auto pressed = shortcut_chord_from_keyboard(code);
    const NativePluginSdkChord chord{pressed.key, pressed.modifiers};
    const auto focus = application_shortcut_context().focus;
    const bool text_focus = focus == ShortcutFocus::text || focus == ShortcutFocus::composer;
    if (text_focus && (chord.modifiers & (shortcut_ctrl | shortcut_alt)) == (shortcut_ctrl | shortcut_alt) &&
        altgr_types(chord.key))
        return false;
    if (repeated) {
        // A held chord fires once; its repeats stay with the plugin.
        NativePluginSdkKeybindView views[64];
        const u32 count = native_plugin_sdk_keybinds(*rt, {views, 64});
        for (u32 i = 0; i < count; ++i)
            if (views[i].chord.key == chord.key && views[i].chord.modifiers == chord.modifiers)
                return !text_focus || (chord.modifiers & (shortcut_ctrl | shortcut_alt));
        return false;
    }
    return native_plugin_sdk_keybind_press(rt, chord, text_focus);
}

void application_plugins_keybinds_synchronize() noexcept {
    auto* rt = application_plugins_runtime();
    if (!rt)
        return;
    const u32 revision = application_custom_keybinds_revision();
    const u8 push_to_talk = push_to_talk_key();
    if (g_keybind_watch.runtime == rt && g_keybind_watch.revision == revision &&
        g_keybind_watch.push_to_talk == push_to_talk)
        return;
    // Chords published before were checked against the state at that time.
    const bool changed = g_keybind_watch.runtime == rt;
    g_keybind_watch = {rt, revision, push_to_talk};
    if (!changed)
        return;
    native_plugin_sdk_keybinds_recheck(rt);
    if (application_settings_active())
        application_settings_refresh_pages();
}

} // namespace maicord::app_detail
