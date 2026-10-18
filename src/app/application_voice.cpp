#include "application_user_actions.hpp"
#include "application_content_point.hpp"
#include "application_voice.hpp"
#include "application_call.hpp"
#include "application_stage.hpp"
#include "application_shortcuts.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_message_interaction.hpp"
#include "application_state.hpp"
#include "application_voice_settings.hpp"
#include "maicord/app/voice_connection.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/voice_channel_rows.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
constexpr u32 kMenuMute = 0x7a00'0001;
constexpr u16 kVolumeSteps = 201; // 0..200 %.
struct VoiceUi {
    VoicePreferences preferences;
    UiVoicePanelView view;
    VoiceAudioStatus status;
    void* pushed_instance;
    u64 pushed_user;
    u64 status_at_ms;
    u64 menu_user;
    bool loaded, dirty, monitor, mute_before_deafen, users_dirty, menu_dragging, save_pending;
    char menu_label[48];
    u32 menu_label_bytes;
    UiVoiceElapsedClock timer;
};
VoiceUi voice{};

// Voice visuals are not part of the renderer's presentation stamp.
const GatewayClientVoiceSnapshot* session() noexcept {
    return g_app.account_ui ? &g_app.account_ui->voice_session : nullptr;
}
// DM and group calls have no guild; their voice server is keyed by the channel.
bool in_call(const GatewayClientVoiceSnapshot& s) noexcept {
    return s.channel_id && s.phase != VoiceSetupPhase::idle;
}
const UiRect* footer() noexcept {
    static UiRect rectangle;
    const auto* node =
        ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::channel_footer));
    if (!node || !g_app.account_ui || g_app.login.visible)
        return nullptr;
    rectangle = node->rectangle;
    return &rectangle;
}
void save() noexcept {
    if (!voice_preferences_save(voice.preferences))
        log_write(LogLevel::warning, "voice", "preferences_save_failed");
}
void ensure_loaded() noexcept {
    if (voice.loaded)
        return;
    voice.loaded = true;
    voice.preferences = voice_preferences_defaults();
    voice_preferences_load(&voice.preferences);
    voice.dirty = true;
}
// Desired self state for the current call (or the next join).
bool request(u64 guild_id, u64 channel_id) noexcept {
    auto& h = g_app.account;
    // A stage connection may run without DAVE; the module needs the kind.
    const auto* channel = g_app.account_ui && channel_id
                              ? chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id)
                              : nullptr;
    const GatewayVoiceStateUpdate state{guild_id, channel_id, voice.preferences.self_mute,
                                        voice.preferences.self_deaf, false,
                                        channel && channel->guild_id && channel->type == 13};
    if (!h.instance || !h.api.voice_request || !h.api.voice_request(h.instance, &state)) {
        log_write(LogLevel::warning, "voice", channel_id ? "request_rejected" : "leave_rejected");
        return false;
    }
    return true;
}
void apply_self_state() noexcept {
    if (const auto* s = session(); s && in_call(*s))
        request(s->guild_id, s->channel_id);
}
void push() noexcept {
    auto& h = g_app.account;
    if (!h.instance || !h.api.voice_audio_configure)
        return;
    VoiceAudioSettings settings = voice.preferences.audio;
    settings.monitor = voice.monitor;
    const u64 user = session() ? session()->user_id : 0;
    if (!voice.dirty && h.instance == voice.pushed_instance && user == voice.pushed_user)
        return;
    if (!h.api.voice_audio_configure(h.instance, &settings))
        return;
    if (h.api.soundboard_audio) {
        SoundboardAudioCommand soundboard{};
        soundboard.operation = SoundboardAudioOperation::configure;
        soundboard.muted = voice.preferences.soundboard_muted;
        soundboard.volume_percent = voice.preferences.soundboard_volume;
        h.api.soundboard_audio(h.instance, &soundboard, nullptr);
    }
    // A new account session starts with fresh playback: restore user volumes.
    if (h.instance != voice.pushed_instance || user != voice.pushed_user)
        for (u32 i = 0; h.api.voice_user_audio && i < voice.preferences.user_count; ++i) {
            const auto& u = voice.preferences.users[i];
            h.api.voice_user_audio(h.instance, u.user_id, u.gain_percent, u.muted);
        }
    voice.dirty = false;
    voice.pushed_instance = h.instance;
    voice.pushed_user = user;
}
UiVoiceSignal signal(u16 round_trip_ms) noexcept {
    return !round_trip_ms         ? UiVoiceSignal::unknown
           : round_trip_ms < 150 ? UiVoiceSignal::good
           : round_trip_ms < 300 ? UiVoiceSignal::fair
                                 : UiVoiceSignal::poor;
}
bool same_view(const UiVoicePanelView& a, const UiVoicePanelView& b) noexcept {
    return a.guild_id == b.guild_id && a.channel_id == b.channel_id && a.state == b.state && a.signal == b.signal &&
           a.self_mute == b.self_mute && a.self_deaf == b.self_deaf && a.server_mute == b.server_mute &&
           a.server_deaf == b.server_deaf && a.speaking == b.speaking && a.hovered == b.hovered &&
           a.pressed == b.pressed;
}
void refresh_view() noexcept {
    voice.view.elapsed = &voice.timer;
    UiVoicePanelView next = voice.view;
    const auto* s = session();
    const bool joined = s && in_call(*s);
    next.guild_id = joined ? s->guild_id : 0;
    next.channel_id = joined ? s->channel_id : 0;
    next.state = !joined ? UiVoicePanelState::hidden
                 : s->error != GatewayVoiceError::none || s->connection_error != VoiceConnectionError::none
                     ? UiVoicePanelState::failed
                 : s->socket_phase == VoiceSocketPhase::open && s->udp_phase == VoiceUdpPhase::ready
                     ? UiVoicePanelState::connected
                     : UiVoicePanelState::connecting;
    next.signal = signal(voice.status.round_trip_ms);
    next.self_mute = voice.preferences.self_mute;
    next.self_deaf = voice.preferences.self_deaf;
    next.server_mute = joined && (s->flags & gateway_voice_mute);
    next.server_deaf = joined && (s->flags & gateway_voice_deaf);
    // The worker's speaker list is current every pump; the audio status that
    // also reports speech is polled once a second.
    next.speaking = joined && (gateway_voice_self_speaking(*s) || voice.status.transmitting);
    // The channel list shows guild voice channels only.
    if (ui_voice_elapsed_observe(&voice.timer, next.guild_id ? next.channel_id : 0,
                                 next.state == UiVoicePanelState::connected, clock_monotonic_milliseconds()))
        application_repaint();
    if (next.state == UiVoicePanelState::hidden && (next.hovered == UiVoiceControl::disconnect ||
                                                    next.hovered == UiVoiceControl::channel))
        next.hovered = UiVoiceControl::none;
    if (next.state != voice.view.state) {
        // The panel's "Couldn't connect" state, with the worker's reasons.
        log_u64(next.state == UiVoicePanelState::failed ? LogLevel::error : LogLevel::info, "voice", "panel_state",
                static_cast<u64>(next.state));
        if (next.state == UiVoicePanelState::failed && s) {
            log_u64(LogLevel::error, "voice", "gateway_voice_error", static_cast<u64>(s->error));
            log_u64(LogLevel::error, "voice", "connection_error", static_cast<u64>(s->connection_error));
            log_u64(LogLevel::error, "voice", "close_code", s->voice_close_code);
            log_u64(LogLevel::error, "voice", "udp_error", static_cast<u64>(s->udp_error));
        }
    }
    const bool layout = next.state != voice.view.state &&
                        (next.state == UiVoicePanelState::hidden || voice.view.state == UiVoicePanelState::hidden);
    if (!same_view(next, voice.view)) {
        voice.view = next;
        if (layout)
            synchronize_shell_layout();
        application_repaint();
    }
}
UiVoiceControl hit(float x, float y) noexcept {
    const UiRect* rectangle = footer();
    return rectangle ? ui_voice_hit(*rectangle, voice.view, x, y) : UiVoiceControl::none;
}
// The participant row under the pointer of the message being handled (a
// click on a voice channel row), when that user has their camera on.
u64 camera_user_at_message_point() noexcept {
    const DWORD position = GetMessagePos();
    POINT point{static_cast<short>(LOWORD(position)), static_cast<short>(HIWORD(position))};
    if (!g_app.window || !application_screen_to_content(&point))
        return 0;
    const float scale = 96.f / static_cast<float>(g_app.renderer.dpi ? g_app.renderer.dpi : 96u);
    const auto* state = application_voice_participant_at(
        static_cast<float>(point.x) * scale, static_cast<float>(point.y) * scale, nullptr, nullptr);
    return state && (state->flags & voice_self_video) ? state->user_id : 0;
}
void set_user(u64 user_id, u32 gain, bool muted) noexcept {
    auto& h = g_app.account;
    voice_preferences_set_user(&voice.preferences, user_id, gain, muted);
    voice.users_dirty = true;
    if (h.instance && h.api.voice_user_audio)
        h.api.voice_user_audio(h.instance, user_id, gain, muted);
}
void format_label(u32 gain) noexcept {
    TextBank label{voice.menu_label, sizeof(voice.menu_label), 0};
    label.add(utf8_literal("User volume · "));
    label.number(gain);
    label.add(utf8_literal("%"));
    voice.menu_label_bytes = label.used;
}
bool menu_open() noexcept {
    return voice.menu_user && g_app.popup_scene.active && g_app.popup_scene.owner_id == voice.menu_user &&
           g_app.popup_owner_kind == PopupOwnerKind::member && g_app.popup_scene.feedback.slider_count;
}
} // namespace

void application_voice_pump() noexcept {
    ensure_loaded();
    if (g_app.account_ui)
        g_app.account_ui->navigation_view.voice_panel = &voice.view;
    push();
    auto& h = g_app.account;
    const auto* s = session();
    const u64 now = GetTickCount64();
    if (h.instance && h.api.voice_audio_status && ((s && in_call(*s)) || voice.monitor) &&
        now >= voice.status_at_ms) {
        VoiceAudioStatus status{};
        if (h.api.voice_audio_status(h.instance, &status))
            voice.status = status;
        voice.status_at_ms = now + (voice.monitor ? 30 : 1000);
    } else if (!(s && in_call(*s)) && !voice.monitor)
        voice.status = {};
    refresh_view();
    application_voice_settings_tick();
    if (voice.save_pending && !(GetKeyState(VK_LBUTTON) & 0x8000)) {
        voice.save_pending = false;
        save();
    }
    if (voice.users_dirty && !menu_open() && !voice.menu_dragging) {
        voice.users_dirty = false;
        save();
    }
    application_shortcuts_pump();
    application_call_voice_pump();
    application_stage_pump();
}

u64 application_voice_next_wake() noexcept {
    const auto* s = session();
    // Background voice keybinds sample their chords on the same cadence as push to talk.
    const u64 keys = application_shortcuts_next_wake();
    const auto earliest = [keys](u64 wake) noexcept { return !keys ? wake : !wake ? keys : minimum(wake, keys); };
    if (!voice.monitor && !(s && in_call(*s)))
        return earliest(voice.users_dirty || voice.save_pending ? GetTickCount64() + 250 : 0);
    return earliest(voice.status_at_ms ? voice.status_at_ms : 1);
}

float application_voice_panel_reserve() noexcept {
    return g_app.account_ui ? ui_voice_panel_reserve(&voice.view) : 0;
}

bool application_voice_join(u64 guild_id, u64 channel_id) noexcept {
    ensure_loaded();
    const auto* channel = g_app.account_ui && channel_id
                              ? chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id)
                              : nullptr;
    if (!channel_id || (!guild_id && !(channel && channel_is_private(channel->type))))
        return false;
    // As in the official client, a voice channel opens its call view; a click on a
    // participant whose camera is on focuses their tile.
    const auto* s = session();
    const u64 focus = guild_id ? camera_user_at_message_point() : 0;
    if (!(s && in_call(*s) && s->channel_id == channel_id))
        request(guild_id, channel_id);
    application_call_open(channel_id, focus);
    return true;
}

bool application_voice_leave() noexcept {
    // Disconnecting returns to the text channel at once, as in the official client.
    application_call_close();
    const auto* s = session();
    if (!s || !in_call(*s))
        return true;
    request(s->guild_id, 0);
    return true;
}

bool application_voice_toggle_mute() noexcept {
    ensure_loaded();
    auto& p = voice.preferences;
    if (p.self_deaf) // Unmuting while deafened also undeafens, as in the official client.
        p.self_deaf = p.self_mute = false;
    else
        p.self_mute = !p.self_mute;
    save();
    apply_self_state();
    refresh_view();
    return true;
}

bool application_voice_toggle_deafen() noexcept {
    ensure_loaded();
    auto& p = voice.preferences;
    if (!p.self_deaf) {
        voice.mute_before_deafen = p.self_mute;
        p.self_deaf = p.self_mute = true;
    } else {
        p.self_deaf = false;
        p.self_mute = voice.mute_before_deafen;
    }
    save();
    apply_self_state();
    refresh_view();
    return true;
}

u64 application_voice_timer_next_wake() noexcept {
    return ui_voice_elapsed_next_wake(voice.timer);
}

void application_voice_timer_tick(u64 now_ms) noexcept {
    if (!voice.timer.started_ms)
        return;
    const bool window_hidden = !g_app.window || IsIconic(g_app.window) || !IsWindowVisible(g_app.window);
    const auto& scene = g_app.shell_navigation_scene;
    bool visible = false;
    for (u32 i = 0; !window_hidden && scene.active && !renderer_settings_hides_shell(g_app.renderer) && i < scene.count;
         ++i) {
        if (scene.rows[i].view.id != voice.timer.channel_id)
            continue;
        const auto* layout = shell_navigation_scene_layout_node(scene, scene.rows[i]);
        visible = layout && ui_rect_has_area(ui_rect_intersection(layout->rectangle, scene.viewport));
        break;
    }
    if (ui_voice_elapsed_tick(&voice.timer, now_ms, visible))
        application_repaint();
}

UiVoicePanelView application_voice_view() noexcept {
    return voice.view;
}

bool application_voice_shell_click(float x, float y) noexcept {
    switch (hit(x, y)) {
    case UiVoiceControl::mute:
        return application_voice_toggle_mute();
    case UiVoiceControl::deafen:
        return application_voice_toggle_deafen();
    case UiVoiceControl::disconnect:
        return application_voice_leave();
    case UiVoiceControl::channel:
        if (const auto* s = session(); s && in_call(*s))
            application_call_open(s->channel_id, 0); // The voice card opens the call view.
        return true;
    default:
        return false;
    }
}

const VoiceState* application_voice_participant_at(float x, float y, UiRect* row, UiRect* live) noexcept {
    const auto& scene = g_app.shell_navigation_scene;
    if (!scene.active || !g_app.account_ui || !ui_rect_contains(scene.viewport, x, y))
        return nullptr;
    for (u32 i = 0; i < scene.count; ++i) {
        const auto& channel_row = scene.rows[i];
        const auto* layout = channel_row.view.voice_count ? shell_navigation_scene_layout_node(scene, channel_row) : nullptr;
        UiVoiceParticipantRow user{};
        for (u32 j = 0; layout && ui_voice_participant_row(channel_row.view, layout->rectangle,
                                                           g_app.account_ui->voice_states, j, &user);
             ++j) {
            // Rows scrolled under the list's edges are not hoverable.
            const UiRect visible = ui_rect_intersection(user.row, scene.viewport);
            if (!ui_rect_contains(visible, x, y))
                continue;
            if (row)
                *row = visible;
            if (live)
                *live = ui_rect_intersection(user.live, scene.viewport);
            return user.state;
        }
    }
    return nullptr;
}

bool application_voice_shell_hover(float x, float y) noexcept {
    const auto control = hit(x, y);
    if (control != voice.view.hovered) {
        voice.view.hovered = control;
        application_repaint();
    }
    return control != UiVoiceControl::none;
}

bool application_voice_hand_cursor(float x, float y) noexcept {
    return hit(x, y) != UiVoiceControl::none;
}

Utf8View application_voice_tooltip(float x, float y, UiRect* anchor, u32* identity) noexcept {
    const UiRect* rectangle = footer();
    const auto control = rectangle ? ui_voice_hit(*rectangle, voice.view, x, y) : UiVoiceControl::none;
    const auto& v = voice.view;
    Utf8View label{};
    UiRect area{};
    switch (control) {
    case UiVoiceControl::mute:
        label = v.server_mute                ? utf8_literal("Server muted")
                : v.self_mute || v.self_deaf ? utf8_literal("Unmute")
                                             : utf8_literal("Mute");
        area = ui_voice_account_button(*rectangle, control);
        break;
    case UiVoiceControl::deafen:
        label = v.server_deaf ? utf8_literal("Server deafened")
                : v.self_deaf ? utf8_literal("Undeafen")
                              : utf8_literal("Deafen");
        area = ui_voice_account_button(*rectangle, control);
        break;
    case UiVoiceControl::disconnect:
        label = utf8_literal("Disconnect");
        area = ui_voice_panel_disconnect(ui_voice_panel_rectangle(*rectangle));
        break;
    case UiVoiceControl::channel:
        label = utf8_literal("Show call");
        area = ui_voice_panel_channel_text(ui_voice_panel_rectangle(*rectangle));
        break;
    default:
        return {};
    }
    if (anchor)
        *anchor = area;
    if (identity)
        *identity = static_cast<u32>(control) * 4 + (v.self_mute ? 1 : 0) + (v.self_deaf ? 2 : 0);
    return label;
}

bool application_voice_context_at(float x, float y, UiRect anchor) noexcept {
    const auto* participant = application_voice_participant_at(x, y, nullptr, nullptr);
    if (!participant)
        return false;
    const u64 guild_id = participant->guild_id, user_id = participant->user_id;
    ensure_loaded();
    const bool self = user_id == g_app.account_ui->chat_state.self_user_id;
    const auto audio = voice_preferences_user(voice.preferences, user_id);
    // Local mute and the volume slider lead the shared user menu.
    PopupItem mute{utf8_literal("Mute"), kMenuMute,
                   static_cast<u16>(popup_item_enabled | (audio.muted ? popup_item_checked : 0)), 0};
    format_label(audio.gain_percent);
    UiPopupFeedback feedback{{voice.menu_label, voice.menu_label_bytes},
                             {64, 12},
                             false,
                             kVolumeSteps,
                             audio.gain_percent};
    if (!application_user_menu_open(user_id, anchor, guild_id ? guild_id : 0,
                                    guild_id ? UserMenuSource::voice : UserMenuSource::private_channel,
                                    self ? ConstSlice<PopupItem>{} : ConstSlice<PopupItem>{&mute, 1},
                                    self ? nullptr : &feedback))
        return false;
    voice.menu_user = self ? 0 : user_id;
    voice.menu_dragging = false;
    return true;
}

bool application_voice_menu_action(u32 action) noexcept {
    return action == kMenuMute;
}

bool application_voice_menu_activate(u32 action) noexcept {
    if (action == kMenuMute && menu_open()) {
        const auto audio = voice_preferences_user(voice.preferences, voice.menu_user);
        set_user(voice.menu_user, audio.gain_percent, !audio.muted);
    }
    voice.menu_user = 0;
    return dismiss_popup();
}

bool application_voice_menu_pointer(float x, float y, bool press) noexcept {
    if (!menu_open()) {
        voice.menu_dragging = false;
        return false;
    }
    const UiRect track = popup_feedback_slider(g_app.popup_scene);
    if (!press && voice.menu_dragging && !(GetKeyState(VK_LBUTTON) & 0x8000)) {
        voice.menu_dragging = false;
        ReleaseCapture();
        return true;
    }
    if (press ? !ui_rect_contains({track.left - 6, track.top - 6, track.right + 6, track.bottom + 6}, x, y)
              : !voice.menu_dragging)
        return false;
    if (press) {
        voice.menu_dragging = true;
        SetCapture(g_app.window);
    }
    const auto g = ui_slider_geometry(ui_settings_tokens(application_theme()), track, kVolumeSteps, 0, false);
    const u32 gain = ui_slider_index_at(g, kVolumeSteps, x);
    if (popup_scene_set_feedback_slider(&g_app.popup_scene, static_cast<u16>(gain))) {
        const auto audio = voice_preferences_user(voice.preferences, voice.menu_user);
        set_user(voice.menu_user, gain, audio.muted);
        format_label(gain);
        popup_scene_set_feedback(&g_app.popup_scene,
                                 {voice.menu_label, voice.menu_label_bytes});
        refresh_popup_text();
        application_repaint();
    }
    return true;
}

VoicePreferences& application_voice_preferences() noexcept {
    ensure_loaded();
    return voice.preferences;
}

void application_voice_settings_changed(bool persist) noexcept {
    voice.dirty = true;
    push();
    if (persist) {
        voice.save_pending = false;
        save();
    } else
        voice.save_pending = true; // Slider drag: saved once the button is released.
}

void application_voice_set_monitor(bool enabled) noexcept {
    if (voice.monitor == enabled)
        return;
    voice.monitor = enabled;
    voice.dirty = true;
    voice.status_at_ms = 0;
    push();
}

VoiceAudioStatus application_voice_status() noexcept {
    return voice.status;
}
} // namespace maicord::app_detail
