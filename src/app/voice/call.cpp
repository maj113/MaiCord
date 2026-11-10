#include "call.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "../shell/overlay_input.hpp"
#include "../shell/window_state.hpp"
#include "../shell/content_input.hpp"
#include "../account/account_presentation.hpp"
#include "../shell/cursor.hpp"
#include "../account/friends.hpp"
#include "../chat/messages/pinned_messages.hpp"
#include "../chat/search/search.hpp"
#include "../settings/settings.hpp"
#include "../shell/input.hpp"
#include "../shell/window_events.hpp"
#include "../shell/shortcuts.hpp"
#include "stage.hpp"
#include "soundboard.hpp"
#include "../shell/state.hpp"
#include "stream.hpp"
#include "../chat/private_header.hpp"
#include "go_live.hpp"
#include "../chat/threads/browser.hpp"
#include "voice.hpp"
#include "../shell/window.hpp"
#include "maicord/app/voice/call_video_presenter.hpp"
#include "maicord/app/voice/call_view_lifecycle.hpp"
#include "maicord/app/gateway/stream_session.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/voice/call_scene.hpp"
#include "maicord/ui/core/cursor.hpp"
#include "maicord/ui/chat/private_channel_title.hpp"
#include "../account/account_ui_runtime.hpp"

#include "maicord/platform/window/key.hpp"
#include "maicord/media/core/asset.hpp"

namespace maicord::app_detail {
namespace {
struct CallView {
    UiCallScene scene;
    UiCallParticipant participants[kUiCallMaximumTiles];
    CallVideoPresenter video;
    GatewayClientVoiceVideoSnapshot snapshot;
    char title[kUiCallTitleBytes];
    u64 channel_id, focus_user, opened_ms, next_poll_ms, video_generation, pressed_user;
    u32 intent_revision, title_bytes;
    UiCallControl pressed;
    bool open, fullscreen, window_fullscreen, snapshot_valid, captured;
    // Navigation in the handler that opened the view (accepting a DM call
    // navigates to the DM) is absorbed by the first synchronize.
    bool intent_pending;
    // Text in voice: the channel chat shares the content column; split_width
    // is the call's share the shell layout reserved for it (zero: no split).
    bool chat_open;
    float split_width;
    // The channel search field this frame's search pass placed in the header.
    float search_width;
    CallViewLifecycle lifecycle;
};
// Zero-filled in the PE (.bss): the morph's nonzero default style would put
// this whole ~39 KB view into initialized .data. A morph is reset with its
// scope before it can tick, so its stored style is never read before that.
CallView view{.scene = {.morph = {.style = {0, UiMotionCurve::linear}}}};

bool modal_open() noexcept {
    return g_app.login.visible || g_app.popup_scene.active || g_app.text_form || g_app.reaction_viewer.active ||
           g_app.image_viewer.scene.active ||
           (g_app.renderer.profile_scene && g_app.renderer.profile_scene->active) ||
           application_friends_requests_active() || application_settings_active();
}
bool scene_visible() noexcept {
    return view.open && view.scene.active && g_app.renderer.call_scene == &view.scene && !g_app.login.visible &&
           !application_settings_active();
}
void set_window_fullscreen(bool on) noexcept {
    if (on != view.window_fullscreen && application_window_set_fullscreen(on))
        view.window_fullscreen = on;
}
void set_fullscreen(bool on) noexcept {
    if (view.fullscreen == on)
        return;
    view.fullscreen = on;
    set_window_fullscreen(on);
    application_repaint();
}
void release_pointer() noexcept {
    view.pressed = UiCallControl::none;
    view.pressed_user = 0;
    ui_call_scene_set_pressed(&view.scene, UiCallControl::none);
    if (view.captured) {
        view.captured = false;
        application_release_capture();
    }
}
Utf8View call_title(u64 channel_id) noexcept {
    auto& ui = *g_app.account_ui;
    const auto* channel = chat_state_store_find_channel(ui.chat_state, channel_id);
    if (!channel)
        return utf8_literal("Voice call");
    if (channel_is_private(channel->type)) {
        const Utf8View title = private_channel_title(ui.chat_state, &ui.user_store, *channel, {view.title, sizeof(view.title)});
        return title.bytes ? title : utf8_literal("Call");
    }
    return chat_state_store_channel_name(ui.chat_state, *channel);
}
const GatewayVoiceVideoUser* video_user(u64 user_id) noexcept {
    for (u32 i = 0; view.snapshot_valid && i < view.snapshot.count && i < kGatewayVoiceVideoUsers; ++i)
        if (view.snapshot.users[i].user_id == user_id && view.snapshot.users[i].ssrc)
            return &view.snapshot.users[i];
    return nullptr;
}
// The decoded picture size of a tile whose stream still carries `ssrc`.
void attach_picture(UiCallParticipant& p, u32 ssrc) noexcept {
    if (const auto* picture = call_video_presenter_find(view.video, p.user_id);
        picture && picture->ready && picture->ssrc == ssrc) {
        p.video_width = picture->display_width;
        p.video_height = picture->display_height;
    }
}
void add_participant(u32* count, u64 user_id, u32 flags, bool self) noexcept {
    if (*count >= kUiCallMaximumTiles || !user_id)
        return;
    auto& ui = *g_app.account_ui;
    const auto* user = user_store_find(ui.user_store, user_id);
    // A server call names members by their nickname, as the sidebar does.
    const auto* channel = chat_state_store_find_channel(ui.chat_state, view.channel_id);
    const auto* member =
        channel && channel->guild_id ? guild_member_store_find(ui.guild_member_store, channel->guild_id, user_id)
                                     : nullptr;
    const Utf8View nickname = member ? guild_member_store_nickname(*member) : Utf8View{};
    UiCallParticipant p{};
    p.user_id = user_id;
    p.name = nickname.bytes ? nickname : user ? user_store_name(ui.user_store, *user) : Utf8View{};
    // The renderer draws custom avatars over the placeholder; others show initials.
    p.has_avatar = user && user->avatar_hash_bytes && user_avatar_variant(user_store_avatar_hash(*user)) != 0;
    p.speaking = (flags & voice_speaking) != 0;
    p.muted = (flags & (voice_mute | voice_self_mute)) != 0;
    p.deafened = (flags & (voice_deaf | voice_self_deaf)) != 0;
    p.live = voice_state_live(VoiceState{0, 0, user_id, flags});
    p.self = self;
    p.effect = application_soundboard_effect(user_id);
    if (const auto* video = video_user(user_id)) {
        p.video_ssrc = video->ssrc;
        attach_picture(p, video->ssrc);
    }
    view.participants[(*count)++] = p;
}
UiCallStream tile_stream(StreamWatchState state) noexcept {
    switch (state) {
    case StreamWatchState::connecting:
        return UiCallStream::connecting;
    case StreamWatchState::live:
        return UiCallStream::live;
    case StreamWatchState::reconnecting:
        return UiCallStream::reconnecting;
    case StreamWatchState::ended:
        return UiCallStream::ended;
    default:
        return UiCallStream::none;
    }
}
Utf8View user_name(u64 user_id) noexcept {
    auto& ui = *g_app.account_ui;
    const auto* user = user_store_find(ui.user_store, user_id);
    return user ? user_store_name(ui.user_store, *user) : Utf8View{};
}
// The voice server's announced cameras (op 12/13), in snapshot order.
u32 camera_targets(const GatewayClientVoiceVideoSnapshot& snapshot, CallVideoTarget* output) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < snapshot.count && i < kGatewayVoiceVideoUsers; ++i)
        output[count++] = {snapshot.users[i].user_id, snapshot.users[i].ssrc, 0, false};
    return count;
}
// Go Live tiles lead the grid: the watched stream (with its state), then
// every other live user's stream offering "Watch stream" over its preview.
u32 project_streams(u64 channel_id) noexcept {
    auto& ui = *g_app.account_ui;
    const u64 self = ui.chat_state.self_user_id;
    u32 count = 0;
    // Your own stream leads, with its self-preview, while it starts or runs.
    if (UiCallParticipant own{}; application_go_live_tile(channel_id, &own)) {
        if (own.stream == UiCallStream::live || own.stream == UiCallStream::reconnecting) {
            own.video_ssrc = kGoLivePreviewSsrc;
            attach_picture(own, kGoLivePreviewSsrc);
        }
        view.participants[count++] = own;
    }
    const auto& watched = application_stream_view();
    u64 watched_owner = 0;
    // A stream whose owner the channel's known voice states no longer list
    // live (left, moved, stopped) has no tile, even before STREAM_DELETE.
    const auto* owner = voice_state_store_find(ui.voice_states, watched.key.guild_id, watched.key.owner_id);
    const bool owner_live = !voice_state_store_channel_count(ui.voice_states, channel_id) ||
                            (owner && owner->channel_id == channel_id && voice_state_live(*owner));
    if (watched.state != StreamWatchState::none && watched.key.channel_id == channel_id &&
        (owner_live || watched.state == StreamWatchState::ended)) {
        watched_owner = watched.key.owner_id;
        UiCallParticipant p{};
        p.user_id = application_stream_tile(watched_owner);
        p.name = user_name(watched_owner);
        p.stream = tile_stream(watched.state);
        p.notice = static_cast<UiCallStreamNotice>(watched.notice);
        p.paused = watched.paused;
        p.viewers = watched.viewers;
        if (p.stream == UiCallStream::live || p.stream == UiCallStream::reconnecting) {
            p.video_ssrc = watched.video_ssrc;
            attach_picture(p, watched.video_ssrc);
        }
        if (p.stream != UiCallStream::ended && !(p.video_width && p.video_height))
            p.preview = application_stream_preview(watched.key);
        view.participants[count++] = p;
    }
    const u32 states = voice_state_store_channel_count(ui.voice_states, channel_id);
    for (u32 i = 0; i < states && count < kUiCallMaximumTiles / 2; ++i) {
        const auto* state = voice_state_store_channel_at(ui.voice_states, channel_id, i);
        if (!state)
            break;
        if (!voice_state_live(*state) || state->user_id == self || state->user_id == watched_owner)
            continue;
        UiCallParticipant p{};
        p.user_id = application_stream_tile(state->user_id);
        p.name = user_name(state->user_id);
        p.stream = UiCallStream::available;
        p.preview = application_stream_preview(gateway_stream_key_for(state->guild_id, channel_id, state->user_id));
        view.participants[count++] = p;
    }
    return count;
}
u32 project_participants(u64 channel_id) noexcept {
    auto& ui = *g_app.account_ui;
    const u64 self = ui.chat_state.self_user_id;
    const auto* s = application_voice_call();
    const bool joined = s && s->channel_id == channel_id;
    const u32 streams = project_streams(channel_id);
    u32 count = streams;
    bool self_seen = false;
    // A stage's tiles are its speakers; the audience is the stage band.
    const bool stage = application_stage_channel(channel_id);
    const u32 states = voice_state_store_channel_count(ui.voice_states, channel_id);
    for (u32 i = 0; i < states; ++i) {
        const auto* state = voice_state_store_channel_at(ui.voice_states, channel_id, i);
        if (!state)
            break;
        self_seen = self_seen || state->user_id == self;
        if (!stage || application_stage_speaker(state->flags))
            add_participant(&count, state->user_id, state->flags, state->user_id == self);
    }
    // A camera announced on the call gets a tile without a voice state only
    // while the channel's are unknown: a user who left stays announced until
    // the voice server's op 13 (call_video_unlisted_users).
    if (joined && view.snapshot_valid) {
        CallVideoTarget cameras[kGatewayVoiceVideoUsers];
        u64 unlisted[kGatewayVoiceVideoUsers];
        const u32 unknown = call_video_unlisted_users(ui.voice_states, channel_id,
                                                      {cameras, camera_targets(view.snapshot, cameras)},
                                                      {unlisted, kGatewayVoiceVideoUsers});
        for (u32 i = 0; i < unknown; ++i) {
            bool present = false;
            for (u32 j = 0; j < count && !present; ++j)
                present = view.participants[j].user_id == unlisted[i];
            if (!present)
                add_participant(&count, unlisted[i], 0, false);
        }
    }
    if (!self_seen && self && !stage) {
        // DM calls and joins in flight: the local user leads the grid.
        const auto& preferences = application_voice_preferences();
        u32 flags = (preferences.self_mute ? voice_self_mute : 0u) | (preferences.self_deaf ? voice_self_deaf : 0u);
        if (joined && (s->flags & gateway_voice_mute))
            flags |= voice_mute;
        if (joined && (s->flags & gateway_voice_deaf))
            flags |= voice_deaf;
        if (joined && application_voice_status().transmitting)
            flags |= voice_speaking;
        if (count == kUiCallMaximumTiles)
            --count;
        for (u32 i = count; i > streams; --i)
            view.participants[i] = view.participants[i - 1];
        u32 at = streams;
        add_participant(&at, self, flags, true);
        ++count;
    }
    return count;
}
UiRect call_viewport() noexcept {
    const UiRect window = application_viewport_dips();
    if (view.fullscreen)
        return window;
    const UiRect content = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::content);
    // With the chat beside it, the call takes the reserved left share and the
    // shell's content column (title, messages, composer) shows the chat.
    if (view.split_width > 0 && ui_rect_has_area(content))
        return {content.left - view.split_width, window.top, content.left, window.bottom};
    return {ui_rect_has_area(content) ? content.left : window.left, window.top, window.right, window.bottom};
}
// The channel's text chat can sit beside the call: a guild voice or stage
// channel whose messages this account may read.
bool chat_available(u64 channel_id) noexcept {
    const auto* channel = g_app.account_ui ? chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id)
                                           : nullptr;
    constexpr u32 read = channel_action_view | channel_action_read_history;
    return channel && channel->guild_id && channel_is_voice_chat(channel->type) && channel->permissions_resolved &&
           (channel->actions & read) == read && !(channel->flags & kChannelObfuscatedFlag);
}
bool chat_showing() noexcept {
    return view.chat_open && g_app.account_ui && g_app.account_ui->active_channel_id == view.channel_id &&
           g_app.account_ui->chat_state.selected_channel_id == view.channel_id;
}
void relayout() noexcept {
    synchronize_shell_layout();
    application_repaint();
}
void set_chat(bool open) noexcept {
    if (open) {
        if (!chat_available(view.channel_id))
            return;
        // Selecting the voice channel's chat is navigation the view absorbs.
        view.intent_pending = true;
        if (!application_navigate_known_message(view.channel_id, 0))
            return;
        view.intent_pending = true;
    }
    if (view.chat_open == open)
        return;
    view.chat_open = open;
    relayout();
}
// One pass of wants (and pictures when poll is set) against the current tiles.
CallVideoPumpResult synchronize_video(bool poll) noexcept {
    auto& host = g_app.account;
    const auto* s = application_voice_call();
    if (!host.instance || !s) {
        view.snapshot_valid = false;
        call_video_presenter_reset(&view.video);
        return {};
    }
    GatewayClientVoiceVideoSnapshot snapshot{};
    if (!account_module_voice_video_snapshot(host.instance, &snapshot))
        return {};
    if (!view.snapshot_valid || snapshot.generation != view.video_generation) {
        // A new voice connection starts with fresh sink wants.
        call_video_presenter_reset(&view.video);
        view.video_generation = snapshot.generation;
    }
    // A camera added or removed (op 12/13) changes the tiles even when no
    // picture was ready to report a change.
    CallVideoTarget before[kGatewayVoiceVideoUsers], after[kGatewayVoiceVideoUsers];
    const bool roster = !view.snapshot_valid ||
                        call_video_roster_changed({before, camera_targets(view.snapshot, before)},
                                                  {after, camera_targets(snapshot, after)});
    copy_value(&view.snapshot, snapshot);
    view.snapshot_valid = true;
    const bool shown = scene_visible() && view.channel_id == s->channel_id && g_app.window && !application_window_minimized();
    const float pixel_scale = static_cast<float>(g_app.renderer.dpi ? g_app.renderer.dpi : 96u) / 96.f;
    CallVideoTarget targets[kGatewayVoiceVideoUsers + 2];
    u32 count = 0;
    const auto add_target = [&](u64 user_id, u32 ssrc) noexcept {
        CallVideoTarget target{user_id, ssrc, 0, false};
        for (u32 t = 0; shown && t < view.scene.tile_count; ++t) {
            const auto& tile = view.scene.tiles[t];
            if (tile.user_id != user_id || !ui_call_scene_tile_visible(view.scene, t))
                continue;
            target.visible = true;
            target.tile_pixels = static_cast<u32>(
                maximum(ui_rect_width(tile.target), ui_rect_height(tile.target)) * pixel_scale + .5f);
        }
        targets[count++] = target;
    };
    for (u32 i = 0; i < snapshot.count && i < kGatewayVoiceVideoUsers; ++i) {
        const auto& user = snapshot.users[i];
        if (user.user_id && user.ssrc)
            add_target(user.user_id, user.ssrc);
    }
    // The watched stream is wanted (op 15 on its own connection) only while
    // its tile is visible, like cameras; its picture uses a tagged SSRC.
    const auto& watched = application_stream_view();
    if (watched.video_ssrc && watched.key.channel_id == s->channel_id &&
        (watched.state == StreamWatchState::live || watched.state == StreamWatchState::reconnecting))
        add_target(application_stream_tile(watched.key.owner_id), watched.video_ssrc);
    // Your own stream's self-preview: the session's picture, never a receive want.
    if (UiCallParticipant own{}; application_go_live_tile(s->channel_id, &own) &&
                                 (own.stream == UiCallStream::live || own.stream == UiCallStream::reconnecting))
        add_target(own.user_id, kGoLivePreviewSsrc);
    CallVideoHost callbacks{};
    callbacks.frame = +[](void*, u32 ssrc, VideoStreamFrame* output) noexcept {
        if (ssrc == kGoLivePreviewSsrc)
            return application_go_live_preview(output);
        auto& h = g_app.account;
        return h.instance && account_module_voice_video_frame(h.instance, ssrc, output);
    };
    callbacks.present = +[](void*, u64 user_id, u32 ssrc, const VideoStreamFrame& frame, u32 extent) noexcept {
        const auto result =
            renderer_present_video_frame(&g_app.renderer, media_asset_call_video(user_id, ssrc), frame.gpu, extent);
        if (!result.ok()) {
            // Per-frame failures: one line with the error and the count since
            // the previous line, at most every few seconds.
            static u64 next_log_ms = 0;
            static u32 suppressed = 0;
            const u64 now = clock_monotonic_milliseconds();
            if (now < next_log_ms) {
                ++suppressed;
            } else {
                log_u64(LogLevel::warning, "voice", "call_video_present_failed", static_cast<u64>(result.error));
                if (suppressed)
                    log_u64(LogLevel::warning, "voice", "call_video_present_failed_suppressed", suppressed);
                suppressed = 0;
                next_log_ms = now + 5'000;
            }
        }
        return result.ok();
    };
    callbacks.release = +[](void*, VideoStreamFrame* frame) noexcept {
        if (frame) {
            gpu_release(frame->gpu.chroma);
            gpu_release(frame->gpu.luma);
            gpu_release(frame->gpu.texture);
        }
    };
    callbacks.wanted = +[](void*, u64 user_id, bool wanted) noexcept {
        auto& h = g_app.account;
        if (g_app.account_ui && user_id == gateway_stream_video_user(g_app.account_ui->chat_state.self_user_id))
            return true; // The self-preview is local.
        return h.instance && account_module_voice_video_wanted(h.instance, user_id, wanted);
    };
    callbacks.resident = +[](void*, u64 user_id, u32 ssrc) noexcept {
        return g_app.renderer.preview_media_atlas &&
               media_atlas_peek(g_app.renderer.preview_media_atlas, media_asset_call_video(user_id, ssrc)).ok();
    };
    auto result = call_video_presenter_pump(&view.video, callbacks, {targets, count}, poll && shown);
    result.changed = result.changed || roster;
    return result;
}
u64 channel_guild(u64 channel_id) noexcept {
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id);
    return channel ? channel->guild_id : 0;
}
void toggle_focus(u64 user_id) noexcept {
    view.focus_user = view.focus_user == user_id ? 0 : user_id;
    application_repaint();
}
void activate(UiCallControl control, u64 user_id) noexcept {
    switch (control) {
    case UiCallControl::close:
        application_call_close();
        break;
    case UiCallControl::mute:
        application_voice_toggle_mute();
        break;
    case UiCallControl::deafen:
        application_voice_toggle_deafen();
        break;
    case UiCallControl::disconnect:
        application_voice_leave();
        application_call_close();
        break;
    case UiCallControl::fullscreen:
        set_fullscreen(!view.fullscreen);
        break;
    case UiCallControl::chat:
        release_pointer();
        set_chat(!chat_showing());
        break;
    case UiCallControl::tile_fullscreen:
        if (view.fullscreen && view.focus_user == user_id)
            set_fullscreen(false);
        else {
            view.focus_user = user_id;
            set_fullscreen(true);
        }
        break;
    case UiCallControl::stream_watch: {
        // Switching from another stream asks nothing, as in the official client.
        const u64 owner = gateway_stream_video_owner(user_id);
        const auto* state = owner ? voice_state_store_find(g_app.account_ui->voice_states,
                                                           channel_guild(view.channel_id), owner)
                                  : nullptr;
        if (state && voice_state_live(*state))
            application_stream_watch(gateway_stream_key_for(state->guild_id, state->channel_id, owner));
        break;
    }
    case UiCallControl::stream_stop:
        // Your own tile's bar stops your stream.
        if (g_app.account_ui && user_id == gateway_stream_video_user(g_app.account_ui->chat_state.self_user_id)) {
            application_go_live_stop();
            break;
        }
        [[fallthrough]];
    case UiCallControl::stream_dismiss:
        application_stream_stop();
        if (view.focus_user == user_id)
            view.focus_user = 0;
        break;
    case UiCallControl::stream_viewers:
        release_pointer();
        application_go_live_viewers_open(ui_call_scene_tile_control(view.scene, user_id, control));
        break;
    case UiCallControl::screen_share:
        release_pointer();
        application_go_live_button(view.scene.controls[static_cast<u32>(UiCallControl::screen_share)]);
        break;
    case UiCallControl::stream_volume:
        release_pointer();
        application_stream_menu_open(user_id, ui_call_scene_tile_control(view.scene, user_id, control),
                                     view.fullscreen && view.focus_user == user_id);
        break;
    case UiCallControl::stage_start:
    case UiCallControl::stage_topic:
    case UiCallControl::stage_end:
        release_pointer(); // These open a form.
        application_stage_activate(view.channel_id, control, user_id);
        break;
    case UiCallControl::stage_hand:
    case UiCallControl::stage_become:
    case UiCallControl::stage_audience:
    case UiCallControl::stage_accept:
    case UiCallControl::stage_decline:
    case UiCallControl::stage_invite:
    case UiCallControl::stage_move:
        application_stage_activate(view.channel_id, control, user_id);
        break;
    case UiCallControl::soundboard:
        release_pointer();
        application_soundboard_toggle(view.channel_id,
                                      view.scene.controls[static_cast<u32>(UiCallControl::soundboard)]);
        break;
    default:
        break; // Camera sending is not part of MaiCord.
    }
    application_repaint();
}
bool clear_hover() noexcept {
    if (!ui_call_scene_set_hover(&view.scene, -1e7f, -1e7f))
        return false;
    application_repaint();
    return true;
}
// Keys that would edit or send through the hidden composer.
bool editing_key(u32 key) noexcept {
    const bool control = application_control_down(), alt = application_alt_down();
    if (alt)
        return false;
    if (control)
        return key == 'V' || key == 'Z' || key == 'Y' || key == key::backspace || key == key::del;
    switch (key) {
    case key::enter: case key::backspace: case key::del: case key::space: case key::tab: case key::up: case key::down:
    case key::left: case key::right: case key::page_up: case key::page_down: case key::home: case key::end:
        return true;
    default:
        return false;
    }
}
} // namespace

bool application_call_open(u64 channel_id, u64 focus_user) noexcept {
    if (!g_app.account_ui || g_app.login.visible)
        return false;
    if (!channel_id)
        if (const auto* s = application_voice_call())
            channel_id = s->channel_id;
    if (!channel_id)
        return false;
    // The view replaces the side panels that share the content column.
    if (application_search_active())
        application_search_close_panel();
    if (application_pinned_messages_active())
        application_pinned_messages_close();
    if (application_thread_browser_active())
        application_thread_browser_close();
    if (!view.open || view.channel_id != channel_id) {
        view.opened_ms = clock_monotonic_milliseconds();
        reset_value(&view.lifecycle);
    }
    view.open = true;
    view.channel_id = channel_id;
    view.focus_user = focus_user;
    view.intent_revision = g_app.account_ui->chat_state.navigation_intent_revision;
    view.intent_pending = true;
    application_repaint();
    return true;
}

void application_call_close() noexcept {
    if (!view.open && !view.window_fullscreen)
        return;
    release_pointer();
    set_fullscreen(false);
    set_window_fullscreen(false);
    view.open = false;
    view.focus_user = 0;
    const bool split = view.split_width > 0;
    view.chat_open = false;
    view.split_width = 0;
    ui_call_scene_release(&view.scene);
    if (g_app.renderer.call_scene == &view.scene)
        g_app.renderer.call_scene = nullptr;
    if (split)
        relayout();
    application_repaint();
}

bool application_call_visible() noexcept {
    return scene_visible();
}

bool application_call_synchronize() noexcept {
    if (!view.open) {
        if (g_app.renderer.call_scene == &view.scene)
            g_app.renderer.call_scene = nullptr;
        return true;
    }
    if (g_app.account_ui && view.intent_pending) {
        view.intent_pending = false;
        view.intent_revision = g_app.account_ui->chat_state.navigation_intent_revision;
    }
    if (!g_app.account_ui || g_app.login.visible ||
        g_app.account_ui->chat_state.navigation_intent_revision != view.intent_revision ||
        application_search_active() || application_pinned_messages_active() ||
        application_thread_browser_active()) {
        application_call_close();
        return true;
    }
    const u64 now = clock_monotonic_milliseconds();
    const auto* s = application_voice_call();
    const bool call = s != nullptr;
    // Leaving, a server disconnect or a connection that failed for good ends
    // the view; a join still in flight keeps it for the grace period.
    const bool failed =
        call && (s->error != GatewayVoiceError::none || s->connection_error != VoiceConnectionError::none);
    switch (call_view_lifecycle_step(&view.lifecycle, view.channel_id, call ? s->channel_id : 0, failed,
                                     now - view.opened_ms)) {
    case CallViewStep::close:
        application_call_close();
        return true;
    case CallViewStep::follow:
        view.channel_id = s->channel_id; // Moved by the server: follow the call.
        break;
    case CallViewStep::keep:
        break;
    }
    if (call && !view.snapshot_valid)
        synchronize_video(false);
    const u32 count = project_participants(view.channel_id);
    const bool joined = call && s->channel_id == view.channel_id;
    const auto& preferences = application_voice_preferences();
    UiCallContent content{};
    content.title = call_title(view.channel_id);
    content.participants = {view.participants, count};
    content.focus_user = view.focus_user;
    content.gap = application_theme().scale.space_4;
    content.fullscreen = view.fullscreen;
    content.connected = joined && s->socket_phase == VoiceSocketPhase::open && s->udp_phase == VoiceUdpPhase::ready;
    content.self_mute = preferences.self_mute;
    content.self_deaf = preferences.self_deaf;
    content.server_mute = joined && (s->flags & gateway_voice_mute);
    content.server_deaf = joined && (s->flags & gateway_voice_deaf);
    content.stage = application_stage_content(view.channel_id);
    content.chat_available = chat_available(view.channel_id);
    if (view.chat_open && (!content.chat_available || !chat_showing())) {
        // The chat moved elsewhere or lost access: the call takes the column back.
        view.chat_open = false;
        relayout();
    }
    content.chat_open = view.chat_open;
    content.soundboard_available = joined && !content.stage.active && application_soundboard_available(view.channel_id);
    content.soundboard_open = content.soundboard_available && application_soundboard_picker_open();
    // Streaming goes to the call you are in; another channel's view offers it nothing.
    content.share = joined ? application_go_live_share(view.channel_id) : UiCallShare::not_allowed;
    // The search pass (earlier this frame) placed its field in the header.
    content.search_width = g_app.renderer.search_scene ? view.search_width : 0;
    const u32 revision = view.scene.revision;
    if (!ui_call_scene_update(&view.scene, content, call_viewport(), now, application_reduced_motion())) {
        // Too small to lay out (a tiny window): leave the content visible.
        if (g_app.renderer.call_scene == &view.scene) {
            g_app.renderer.call_scene = nullptr;
            application_repaint();
        }
        return true;
    }
    // A focus on someone who left falls back to the grid.
    if (view.focus_user && !view.scene.focus_user)
        view.focus_user = 0;
    if (g_app.renderer.call_scene != &view.scene || revision != view.scene.revision) {
        g_app.renderer.call_scene = &view.scene;
        g_app.renderer.retained_presentation.valid = false;
    }
    // Newly visible cameras start the frame poll on the next wake.
    for (u32 i = 0; !view.next_poll_ms && i < count; ++i)
        if (view.participants[i].video_ssrc)
            view.next_poll_ms = now + 1;
    return true;
}

namespace {
HANDLE video_notification() noexcept {
    const auto& host = g_app.account;
    return host.instance ? account_module_voice_video_notification(host.instance) : nullptr;
}
} // namespace

HANDLE application_call_video_notification() noexcept {
    return view.next_poll_ms ? video_notification() : nullptr;
}

void application_call_video_signaled() noexcept {
    view.next_poll_ms = call_video_poll_signaled(view.next_poll_ms, clock_monotonic_milliseconds());
}

bool application_call_pump(bool* repaint_requested) noexcept {
    if (!g_app.account_ui)
        return true;
    application_stream_pump();
    application_private_header_pump();
    const u64 now = clock_monotonic_milliseconds();
    // Poll when due (or unscheduled); earlier pumps only reconcile wants, so
    // a busy frame loop never postpones the next poll.
    const bool poll = call_video_poll_due(view.next_poll_ms, now);
    const auto result = synchronize_video(poll);
    const bool signaled = video_notification() != nullptr;
    view.next_poll_ms = call_video_poll_after(view.next_poll_ms, now, poll, result, signaled);
    const u64 morph = scene_visible() ? ui_call_scene_next_wake(view.scene, application_reduced_motion()) : 0;
    if ((result.presented || result.changed || (morph && now >= morph)) && repaint_requested) {
        *repaint_requested = true;
        if (result.changed)
            g_app.renderer.retained_presentation.valid = false;
    }
    return true;
}

void application_call_voice_pump() noexcept {
    if (!g_app.account_ui)
        return;
    application_stream_pump();
    application_private_header_pump();
    const auto result = synchronize_video(false);
    if (result.visible && !view.next_poll_ms)
        view.next_poll_ms = clock_monotonic_milliseconds() + call_video_poll_interval(video_notification() != nullptr);
    if (result.changed)
        application_repaint();
}

u64 application_call_next_wake() noexcept {
    // A join that never shows up closes the view when its grace runs out.
    u64 wake = earliest_deadline(view.next_poll_ms,
                                 view.open ? call_view_lifecycle_deadline(view.lifecycle, view.opened_ms) : 0);
    wake = earliest_deadline(wake, application_stream_next_wake());
    wake = earliest_deadline(wake, application_private_header_next_wake());
    if (scene_visible())
        wake = earliest_deadline(wake, ui_call_scene_next_wake(view.scene, application_reduced_motion()));
    return wake;
}

bool application_call_window_event(const WindowEvent& event) noexcept {
    if (event.type == EventType::capture_lost || event.type == EventType::cancel_input || input_focus_lost(event)) {
        if (view.captured && (event.type != EventType::capture_lost || event.focus.other != g_app.window)) {
            view.captured = false;
            view.pressed = UiCallControl::none;
            view.pressed_user = 0;
            if (ui_call_scene_set_pressed(&view.scene, UiCallControl::none))
                application_repaint();
        }
        return false;
    }
    if (!scene_visible())
        return false;
    if (event.type == EventType::pointer_leave) {
        clear_hover();
        return false;
    }
    if (modal_open()) {
        if (event.type == EventType::pointer_move)
            clear_hover();
        return false;
    }
    auto& scene = view.scene;
    if (input_plain_key(event)) {
        const u32 key = event.key.key;
        const bool down = event.type == EventType::key_down;
        if (key == key::escape && down && !application_control_down() && !application_shift_down() &&
            !application_alt_down()) {
            if (view.fullscreen) {
                set_fullscreen(false);
                return true;
            }
            if (view.focus_user) {
                toggle_focus(view.focus_user);
                return true;
            }
            return false;
        }
        // Beside the chat, keys belong to the composer.
        if (!editing_key(key) || view.split_width > 0)
            return false;
        // Voice keybinds keep working on keys the hidden composer would take.
        application_custom_keybind_input(down ? UiInputKind::key_down : UiInputKind::key_up, key,
                                         input_key_was_down(event));
        return true;
    }
    if (event.type == EventType::character && (event.flags & ev_system) == 0)
        return view.split_width <= 0;
    if (event.type == EventType::pointer_move) {
        const float x = client_dip_x(event), y = client_dip_y(event);
        if (ui_call_scene_set_hover(&scene, x, y))
            application_repaint();
        return view.captured || ui_call_scene_contains(scene, x, y);
    }
    if (event.type == EventType::cursor_query) {
        UiPoint point{};
        if (!client_cursor_dip(event, &point) || !ui_call_scene_contains(scene, point.x, point.y))
            return false;
        const auto hit = ui_call_scene_hit_test(scene, point.x, point.y);
        const bool actionable = hit.control != UiCallControl::none ? ui_call_control_enabled(scene, hit.control)
                                                                   : hit.user_id != 0;
        application_set_cursor(ui_action_cursor(actionable));
        return true;
    }
    if (input_down(event)) {
        const float x = client_dip_x(event), y = client_dip_y(event);
        if (!ui_call_scene_contains(scene, x, y))
            return false;
        window_focus(g_app.window);
        const auto hit = ui_call_scene_hit_test(scene, x, y);
        if (input_double(event) && hit.control == UiCallControl::none && hit.user_id) {
            activate(UiCallControl::tile_fullscreen, hit.user_id);
            return true;
        }
        if (hit.control != UiCallControl::none && !ui_call_control_enabled(scene, hit.control))
            return true;
        view.pressed = hit.control;
        view.pressed_user = hit.user_id;
        if (hit.control != UiCallControl::none || hit.user_id) {
            ui_call_scene_set_pressed(&scene, hit.control);
            window_capture_pointer(g_app.window);
            view.captured = true;
            application_repaint();
        }
        return true;
    }
    if (input_release(event)) {
        const float x = client_dip_x(event), y = client_dip_y(event);
        const bool pressed = view.pressed != UiCallControl::none || view.pressed_user;
        const auto control = view.pressed;
        const u64 user = view.pressed_user;
        release_pointer();
        if (!pressed)
            return ui_call_scene_contains(scene, x, y);
        const auto hit = ui_call_scene_hit_test(scene, x, y);
        if (control != UiCallControl::none && hit.control == control)
            activate(control, user);
        else if (control == UiCallControl::none && user && hit.user_id == user && hit.control == UiCallControl::none)
            toggle_focus(user);
        application_repaint();
        return true;
    }
    if (event.type == EventType::wheel) {
        const UiPoint point = client_wheel_dip(event);
        if (!ui_call_scene_contains(scene, point.x, point.y))
            return false;
        const bool horizontal = input_horizontal_wheel(event);
        const float pixels = static_cast<float>(horizontal ? event.wheel.dx120 : event.wheel.dy120) / 120.0f * 80;
        if (ui_call_scene_scroll(&scene, point.x, point.y, horizontal ? pixels : -pixels))
            application_repaint();
        return true;
    }
    if (input_other_button(event) && (event.flags & ev_double_click) == 0)
        return ui_call_scene_contains(scene, client_dip_x(event), client_dip_y(event));
    if (event.type == EventType::context_menu) {
        if ((event.flags & ev_keyboard) != 0)
            return false;
        const UiPoint point = client_context_dip(event);
        if (!ui_call_scene_contains(scene, point.x, point.y))
            return false;
        // The watched stream's menu: volume, full screen, stop watching.
        const auto hit = ui_call_scene_hit_test(scene, point.x, point.y);
        if (hit.user_id && gateway_stream_video_user_tagged(hit.user_id))
            application_stream_menu_open(hit.user_id, {point.x, point.y, point.x + 1, point.y + 1},
                                         view.fullscreen && view.focus_user == hit.user_id);
        return true;
    }
    return false;
}

bool application_call_header_search(float width, UiRect* field) noexcept {
    if (field)
        reset_value(field);
    view.search_width = 0;
    // Beside its chat (split) the call leaves the title bar to the chat column.
    if (!field || !view.open || !g_app.account_ui || g_app.login.visible || application_settings_active() ||
        view.split_width > 0)
        return false;
    if (view.fullscreen)
        return true; // Covered: no search field over a fullscreen call.
    const UiRect viewport = call_viewport();
    *field = ui_call_header_layout({viewport.left, viewport.top, viewport.right, viewport.top + kUiCallHeaderHeight},
                                   chat_available(view.channel_id), width)
                 .search;
    if (ui_rect_has_area(*field))
        view.search_width = width;
    return true;
}

float application_call_chat_split(float available) noexcept {
    // The chat column keeps a readable width; the call keeps room for tiles.
    constexpr float kChatMinimum = 340, kChatMaximum = 520, kCallMinimum = 360;
    float split = 0;
    if (view.open && view.chat_open && !view.fullscreen && available > 0) {
        const float chat = minimum(maximum(available * .4f, kChatMinimum), kChatMaximum);
        if (available - chat >= kCallMinimum)
            split = available - chat;
    }
    view.split_width = split;
    return split;
}

void application_call_toggle_tile_fullscreen(u64 tile_id) noexcept {
    if (view.open && tile_id)
        activate(UiCallControl::tile_fullscreen, tile_id);
}

void application_call_release() noexcept {
    application_call_close();
    call_video_presenter_reset(&view.video);
    view.snapshot_valid = false;
    view.next_poll_ms = 0;
}
} // namespace maicord::app_detail
