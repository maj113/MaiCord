#include "video_internal.hpp"
#include "video.hpp"
#include "../shell/frame_presentation.hpp"
#include "../voice/call.hpp"
#include "../chat/application_link_trust.hpp"
#include "maicord/ui/core/scene_cursor.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/core/log_event.hpp"
#include "../account/account_presentation.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/voice/audio_presentation.hpp"

namespace maicord::app_detail {

bool application_video_pump(bool* repaint) noexcept {
    // Call view pictures share the video wake and presentation turn.
    if (!application_call_pump(repaint))
        return false;
    auto* video = g_app.video;
    if (!video)
        return true;
    const auto prior_playback_error = video->status.playback.error;
    const u32 prior_native_error = video->status.playback.native_error;
    const auto prior_rejected_error = video->status.playback.rejected_error;
    const bool searched = media_asset_search(video->source.key);
    if (!video_session_available() || video->account_serial != application_account_scope() ||
        (!searched && video->source.channel_id != g_app.account_ui->active_channel_id)) {
        application_video_close();
        return true;
    }
    ApplicationSearchSourceView search_source{};
    const auto found = searched ? MessageStoreResult{}
                                : message_store_find(g_app.message_view.store, video->source.message_id);
    const auto* message = searched
                              ? (application_search_source_resolve(video->source.search_source,
                                                                   video->source.snapshot, &search_source)
                                     ? search_source.message
                                     : nullptr)
                          : found.ok() ? message_store_resolve(g_app.message_view.store, found.handle)
                                       : nullptr;
    if (!message || !application_channel_readable(g_app, message->channel_id) ||
        message->revision != video->source.revision) {
        application_video_close();
        return true;
    }
    if (inline_media_activation_consumed(video->source, InlineMediaPolicyUse::retain)) {
        application_video_close();
        return true;
    }
    if (video->live_restart) {
        restart_live_session(*video);
        if (repaint) *repaint = true;
        return true;
    }
    InlineMediaSource positioned{};
    bool visible = position_video_scene(video, positioned);
    const bool surface_covered = content_surface_covers_video();
    if (video->selecting && (!searched || visible || surface_covered)) {
        const auto& existing = g_app.account.api.video_status(g_app.account.instance);
        if (existing.selected && existing.playback.identity.session_epoch != video->session_epoch) {
            AccountVideoFrame frame{};
            if (g_app.account.api.video_take(g_app.account.instance, &frame) &&
                !g_app.account.api.video_acknowledge(g_app.account.instance, &frame))
                return false;
            if (!account_video_identity_equal(video->retiring_identity, existing.playback.identity)) {
                const AccountVideoControl stop{existing.playback.identity,
                                               existing.playback.desired_command + 1,
                                               0,
                                               1,
                                               AccountVideoCommand::stop,
                                               false};
                const auto stopped = g_app.account.api.video_control(g_app.account.instance, &stop);
                if (stopped.ok() && stopped.admitted)
                    video->retiring_identity = existing.playback.identity;
            }
        } else if (!surface_covered) {
            const AccountVideoSelection selection{video->source.key,
                                                  {video->account_serial, video->source.message_id, 0,
                                                   video->session_epoch, video->source.revision, 0, 0}, video->collection_track};
            const auto selected = g_app.account.api.video_select(g_app.account.instance, &selection);
            if (selected.ok() && selected.admitted) {
                video->selecting = false;
                video->selected = true;
                refresh_video_status(*video);
            } else if (selected.error == AccountVideoError::busy) {
                if (!request_video_source(video->source, &video->source_refresh, video)) {
                    video->selecting = false;
                    video->status.playback.error = AccountVideoError::decoder;
                }
            } else {
                video->selecting = false;
                video->status.playback.error = selected.error;
                video->status.playback.native_error = selected.native_error;
            }
        }
    }
    if (video->selected) {
        refresh_video_status(*video);
        if (video->status.collection_count && video->status.collection_track &&
            video->status.collection_track <= video->status.collection_count) {
            video->collection_count = video->status.collection_count;
            video->collection_track = video->status.collection_track;
            video->collection_more = video->status.collection_more;
        }
        if (video->status.playback.identity.session_epoch != video->session_epoch ||
            video->status.playback.identity.account_serial != video->account_serial ||
            video->status.playback.identity.message_id != video->source.message_id ||
            video->status.playback.identity.message_revision != video->source.revision) {
            application_video_close();
            return true;
        }
        if (!video->status.selected)
            video->requested_play = false;
        if (video->status.source_refresh && video->status.selected) {
            if (video->status.playback.live) {
                if (video->live_restarts < 3) {
                    ++video->live_restarts;
                    video->live_restart = true;
                    application_request_frame();
                }
            } else {
                // Signed attachments keep their entity and replace only the URL.
                bool force = true;
                request_video_source(video->source, &force, video);
            }
        }
        if (!video->status.source_refresh && video->status.playback.playing && video->status.playback.position_100ns >= 50000000)
            video->live_restarts = 0;
        if (video->restore_gain && !video->live_restart &&
            send_video_command(*video, AccountVideoCommand::volume, 0, video->restore_volume) &&
            send_video_command(*video, AccountVideoCommand::mute, 0, 1, video->restore_muted)) video->restore_gain = false;
        if (video->requested_seek && visible && video->status.playback.duration_100ns > 0) {
            const i64 target = minimum<i64>(i64{video->source.start_seconds} * 10000000,
                                            video->status.playback.duration_100ns);
            if (send_video_command(*video, AccountVideoCommand::seek, target))
                video->requested_seek = false;
        }
        if (video->requested_play && !video->requested_seek && !video->restore_gain && !video->live_restart && visible) {
            if (send_video_command(*video, AccountVideoCommand::play))
                video->requested_play = false;
        }
        if (!visible && video->status.playback.playing && !video->hidden_pause) {
            video->hidden_pause = send_video_command(*video, AccountVideoCommand::pause);
        }
        if (visible && !surface_covered && video->collection_continue && !video->requested_play &&
            video->collection_track && video->collection_track < video->collection_count && video->status.playback.ended &&
            video->status.playback.error == AccountVideoError::none && video->status.playback.rejected_error == AccountVideoError::none &&
            video->status.playback.observed_command >= video->status.playback.desired_command) {
            const bool switched = switch_collection_track(*video, video->collection_track + 1, video->ui.video.focus);
            if (repaint) *repaint = true;
            return switched;
        }
        AccountVideoFrame frame{};
        if (g_app.account.api.video_take(g_app.account.instance, &frame)) {
            bool geometry_changed = false;
            const bool current =
                account_video_identity_equal(frame.identity, video->status.playback.identity) &&
                media_asset_key_equal(frame.key, video->source.key);
            if (current && visible && frame.gpu && !video->source.audio) {
                const u32 extent = static_cast<u32>(maximum(ui_rect_width(positioned.preview),
                    ui_rect_height(positioned.preview)) * g_app.renderer.dpi / 96.0f + .5f);
                const auto uploaded = renderer_present_video_frame(&g_app.renderer, frame.key, *frame.gpu,
                                                                  maximum(1u, extent));
                if (uploaded.ok()) {
                    video->provider_frame_ready = video->source.provider;
                    geometry_changed = update_video_extent(*video, *frame.gpu);
                    if (repaint)
                        *repaint = true;
                } else {
                    video->presentation_failed = true;
                    send_video_command(*video, AccountVideoCommand::pause);
                    log_write(LogLevel::warning, "media", "video_frame_presentation_failed");
                }
            }
            if (!g_app.account.api.video_acknowledge(g_app.account.instance, &frame))
                return false;
            if (geometry_changed) {
                if (!refresh_account_text(true)) {
                    application_record_presentation_failure("video_geometry_projection_failed");
                    application_request_frame();
                }
                visible = position_video_scene(video, positioned);
            }
        }
    }
    const auto& current_playback = video->status.playback;
    if ((current_playback.error != AccountVideoError::none &&
         (current_playback.error != prior_playback_error ||
          current_playback.native_error != prior_native_error)) ||
        (current_playback.rejected_error != AccountVideoError::none &&
         current_playback.rejected_error != prior_rejected_error)) {
        LogEvent("video_playback_status_failed")
            .field("error", static_cast<u64>(current_playback.error))
            .error("native_error", static_cast<u32>(current_playback.native_error))
            .field("rejected_error", static_cast<u64>(current_playback.rejected_error))
            .field("position_seconds", current_playback.position_100ns > 0
                                           ? static_cast<u64>(current_playback.position_100ns / 10000000)
                                           : 0)
            .field("has_audio", static_cast<u64>(current_playback.has_audio))
            .field("selected", static_cast<u64>(video->status.selected))
            .write(LogLevel::warning, "media");
    }
    return project_video_scene(video, positioned, visible, repaint);
}

u64 application_video_next_wake() noexcept {
    const u64 inline_wake = g_app.video && video_surface_available() && !g_app.preferences.reduced_motion
                                ? g_app.video->presentation_wake_ms
                                : 0;
    const u64 call_wake = application_call_next_wake();
    return !inline_wake ? call_wake : !call_wake ? inline_wake : minimum(inline_wake, call_wake);
}
// Pointer, cursor, key and tooltip events: input-rate, not per frame. The pump above stays at full speed.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
bool application_video_pointer(const WindowEvent& event) noexcept {
    VideoPointer pointer{};
    switch (event.type) {
    case EventType::pointer_leave:
        pointer = VideoPointer::leave;
        break;
    case EventType::pointer_move:
        pointer = VideoPointer::move;
        break;
    case EventType::pointer_down:
        if (!input_press(event))
            return false;
        pointer = VideoPointer::down;
        break;
    case EventType::pointer_up:
        if (!input_release(event))
            return false;
        pointer = VideoPointer::up;
        break;
    default:
        return false;
    }
    if (pointer == VideoPointer::leave) {
        if (g_app.video) {
            if (g_app.video->source.audio) {
                auto& scene = g_app.video->ui.audio;
                const auto before = ui_audio_scene_paint_revision(scene);
                ui_audio_scene_pointer(&scene, UiAudioPointerEvent::leave, {});
                if (before != ui_audio_scene_paint_revision(scene))
                    application_request_frame();
                return false;
            }
            auto& scene = g_app.video->ui.video;
            const auto before = ui_video_scene_paint_revision(scene);
            ui_video_scene_pointer(&scene, UiVideoPointerEvent::leave, {});
            if (before != ui_video_scene_paint_revision(scene))
                application_request_frame();
        }
        return false;
    }
    if (!video_surface_available())
        return false;
    const float scale = 96.0f / static_cast<float>(g_app.renderer.dpi);
    const UiPoint point{event.pointer.x * scale, event.pointer.y * scale};
    auto* video = g_app.video;
    if (video && !search_video_still_current(*video))
        return false;
    if (video && video->source.audio && video->ui.audio.active) {
        InlineMediaSource current{};
        if (!resolve_visible_video_source(video->source.message_id, video->source.revision, false,
                                          video->source.index, &current, video->source.snapshot,
                                          &video->source.search_source) ||
            !current.audio || !media_asset_key_equal(current.key, video->source.key)) {
            application_video_close();
            return false;
        }
        const auto before = ui_audio_scene_paint_revision(video->ui.audio);
        const auto kind = pointer == VideoPointer::down ? UiAudioPointerEvent::down
                          : pointer == VideoPointer::up ? UiAudioPointerEvent::up
                                                        : UiAudioPointerEvent::move;
        const auto result = ui_audio_scene_pointer(&video->ui.audio, kind, point);
        if (before != ui_audio_scene_paint_revision(video->ui.audio))
            application_request_frame();
        if (result.handled) {
            if (pointer == VideoPointer::down)
                acquire_video_focus(*video);
            dispatch_audio_event(*video, result);
            return true;
        }
    }
    if (video && !video->source.audio && video->ui.video.active && video_scene_pointer_event(*video, pointer, point))
        return true;
    if (pointer != VideoPointer::down)
        return false;
    // Search's typed rich target owns preview activation. A miss on its player
    // controls must not fall through into a covered live message preview.
    if (video && media_asset_search(video->source.key)) {
        application_video_blur();
        return false;
    }
    InlineMediaSource source{};
    const MessageRichRecord* rich = nullptr;
    u32 item_index = 0;
    if (visible_source_at(point, &source, &rich, &item_index)) {
        if (inline_media_activation_consumed(source))
            return true;
        if (source.audio) {
            UiAudioControlsProjection controls{};
            if (!ui_audio_attachment_project(g_app.account_ui->message_rich_store, rich->attachments[item_index],
                                             video ? video->session_epoch : 1, u64{source.revision} + 1,
                                             message_rich_record_voice(*rich), source.preview,
                                             application_theme(), {}, &controls))
                return false;
            UiAudioAction action{};
            if (!ui_audio_controls_pointer_action(controls, point, &action))
                return false;
            if (action.control == UiAudioControl::download)
                return application_downloads_begin(source.message_id, source.key.id, source.snapshot);
            return action.control == UiAudioControl::play_pause && activate_inline_source(source);
        }
        if (video && !video->source.audio && media_asset_key_equal(source.key, video->source.key) &&
            video->selected && video->status.selected && !video->presentation_failed &&
            video->status.playback.error == AccountVideoError::none) {
            acquire_video_focus(*video);
            ui_video_scene_focus(&video->ui.video, UiVideoControl::play_pause, false);
            send_video_command(*video, video->status.playback.playing ? AccountVideoCommand::pause
                                                                      : AccountVideoCommand::play);
            application_request_frame();
            return true;
        }
        // The open control of a provider embed's pill leaves for the page
        // (through the link trust rules) instead of starting playback.
        if (source.embed && source.provider && rich && item_index < rich->embed_count &&
            message_rich_embed_open_pill(g_app.account_ui->message_rich_store, rich->embeds[item_index]) &&
            ui_rect_contains(ui_video_embed_actions(source.preview, true, UiVideoEmbedActionsStyle{}).open, point.x,
                             point.y)) {
            const Utf8View page =
                message_rich_store_string(g_app.account_ui->message_rich_store, rich->embeds[item_index].url);
            if (message_markdown_url_valid(page) && page.bytes <= 2'047)
                application_link_open(page, false);
            return true;
        }
        return start_video_session(source);
    }
    if (video) {
        application_video_blur();
    }
    return false;
}
bool application_video_viewer_pointer(VideoPointer pointer, UiPoint point) noexcept {
    auto* video = g_app.video;
    if (!video || !video->expanded || video->source.audio || !video_surface_available() ||
        !video->ui.video.active)
        return false;
    auto& scene = video->ui.video;
    if (video_scene_pointer_event(*video, pointer, point))
        return true;
    // A click on the picture itself toggles playback, as in the official client's viewer.
    if (pointer == VideoPointer::down && ui_rect_contains(ui_rect_intersection(scene.preview, scene.clip), point.x,
                                                          point.y)) {
        acquire_video_focus(*video);
        dispatch_video_event(*video, ui_video_scene_player_key(&scene, UiVideoPlayerKey::toggle_play, 0,
                                                               clock_monotonic_milliseconds()));
        return true;
    }
    return pointer != VideoPointer::move && ui_rect_contains(ui_rect_intersection(scene.preview, scene.clip),
                                                             point.x, point.y);
}
bool application_video_cursor(UiPoint point, UiCursor* cursor) noexcept {
    if (!cursor || !g_app.account_ui || !video_surface_available() || !g_app.message_scene.active ||
        !ui_rect_contains(g_app.message_scene.viewport, point.x, point.y))
        return false;
    const auto* video = g_app.video;
    if (video && inline_media_activation_consumed(video->source, InlineMediaPolicyUse::cursor))
        return false;
    if (video && video->source.audio && video->ui.audio.active &&
        ui_rect_contains(ui_rect_intersection(video->ui.audio.projection.geometry.bounds, video->ui.audio.clip),
                         point.x, point.y)) {
        *cursor = ui_audio_scene_cursor(video->ui.audio, point);
        return true;
    }
    // A selected inline player toggles playback anywhere on its picture.
    if (video && !video->source.audio && !video->expanded && video->ui.video.active &&
        ui_rect_contains(ui_rect_intersection(video->ui.video.preview, video->ui.video.clip), point.x, point.y)) {
        *cursor = UiCursor::hand;
        return true;
    }
    // Search's typed rich target owns preview activation while it plays.
    if (video && media_asset_search(video->source.key))
        return false;
    InlineMediaSource source{};
    const MessageRichRecord* rich = nullptr;
    u32 item_index = 0;
    if (!visible_source_at(point, &source, &rich, &item_index) ||
        inline_media_activation_consumed(source, InlineMediaPolicyUse::cursor))
        return false;
    if (!source.audio) {
        *cursor = UiCursor::hand;
        return true;
    }
    // An unselected card activates only Play and Download.
    UiAudioControlsProjection controls{};
    const auto control =
        ui_audio_attachment_project(g_app.account_ui->message_rich_store, rich->attachments[item_index],
                                    video ? video->session_epoch : 1, u64{source.revision} + 1,
                                    message_rich_record_voice(*rich), source.preview, application_theme(), {},
                                    &controls)
            ? ui_audio_controls_hit(controls, point)
            : UiAudioControl::none;
    *cursor = ui_action_cursor(control == UiAudioControl::play_pause || control == UiAudioControl::download);
    return true;
}
bool application_video_viewer_cursor(UiPoint point, UiCursor* cursor) noexcept {
    const auto* video = g_app.video;
    if (!cursor || !video || !video->expanded || video->source.audio || !video->ui.video.active)
        return false;
    if (application_video_viewer_hidden_cursor()) {
        *cursor = UiCursor::hidden;
        return true;
    }
    *cursor = ui_video_scene_cursor(video->ui.video, point);
    return *cursor != UiCursor::arrow;
}
bool application_video_viewer_hidden_cursor() noexcept {
    const auto* video = g_app.video;
    return video && video->expanded && !video->source.audio && video->ui.video.active &&
           video->ui.video.idle && video->ui.video.preview_hovered;
}
float application_video_viewer_chrome() noexcept {
    const auto* video = g_app.video;
    return video && video->expanded && !video->source.audio ? ui_video_scene_chrome_opacity(video->ui.video) : 1.0f;
}
void application_video_viewer_activity() noexcept {
    auto* video = g_app.video;
    if (video && video->expanded && !video->source.audio && video->ui.video.active &&
        ui_video_scene_activity(&video->ui.video, clock_monotonic_milliseconds()))
        application_request_frame();
}
namespace {
// The next character event belongs to a key the player took as a shortcut.
bool g_discard_character = false;
} // namespace

bool application_video_key(u32 key, bool shift, bool repeat) noexcept {
    if (content_surface_covers_video())
        return false;
    auto* video = g_app.video;
    if (video && !search_video_still_current(*video))
        return false;
    if (!video || !(video->source.audio ? video->ui.audio.active : video->ui.video.active) ||
        !video_surface_available())
        return false;
    const bool focused = video_input_focus_current(*video) &&
                         (video->source.audio ? video->ui.audio.focus != UiAudioControl::none
                                              : video->ui.video.focus != UiVideoControl::none);
    // Player shortcuts also follow the pointer, but never steal typing or list focus.
    const bool hovered = !video->source.audio && video->ui.video.preview_hovered &&
                         application_window_keyboard_focus() && !g_app.message_composer.focused && !g_app.context_focus;
    if (!focused && !hovered && !video->expanded)
        return false;
    if (key == key::escape) {
        if (video->expanded)
            set_video_expanded(*video, false);
        else if (focused)
            application_video_close();
        else
            return false;
        application_request_frame();
        return true;
    }
    if (video->source.audio) {
        UiAudioKey action{};
        if (key == key::tab)
            action = shift ? UiAudioKey::previous : UiAudioKey::next;
        else if (key == key::left || key == key::down)
            action = UiAudioKey::decrease;
        else if (key == key::right || key == key::up)
            action = UiAudioKey::increase;
        else if (key == key::home)
            action = UiAudioKey::home;
        else if (key == key::end)
            action = UiAudioKey::end;
        else if ((key == key::space || key == key::enter) && !repeat)
            action = UiAudioKey::activate;
        else
            return false;
        dispatch_audio_event(*video, ui_audio_scene_key(&video->ui.audio, action));
        return true;
    }
    auto& scene = video->ui.video;
    const u64 now = clock_monotonic_milliseconds();
    const bool modified = application_control_down() || application_alt_down();
    const bool range_focus = focused && scene.keyboard_focus &&
        (scene.focus == UiVideoControl::volume || scene.focus == UiVideoControl::track);
    const auto player = [&](UiVideoPlayerKey shortcut, u32 percent = 0) noexcept {
        return ui_video_scene_player_key(&scene, shortcut, percent, now);
    };
    u32 digit = 10;
    if (key >= '0' && key <= '9')
        digit = key - '0';
    else if (key >= key::numpad0 && key <= key::numpad9)
        digit = key - key::numpad0;
    const bool printable = key == key::space || digit < 10 || key == 'M' || key == 'F';
    if (modified && printable && key != key::space)
        return false;
    UiVideoSceneResult result{};
    if (key == key::tab) {
        if (!focused && !video->expanded)
            return false;
        result = ui_video_scene_key(&scene, shift ? UiVideoKey::previous : UiVideoKey::next);
    } else if (key == key::enter) {
        if (!focused)
            return false;
        if (!repeat)
            result = ui_video_scene_key(&scene, UiVideoKey::activate);
    } else if (key == key::space) {
        if (!repeat)
            result = player(UiVideoPlayerKey::toggle_play);
    } else if (key == key::left || key == key::right) {
        result = range_focus ? ui_video_scene_key(&scene, key == key::left ? UiVideoKey::decrease
                                                                          : UiVideoKey::increase)
                              : player(key == key::left ? UiVideoPlayerKey::seek_backward
                                                      : UiVideoPlayerKey::seek_forward);
    } else if (key == key::up || key == key::down) {
        result = player(key == key::up ? UiVideoPlayerKey::volume_up : UiVideoPlayerKey::volume_down);
    } else if (key == key::home || key == key::end) {
        result = range_focus ? ui_video_scene_key(&scene, key == key::home ? UiVideoKey::home : UiVideoKey::end)
                              : player(UiVideoPlayerKey::seek_percent, key == key::home ? 0 : 100);
    } else if (digit < 10) {
        if (!repeat)
            result = player(UiVideoPlayerKey::seek_percent, digit * 10);
    } else if (key == 'M') {
        if (!repeat)
            result = player(UiVideoPlayerKey::toggle_mute);
    } else if (key == 'F') {
        if (!repeat)
            result = player(UiVideoPlayerKey::expand);
    } else {
        return false;
    }
    ui_video_scene_activity(&scene, now);
    // The system already queued (or will queue) this key's character; it is not text.
    if (printable)
        g_discard_character = true;
    result.handled = true;
    dispatch_video_event(*video, result);
    return true;
}

bool application_video_discard_character(const WindowEvent& event) noexcept {
    if (event.type == EventType::key_up)
        g_discard_character = false;
    else if (event.type == EventType::character && g_discard_character) {
        g_discard_character = false;
        return true;
    }
    return false;
}

bool application_video_tooltip_target(UiPoint point, bool keyboard, u64 scope,
                                      UiTooltipTarget* target) noexcept {
    const auto* video = g_app.video;
    if (target && video && video->source.audio && video_surface_available() && video->ui.audio.active &&
        video->ui.audio.capture == UiAudioControl::none) {
        const auto& scene = video->ui.audio;
        InlineMediaSource current{};
        if (!resolve_visible_video_source(video->source.message_id, video->source.revision, false,
                                          video->source.index, &current, video->source.snapshot,
                                          &video->source.search_source) ||
            !current.audio || !media_asset_key_equal(current.key, video->source.key) ||
            !ui_rect_equal(current.preview, scene.projection.geometry.bounds) ||
            !ui_rect_equal(current.clip, scene.clip) || current.owner.manager != scene.owner.manager ||
            !ui_widget_handle_equal(current.owner.handle, scene.owner.handle))
            return false;
        const auto id = keyboard ? scene.focus : ui_audio_scene_hit(scene, point);
        if (keyboard && (!video_input_focus_current(*video) || !scene.keyboard_focus))
            return false;
        const auto* control = ui_audio_controls_find(scene.projection, id);
        if (!control || !control->enabled || control->hidden)
            return false;
        const auto anchor = ui_rect_intersection(control->bounds, scene.clip);
        const auto label = ui_audio_controls_label(scene.projection, id);
        if (!ui_rect_has_area(anchor) || !label.bytes)
            return false;
        *target = {{scene.owner, scope, video->source.message_id,
                    hash64_mix(video->session_epoch, u64{video->source.revision}),
                    0x500u + static_cast<u32>(id)},
                   anchor,
                   label,
                   UiTooltipPlacement::above};
        return true;
    }
    if (!target || !video || video->source.audio || !video_surface_available() || !video->selected ||
        !video->status.selected || !video->ui.video.active ||
        video->ui.video.capture != UiVideoControl::none ||
        video->status.playback.identity.session_epoch != video->session_epoch)
        return false;
    InlineMediaSource current{};
    if (!resolve_visible_video_source(video->source.message_id, video->source.revision, video->source.embed,
                                      video->source.index, &current, video->source.snapshot,
                                      &video->source.search_source) ||
        !media_asset_key_equal(current.key, video->source.key) ||
        !ui_rect_equal(current.preview, video->ui.video.preview) ||
        !ui_rect_equal(current.clip, video->ui.video.clip))
        return false;
    const auto& scene = video->ui.video;
    const auto control_id = keyboard ? scene.focus : ui_video_scene_hit(scene, point);
    if ((keyboard && (!video_input_focus_current(*video) || !scene.keyboard_focus)) ||
        (control_id != UiVideoControl::play_pause && control_id != UiVideoControl::mute))
        return false;
    for (const auto& control : scene.projection.controls) {
        const UiRect bounds = !keyboard && control_id == UiVideoControl::play_pause &&
                                      ui_rect_contains(scene.center_play, point.x, point.y)
                                  ? scene.center_play
                                  : control.bounds;
        const UiRect anchor = ui_rect_intersection(bounds, scene.clip);
        if (control.id != control_id || !control.enabled || control.hidden || !ui_rect_has_area(anchor))
            continue;
        const u64 revision =
            hash64_mix(hash64_mix(scene.projection.playback.generation, scene.projection.playback.revision),
                       video->session_epoch);
        *target = {{{&g_app.message_scene.widgets, g_app.message_scene.root},
                    scope,
                    video->source.message_id,
                    revision,
                    0x400u + static_cast<u32>(control_id)},
                   anchor,
                   ui_video_scene_label(scene, control_id),
                   UiTooltipPlacement::above};
        return target->label.bytes != 0;
    }
    return false;
}

#pragma clang attribute pop

} // namespace maicord::app_detail
