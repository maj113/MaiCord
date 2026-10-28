#include "application_stream.hpp"
#include "maicord/core/memory.hpp"
#include "application_account_presentation.hpp"
#include "application_call.hpp"
#include "application_composer.hpp"
#include "application_cursor.hpp"
#include "application_message_interaction.hpp"
#include "application_settings.hpp"
#include "application_shell_input.hpp"
#include "application_state.hpp"
#include "application_voice.hpp"
#include "maicord/app/gateway_stream_session.hpp"
#include "maicord/app/stream_preview_cache.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/ui/call_scene.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/cursor.hpp"
#include "maicord/ui/stream_card_scene.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/media/asset.hpp"

namespace maicord::app_detail {
namespace {
// Hover intent before the card opens, and grace while the pointer crosses
// the gap between the row and the card.
constexpr u64 kHoverOpenMs = 300, kHoverCloseMs = 220;
// A join requested for a watch gets this long to reach the channel.
constexpr u64 kJoinGraceMs = 15'000;
// "The stream has ended" stays briefly, then the tile leaves the grid.
constexpr u64 kEndedLingerMs = 8'000;
constexpr u32 kMenuFullscreen = 0x7a10'0001, kMenuStop = 0x7a10'0002;
constexpr u16 kVolumeSteps = kGatewayStreamVolumeMaximum + 1; // 0..200 %.

struct StreamUi {
    StreamPreviewCache previews;
    GatewayClientStreamSnapshot snapshot;
    StreamWatchView view;
    GatewayStreamKey requested, pending;
    u64 requested_ms, pending_ms, ended_ms;
    // Preview card.
    UiStreamCardScene card;
    GatewayStreamKey hover_key, card_key, press_key;
    UiRect hover_anchor;
    u64 hover_since_ms, leave_ms;
    bool card_pressed, captured;
    // Volume menu.
    u64 menu_tile;
    bool menu_dragging;
    char menu_label[48];
    u32 menu_label_bytes;
    u32 volume;
    u32 preview_scratch[512];
    char preview_url[kGatewayStreamPreviewMaximumUrlBytes];
};
// Zero-filled in the PE (.bss), as in application_call.cpp: the card entrance
// is reset with its scope before it ticks, so its stored style is unread.
StreamUi stream{.card = {.entrance = {.style = {0, UiMotionCurve::linear}}}};

u64 now_ms() noexcept {
    return clock_monotonic_milliseconds();
}
const GatewayClientVoiceSnapshot* session() noexcept {
    return g_app.account_ui ? &g_app.account_ui->voice_session : nullptr;
}
bool in_channel(u64 channel_id) noexcept {
    const auto* s = session();
    return s && channel_id && s->channel_id == channel_id && s->phase != VoiceSetupPhase::idle;
}
bool valid(const GatewayStreamKey& key) noexcept {
    return key.type != GatewayStreamType::none && gateway_stream_key_valid(key);
}
bool watching(const StreamWatchView& view) noexcept {
    return view.state == StreamWatchState::connecting || view.state == StreamWatchState::live ||
           view.state == StreamWatchState::reconnecting;
}
bool same_view(const StreamWatchView& a, const StreamWatchView& b) noexcept {
    return a.key == b.key && a.state == b.state && a.notice == b.notice && a.viewers == b.viewers &&
           a.video_ssrc == b.video_ssrc && a.volume_percent == b.volume_percent && a.paused == b.paused;
}
bool submit(const GatewayStreamKey& key) noexcept {
    auto& host = g_app.account;
    if (!host.instance || !host.api.stream_watch || !host.api.stream_watch(host.instance, &key))
        return false;
    // Switching streams asks nothing (as in the official client): the worker replaces the old one.
    stream.requested = key;
    stream.requested_ms = now_ms();
    stream.ended_ms = 0;
    reset_value(&stream.pending);
    application_repaint();
    return true;
}
// Card blockers: sign-in, settings, popups and modal surfaces own the pointer.
bool card_blocked() noexcept {
    return !g_app.account_ui || g_app.login.visible || g_app.popup_scene.active || g_app.text_form ||
           g_app.image_viewer.scene.active || application_settings_active() ||
           (g_app.renderer.profile_scene && g_app.renderer.profile_scene->active);
}
// keep_hover: the pointer moved on to another live user, whose card follows.
void close_card(bool keep_hover = false) noexcept {
    if (!keep_hover) {
        reset_value(&stream.hover_key);
        stream.hover_since_ms = 0;
    }
    reset_value(&stream.card_key);
    stream.leave_ms = 0;
    stream.card_pressed = false;
    if (stream.card.active) {
        ui_stream_card_close(&stream.card);
        application_repaint();
    }
    if (g_app.renderer.stream_card_scene == &stream.card)
        g_app.renderer.stream_card_scene = nullptr;
}
const VoiceState* live_state(const GatewayStreamKey& key) noexcept {
    if (!g_app.account_ui || !valid(key))
        return nullptr;
    const auto* state = voice_state_store_find(g_app.account_ui->voice_states, key.guild_id, key.owner_id);
    return state && state->channel_id == key.channel_id && voice_state_live(*state) ? state : nullptr;
}
// The watched stream's owner left the channel or stopped streaming, as the
// channel's known voice states say; STREAM_DELETE can lag behind or be lost.
bool owner_gone(const GatewayStreamKey& key) noexcept {
    return valid(key) && g_app.account_ui &&
           voice_state_store_channel_count(g_app.account_ui->voice_states, key.channel_id) && !live_state(key);
}
// (Re)projects the card for card_key; closes it when the stream went away.
void project_card(u64 now) noexcept {
    const auto* state = live_state(stream.card_key);
    if (!state) {
        close_card();
        return;
    }
    auto& ui = *g_app.account_ui;
    const auto* user = user_store_find(ui.user_store, stream.card_key.owner_id);
    const auto* channel = chat_state_store_find_channel(ui.chat_state, stream.card_key.channel_id);
    bool loading = false;
    const MediaAssetKey preview = application_stream_preview(stream.card_key, &loading);
    UiStreamCardContent content{};
    content.name = user ? user_store_name(ui.user_store, *user) : utf8_literal("Unknown user");
    content.channel = channel ? chat_state_store_channel_name(ui.chat_state, *channel) : Utf8View{};
    content.preview = preview;
    content.preview_state = media_asset_key_valid(preview) ? UiStreamCardPreview::ready
                            : loading                     ? UiStreamCardPreview::loading
                                                          : UiStreamCardPreview::unavailable;
    content.watching = stream.view.key == stream.card_key && watching(stream.view);
    const u32 revision = stream.card.revision;
    const u64 identity = gateway_stream_video_user(stream.card_key.owner_id) ^ stream.card_key.channel_id;
    if (!ui_stream_card_show(&stream.card, identity, content, stream.hover_anchor, application_viewport_dips(), now,
                             g_app.preferences.reduced_motion)) {
        close_card();
        return;
    }
    g_app.renderer.stream_card_scene = &stream.card;
    if (revision != stream.card.revision)
        application_repaint();
}
void card_pump(u64 now) noexcept {
    if (card_blocked()) {
        close_card();
        return;
    }
    if (valid(stream.hover_key) && !(stream.hover_key == stream.card_key) && now >= stream.hover_since_ms + kHoverOpenMs) {
        stream.card_key = stream.hover_key;
        stream.leave_ms = 0;
    }
    if (!stream.card.active && !valid(stream.card_key))
        return;
    if (stream.leave_ms && now >= stream.leave_ms + kHoverCloseMs && !stream.card_pressed) {
        close_card(true);
        return;
    }
    if (valid(stream.card_key))
        project_card(now);
    if (stream.card.active && ui_stream_card_tick(&stream.card, now, g_app.preferences.reduced_motion))
        application_repaint();
}
void format_label(u32 percent) noexcept {
    TextBank label{stream.menu_label, sizeof(stream.menu_label), 0};
    label.add("Stream volume \xC2\xB7 ");
    label.number(percent);
    label.add("%");
    stream.menu_label_bytes = label.used;
}
bool menu_open() noexcept {
    return stream.menu_tile && g_app.popup_scene.active && g_app.popup_scene.owner_id == stream.menu_tile &&
           g_app.popup_owner_kind == PopupOwnerKind::stream && g_app.popup_scene.feedback.slider_count;
}
void release_press() noexcept {
    stream.card_pressed = false;
    reset_value(&stream.press_key);
    ui_stream_card_set_pressed(&stream.card, false);
    if (stream.captured) {
        stream.captured = false;
        if (GetCapture() == g_app.window)
            ReleaseCapture();
    }
}
// The live badge (or row) of a streaming participant under (x, y).
bool live_at(float x, float y, GatewayStreamKey* key, UiRect* row, UiRect* badge) noexcept {
    if (const auto* call = g_app.renderer.call_scene; call && ui_call_scene_contains(*call, x, y))
        return false; // The call view covers the channel list (fullscreen).
    UiRect r{}, live{};
    const auto* state = application_voice_participant_at(x, y, &r, &live);
    if (!state || !voice_state_live(*state) || state->user_id == g_app.account_ui->chat_state.self_user_id)
        return false;
    *key = gateway_stream_key_for(state->guild_id, state->channel_id, state->user_id);
    if (row)
        *row = r;
    if (badge)
        *badge = live;
    return valid(*key);
}
} // namespace

void application_stream_watch(const GatewayStreamKey& key) noexcept {
    if (!g_app.account_ui || !valid(key) || key.owner_id == g_app.account_ui->chat_state.self_user_id)
        return;
    if (in_channel(key.channel_id)) {
        if (!submit(key))
            return;
    } else {
        // Like the official client, watching from outside joins the channel.
        stream.pending = key;
        stream.pending_ms = now_ms();
        if (!application_voice_join(key.guild_id, key.channel_id)) {
            reset_value(&stream.pending);
            return;
        }
    }
    close_card();
    application_call_open(key.channel_id, application_stream_tile(key.owner_id));
    application_stream_pump();
}

void application_stream_stop() noexcept {
    auto& host = g_app.account;
    if (valid(stream.requested) && host.instance && host.api.stream_watch)
        host.api.stream_watch(host.instance, nullptr);
    stream.requested = stream.pending = {};
    stream.ended_ms = 0;
    reset_value(&stream.view);
    application_repaint();
}

StreamWatchView application_stream_view() noexcept {
    return stream.view;
}

u64 application_stream_tile(u64 owner_id) noexcept {
    return gateway_stream_video_user(owner_id);
}

MediaAssetKey application_stream_preview(const GatewayStreamKey& key, bool* loading) noexcept {
    if (loading)
        *loading = false;
    const auto* entry = stream_preview_want(&stream.previews, key, now_ms());
    if (!entry)
        return {};
    if (loading)
        *loading = !stream_preview_ready(*entry) && !entry->attempts;
    return stream_preview_ready(*entry) ? stream_preview_media_key(*entry) : MediaAssetKey{};
}

void application_stream_set_volume(u32 percent) noexcept {
    auto& host = g_app.account;
    percent = percent > kGatewayStreamVolumeMaximum ? kGatewayStreamVolumeMaximum : percent;
    if (host.instance && host.api.stream_volume && host.api.stream_volume(host.instance, percent))
        stream.volume = percent;
}

bool application_stream_menu_open(u64 tile_id, const UiRect& anchor, bool fullscreen) noexcept {
    if (!watching(stream.view) || application_stream_tile(stream.view.key.owner_id) != tile_id)
        return false;
    const u32 volume = stream.volume ? stream.volume : stream.view.volume_percent;
    const PopupItem items[]{
        {fullscreen ? utf8_literal("Exit full screen") : utf8_literal("Full screen"), kMenuFullscreen,
         popup_item_enabled, 0},
        {utf8_literal("Stop watching"), kMenuStop,
         static_cast<u16>(popup_item_enabled | popup_item_danger | popup_item_separator_before), 0},
    };
    format_label(volume);
    const UiPopupFeedback feedback{{stream.menu_label, stream.menu_label_bytes},
                                   {64, 12},
                                   false,
                                   kVolumeSteps,
                                   static_cast<u16>(volume)};
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, tile_id, {items, 2}, anchor,
                          application_viewport_dips(), 220.0f, false, 0, 0, 0, &feedback)
             .ok())
        return false;
    stream.menu_tile = tile_id;
    stream.menu_dragging = false;
    close_card();
    set_popup_owner(PopupOwnerKind::stream);
    return refresh_popup_text();
}

bool application_stream_menu_action(u32 action) noexcept {
    return (action == kMenuFullscreen || action == kMenuStop) && g_app.popup_owner_kind == PopupOwnerKind::stream;
}

bool application_stream_menu_activate(u32 action) noexcept {
    const u64 tile = stream.menu_tile;
    stream.menu_tile = 0;
    const bool dismissed = dismiss_popup();
    if (action == kMenuFullscreen)
        application_call_toggle_tile_fullscreen(tile);
    else if (action == kMenuStop)
        application_stream_stop();
    return dismissed;
}

bool application_stream_menu_pointer(float x, float y, bool press) noexcept {
    if (!menu_open()) {
        stream.menu_dragging = false;
        return false;
    }
    const UiRect track = popup_feedback_slider(g_app.popup_scene);
    if (!press && stream.menu_dragging && !(GetKeyState(VK_LBUTTON) & 0x8000)) {
        stream.menu_dragging = false;
        ReleaseCapture();
        return true;
    }
    if (press ? !ui_rect_contains({track.left - 6, track.top - 6, track.right + 6, track.bottom + 6}, x, y)
              : !stream.menu_dragging)
        return false;
    if (press) {
        stream.menu_dragging = true;
        SetCapture(g_app.window);
    }
    const auto g = ui_slider_geometry(ui_settings_layout_tokens(application_theme().styles), track, kVolumeSteps, 0, false);
    const u32 percent = ui_slider_index_at(g, kVolumeSteps, x);
    if (popup_scene_set_feedback_slider(&g_app.popup_scene, static_cast<u16>(percent))) {
        application_stream_set_volume(percent);
        format_label(percent);
        popup_scene_set_feedback(&g_app.popup_scene, {stream.menu_label, stream.menu_label_bytes});
        refresh_popup_text();
        application_repaint();
    }
    return true;
}

void application_stream_pump() noexcept {
    if (!g_app.account_ui) {
        close_card();
        return;
    }
    const u64 now = now_ms();
    auto& host = g_app.account;
    GatewayClientStreamSnapshot snapshot{};
    if (host.instance && host.api.stream_snapshot && host.api.stream_snapshot(host.instance, &snapshot))
        stream.snapshot = snapshot;
    // A watch waiting for its join.
    if (valid(stream.pending)) {
        if (in_channel(stream.pending.channel_id))
            submit(stream.pending);
        if (valid(stream.pending) && now - stream.pending_ms >= kJoinGraceMs)
            reset_value(&stream.pending);
    }
    // Leaving (or moving out of) the channel stops watching; the worker has
    // already ended the stream there.
    if (valid(stream.requested) && !in_channel(stream.requested.channel_id))
        reset_value(&stream.requested);
    auto view = stream_watch_view(stream.snapshot, stream.requested, stream.requested_ms, now);
    if (!valid(stream.requested) && valid(stream.pending)) {
        view.key = stream.pending;
        view.state = StreamWatchState::connecting;
    }
    const bool gone = owner_gone(stream.requested);
    if (gone && view.state != StreamWatchState::ended) {
        // The streamer left before STREAM_DELETE: stop watching, drop the tile.
        if (host.instance && host.api.stream_watch)
            host.api.stream_watch(host.instance, nullptr);
        reset_value(&stream.requested);
        stream.ended_ms = 0;
        reset_value(&view);
    } else if (view.state == StreamWatchState::ended) {
        if (!stream.ended_ms)
            stream.ended_ms = now;
        // The streamer stopping is not an error: the tile leaves the grid. Any
        // other notice lingers too once its streamer has left the channel.
        if ((gone || view.notice == StreamWatchNotice::ended) && now - stream.ended_ms >= kEndedLingerMs) {
            reset_value(&stream.requested);
            reset_value(&view);
        }
    } else
        stream.ended_ms = 0;
    if (!stream.volume && stream.snapshot.volume_percent)
        stream.volume = stream.snapshot.volume_percent;
    if (!same_view(view, stream.view)) {
        stream.view = view;
        application_repaint();
    }
    card_pump(now);
}

u64 application_stream_next_wake() noexcept {
    u64 wake = 0;
    const auto earliest = [&](u64 at) noexcept {
        if (at && (!wake || at < wake))
            wake = at;
    };
    if (valid(stream.hover_key) && !(stream.hover_key == stream.card_key))
        earliest(stream.hover_since_ms + kHoverOpenMs);
    if (stream.card.active && stream.leave_ms)
        earliest(stream.leave_ms + kHoverCloseMs);
    if (stream.card.active)
        earliest(ui_stream_card_next_wake(stream.card, now_ms(), g_app.preferences.reduced_motion));
    if (stream.ended_ms && stream.view.notice == StreamWatchNotice::ended)
        earliest(stream.ended_ms + kEndedLingerMs);
    if (valid(stream.pending))
        earliest(stream.pending_ms + kJoinGraceMs);
    // Monotonic deadlines; the call pump compares them with the same clock.
    return wake;
}

bool application_stream_window_message(UINT message, WPARAM, LPARAM lparam) noexcept {
    if (!g_app.account_ui)
        return false;
    switch (message) {
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
    case WM_KILLFOCUS:
        if (stream.captured && (message != WM_CAPTURECHANGED || reinterpret_cast<HWND>(lparam) != g_app.window)) {
            stream.captured = false;
            release_press();
        }
        return false;
    case WM_MOUSELEAVE:
        reset_value(&stream.hover_key);
        if (stream.card.active && !stream.leave_ms)
            stream.leave_ms = now_ms();
        if (ui_stream_card_set_hover(&stream.card, -1e7f, -1e7f))
            application_repaint();
        return false;
    case WM_MOUSEWHEEL:
        // The channel list scrolls under the card: its anchor is gone.
        close_card();
        return false;
    default:
        break;
    }
    if (message != WM_MOUSEMOVE && message != WM_LBUTTONDOWN && message != WM_LBUTTONUP &&
        message != WM_LBUTTONDBLCLK && message != WM_SETCURSOR && message != WM_RBUTTONDOWN &&
        message != WM_RBUTTONUP)
        return false;
    if (card_blocked()) {
        if (stream.card.active)
            close_card();
        return false;
    }
    UiPoint point{};
    if (message == WM_SETCURSOR) {
        if (LOWORD(lparam) != HTCLIENT || !application_cursor_point(g_app.window, g_app.renderer.dpi, lparam, &point))
            return false;
    } else
        point = {client_dip_x(lparam), client_dip_y(lparam)};
    const bool over_card = ui_stream_card_contains(stream.card, point.x, point.y);
    GatewayStreamKey key{};
    UiRect row{}, badge{};
    const bool live = !over_card && live_at(point.x, point.y, &key, &row, &badge);
    switch (message) {
    case WM_MOUSEMOVE: {
        const u64 now = now_ms();
        if (ui_stream_card_set_hover(&stream.card, point.x, point.y))
            application_repaint();
        if (over_card) {
            stream.leave_ms = 0;
            return true;
        }
        if (live) {
            if (!(key == stream.hover_key)) {
                stream.hover_key = key;
                stream.hover_since_ms = now;
            }
            stream.hover_anchor = row;
            if (key == stream.card_key)
                stream.leave_ms = 0;
            else if (stream.card.active && !stream.leave_ms)
                stream.leave_ms = now; // Another streamer: this card yields.
            application_request_frame();
        } else {
            reset_value(&stream.hover_key);
            if (stream.card.active && !stream.leave_ms) {
                stream.leave_ms = now;
                application_request_frame();
            }
        }
        return stream.captured;
    }
    case WM_SETCURSOR:
        if (over_card) {
            application_set_cursor(ui_action_cursor(ui_stream_card_hit(stream.card, point.x, point.y) ==
                                                    UiStreamCardControl::watch));
            return true;
        }
        if (live && ui_rect_contains(badge, point.x, point.y)) {
            application_set_cursor(ui_action_cursor(true));
            return true;
        }
        return false;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (over_card) {
            if (ui_stream_card_hit(stream.card, point.x, point.y) == UiStreamCardControl::watch) {
                stream.card_pressed = true;
                ui_stream_card_set_pressed(&stream.card, true);
                SetCapture(g_app.window);
                stream.captured = true;
                application_repaint();
            }
            return true;
        }
        if (live && ui_rect_contains(badge, point.x, point.y)) {
            stream.press_key = key;
            SetCapture(g_app.window);
            stream.captured = true;
            return true;
        }
        return false;
    case WM_LBUTTONUP: {
        const bool card_press = stream.card_pressed;
        const GatewayStreamKey badge_press = stream.press_key;
        release_press();
        if (card_press) {
            if (over_card && ui_stream_card_hit(stream.card, point.x, point.y) == UiStreamCardControl::watch) {
                const GatewayStreamKey target = stream.card_key;
                if (stream.view.key == target && watching(stream.view)) {
                    close_card();
                    application_call_open(target.channel_id, application_stream_tile(target.owner_id));
                } else
                    application_stream_watch(target);
            }
            application_repaint();
            return true;
        }
        if (valid(badge_press)) {
            if (live && key == badge_press && ui_rect_contains(badge, point.x, point.y))
                application_stream_watch(badge_press);
            return true;
        }
        return over_card;
    }
    default: // Right clicks on the card stay on it.
        return over_card;
    }
}

UiStreamCardScene* application_stream_card_spinner_scene() noexcept {
    return stream.card.active && ui_stream_card_spinner_visible(stream.card) ? &stream.card : nullptr;
}

bool application_stream_preview_pending(u64 now) noexcept {
    return g_app.account.instance && g_app.account.api.stream_preview &&
           stream_preview_next_due(stream.previews, now) != nullptr;
}

bool application_stream_preview_submit(u64 request_id, u64 now) noexcept {
    const auto* due = stream_preview_next_due(stream.previews, now);
    auto& host = g_app.account;
    if (!due || !host.instance || !host.api.stream_preview)
        return false;
    const GatewayStreamKey key = due->key;
    const auto submitted = host.api.stream_preview(host.instance, &key, request_id);
    if (submitted.ok())
        return stream_preview_submitted(&stream.previews, key, request_id, now);
    if (submitted.error == RestClientSubmitError::busy)
        stream_preview_deferred(&stream.previews, key, now + 250);
    else {
        // Refused outright: back off like a failed fetch.
        stream_preview_submitted(&stream.previews, key, request_id, now);
        stream_preview_complete(&stream.previews, request_id, 0, {}, 0, now);
    }
    return false;
}

bool application_stream_preview_response(const RestClientResponse& response, Utf8View body) noexcept {
    u32 bytes = 0;
    if (response.error == RestClientError::none && response.status_code == 200 && body.bytes)
        bytes = gateway_stream_preview_url(body, {stream.preview_scratch, 512},
                                           {stream.preview_url, sizeof(stream.preview_url)});
    const u16 status = response.error == RestClientError::none
                           ? (bytes || response.status_code != 200 ? response.status_code : u16{0})
                           : u16{0};
    const auto* entry = stream_preview_complete(&stream.previews, response.request_id, status,
                                                {stream.preview_url, bytes}, response.retry_at_ms, now_ms());
    clear_bytes(stream.preview_url, sizeof(stream.preview_url));
    if (entry)
        application_repaint();
    return true;
}

u64 application_stream_preview_next_wake() noexcept {
    return stream_preview_next_wake(stream.previews, now_ms());
}

bool application_stream_preview_key_current(const MediaAssetKey& key) noexcept {
    if (key.kind != MediaAssetKind::embed || !media_asset_key_valid(key))
        return false;
    if (stream.card.active && media_asset_key_equal(stream.card.preview, key))
        return true;
    if (const auto* call = g_app.renderer.call_scene; call && call->active)
        for (u32 i = 0; i < call->tile_count; ++i) {
            const auto& tile = call->tiles[i];
            if (tile.participant < call->participant_count &&
                media_asset_key_equal(call->participants[tile.participant].preview, key) &&
                ui_call_scene_tile_visible(*call, i))
                return true;
        }
    return false;
}

u32 application_stream_preview_media(StreamPreviewMedia* output, u32 capacity) noexcept {
    u32 count = 0;
    for (const auto& entry : stream.previews.entries) {
        if (count == capacity || !stream_preview_ready(entry))
            continue;
        const MediaAssetKey key = stream_preview_media_key(entry);
        if (application_stream_preview_key_current(key))
            output[count++] = {{entry.url, entry.url_bytes}, key};
    }
    return count;
}

void application_stream_release() noexcept {
    close_card();
    release_press();
    stream_preview_clear(&stream.previews);
    reset_value(&stream.snapshot);
    reset_value(&stream.view);
    stream.requested = stream.pending = {};
    stream.menu_tile = 0;
    stream.volume = 0;
}
} // namespace maicord::app_detail
