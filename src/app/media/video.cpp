#include "video_internal.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "video.hpp"
#include "../shell/frame_presentation.hpp"
#include "policy.hpp"
#include "maicord/media/core/presentation_policy.hpp"
#include "../../render/d3d11/renderer_interaction_state.hpp"
#include "maicord/app/media/account_media.hpp"
#include "maicord/media/containers/track_info.hpp"
#include "maicord/ui/voice/audio_controls.hpp"
#include "maicord/ui/voice/audio_presentation.hpp"
#include "maicord/ui/voice/video_controls.hpp"
#include "maicord/ui/voice/video_scene.hpp"

namespace maicord::app_detail {
namespace {

UiVideoPlaybackState project_video_playback(const ApplicationVideo& video) noexcept {
    const auto& playback = video.status.playback;
    UiVideoPlaybackState out{};
    // Control capture belongs to this selected message, across native seeks.
    // GPU frame leases keep their independent playback-epoch fence.
    out.owner = video.source.message_id;
    out.generation = video.session_epoch;
    out.revision = u64{video.source.revision} + 1;
    out.elapsed_ms = playback.position_100ns > 0 ? static_cast<u64>(playback.position_100ns / 10000) : 0;
    out.live = playback.live;
    out.duration_ms = !out.live && playback.duration_100ns > 0 ? static_cast<u64>(playback.duration_100ns / 10000) : 0;
    out.volume = playback.volume_valid ? static_cast<u32>(playback.applied_volume * 100 + .5f) : 0;
    out.playing = playback.playing;
    out.muted = playback.muted;
    out.error = video.presentation_failed || playback.error != AccountVideoError::none ||
                playback.rejected_error != AccountVideoError::none;
    out.buffering = !out.error && (video.selecting || playback.buffering || playback.seeking);
    out.can_play = video.selected && video.status.selected && !video.selecting &&
                   !video.presentation_failed && !playback.release_pending &&
                   playback.error == AccountVideoError::none;
    out.can_seek = out.can_play && !out.live && out.duration_ms != 0;
    out.has_audio = playback.has_audio;
    out.can_volume = out.can_play && playback.has_audio;
    out.collection_count = video.collection_count;
    out.collection_track = video.collection_track;
    out.collection_more = video.collection_more;
    out.can_change_track = video.collection_count && !video.selecting && !playback.release_pending;
    // In the viewer the toolbar's full screen button replaces expand.
    out.can_expand = !video.source.audio && !video.source.audio_preview && !video.presentation_failed && !video.expanded;
    out.expanded = video.expanded;
    const auto& buffered = video.status.buffered;
    out.buffered_count = out.live ? 0 : minimum<u8>(buffered.count, 4);
    for (u32 i = 0; i < out.buffered_count; ++i) {
        out.buffered_begin[i] = buffered.begin[i];
        out.buffered_end[i] = buffered.end[i];
    }
    return out;
}
// Once per audio attachment opened.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
bool capture_audio_metadata(ApplicationVideo& video) noexcept {
    if (!video.source.audio)
        return true;
    const MessageRichStore* store = &g_app.account_ui->message_rich_store;
    const MessageRichRecord* rich = nullptr;
    ApplicationSearchSourceView searched{};
    if (media_asset_search(video.source.key)) {
        if (!application_search_source_resolve(video.source.search_source, video.source.snapshot,
                                               &searched))
            return false;
        store = searched.rich_store;
        rich = searched.rich;
    } else
        rich = video.source.snapshot ? message_rich_store_snapshot(*store, video.source.message_id)
                                     : message_rich_store_find(*store, video.source.message_id);
    if (!store || !rich || rich->message_revision != video.source.revision ||
        video.source.index >= rich->attachment_count)
        return false;
    const auto& attachment = rich->attachments[video.source.index];
    if (attachment.id != video.source.key.id ||
        message_rich_attachment_audio_format(*store, attachment) !=
            media_asset_attachment_format(video.source.key) ||
        !message_rich_store_string(*store, attachment.url).bytes)
        return false;
    auto title = message_rich_store_string(*store, attachment.title);
    if (!title.bytes)
        title = message_rich_store_string(*store, attachment.filename);
    if (!title.bytes)
        title = utf8_literal("Audio attachment");
    u32 copied = 0;
    while (copied < title.bytes) {
        const auto cp = utf8_decode_one(title.data + copied, title.bytes - copied);
        if (!cp.valid)
            return false;
        if (copied + cp.width > sizeof(video.audio_title))
            break;
        copied += cp.width;
    }
    copy_bytes(video.audio_title, title.data, copied);
    video.audio_title_bytes = copied;
    const auto encoded = message_rich_store_string(*store, attachment.waveform);
    if (encoded.bytes) {
        const auto decoded = base64_decode(encoded, {video.audio_waveform, kUiAudioWaveformMaximum});
        if (!decoded.ok())
            return false;
        video.audio_waveform_bytes = decoded.bytes;
    }
    video.audio_declared_duration_ms = attachment.duration_ms;
    video.audio_duration_known = (attachment.state & 8u) != 0;
    video.audio_voice = message_rich_record_voice(*rich);
    return true;
}
#pragma clang attribute pop
UiAudioPlaybackState project_audio_playback(const ApplicationVideo& video) noexcept {
    const auto& playback = video.status.playback;
    UiAudioPlaybackState out{};
    const bool observed =
        video.selected && video.status.selected &&
        playback.kind == audio_playback_kind(media_asset_attachment_format(video.source.key)) &&
        playback.identity.session_epoch == video.session_epoch;
    out.owner = video.source.key.id;
    out.generation = video.session_epoch;
    out.revision = u64{video.source.revision} + 1;
    out.elapsed_ms =
        observed && playback.position_100ns > 0 ? static_cast<u64>(playback.position_100ns / 10000) : 0;
    out.duration_ms =
        observed ? (playback.duration_100ns > 0 ? static_cast<u64>(playback.duration_100ns / 10000) : 0)
                 : video.audio_declared_duration_ms;
    out.has_duration = observed ? out.duration_ms != 0 : video.audio_duration_known;
    out.volume =
        observed && playback.volume_valid ? static_cast<u32>(playback.applied_volume * 100 + .5f) : 0;
    out.playing = observed && playback.playing;
    out.muted = observed && playback.muted;
    out.error = video.presentation_failed || playback.error != AccountVideoError::none ||
                playback.rejected_error != AccountVideoError::none ||
                (video.selected && video.status.selected && !observed);
    // Source readiness owns loading. Once observed, an ongoing seek/buffer
    // retains the real range and its capture; terminal errors own Retry.
    out.loading =
        !out.error && (video.selecting || (!observed && (playback.buffering || playback.seeking)));
    out.can_play = !video.selecting && !playback.release_pending && (observed || out.error);
    out.can_seek = observed && out.has_duration && !out.error && !playback.release_pending;
    out.can_volume =
        observed && playback.has_audio && playback.volume_valid && !out.error && !playback.release_pending;
    out.can_download = true;
    out.voice = video.audio_voice;
    out.title = {video.audio_title, video.audio_title_bytes};
    out.waveform = {video.audio_waveform, video.audio_waveform_bytes};
    return out;
}

} // namespace
// Session start and stop, viewer opening, activation and control events: user-rate, not per frame. The
// per-frame playback state above and positioning below stay at full speed.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
namespace video_detail {

bool send_video_command(ApplicationVideo& video, AccountVideoCommand kind, i64 seek, float volume,
                        bool muted) noexcept {
    if (!video.selected || !video_session_available())
        return false;
    if (video.status.playback.live && kind == AccountVideoCommand::seek) return false;
    if (video.status.playback.live && kind == AccountVideoCommand::play && video.live_started && !video.status.playback.playing) {
        video.live_restart = true;
        video.live_restarts = 0;
        application_request_frame();
        return true;
    }
    const AccountVideoControl command{
        video.status.playback.identity, ++video.command_serial, seek, volume, kind, muted};
    const auto result = g_app.account.api.video_control(g_app.account.instance, &command);
    const bool admitted = result.ok() && result.admitted;
    if (admitted && (kind == AccountVideoCommand::play || kind == AccountVideoCommand::pause))
        video.collection_continue = kind == AccountVideoCommand::play;
    if (admitted && kind == AccountVideoCommand::play && video.status.playback.live) video.live_started = true;
    return admitted;
}
bool set_video_expanded(ApplicationVideo& video, bool expanded) noexcept {
    if (video.source.audio || expanded == video.expanded)
        return false;
    application_video_cancel_pointer();
    if (!expanded) {
        // The viewer animates back to the preview, then detaches this session
        // so it resumes its inline geometry at the same position.
        application_image_viewer_dismiss();
        application_request_frame();
        return true;
    }
    if (!application_image_viewer_open_video(video.source.message_id, video.source.revision,
                                             video.source.embed, video.source.index, video.source.snapshot,
                                             media_asset_search(video.source.key) ? &video.source.search_source
                                                                                  : nullptr,
                                             video.source.key, video.source.preview))
        return false;
    video.expanded = true;
    acquire_video_focus(video);
    ui_video_scene_activity(&video.ui.video, clock_monotonic_milliseconds());
    application_request_frame();
    return true;
}

void dispatch_video_event(ApplicationVideo& video, const UiVideoSceneResult& event) noexcept {
    apply_scene_result(event);
    if (!event.action_requested || !ui_video_action_current(project_video_playback(video), event.action))
        return;
    if (inline_media_activation_consumed(video.source))
        return;
    const auto& action = event.action;
    switch (action.control) {
    case UiVideoControl::play_pause:
        send_video_command(video, action.value ? AccountVideoCommand::play : AccountVideoCommand::pause);
        break;
    case UiVideoControl::seek:
        if (action.position_ms <= static_cast<u64>(0x7fffffffffffffffll / 10000) &&
            !send_video_command(video, AccountVideoCommand::seek,
                                static_cast<i64>(action.position_ms * 10000)))
            video.ui.video.capture_value_valid = false;
        break;
    case UiVideoControl::volume:
        if (!send_video_command(video, AccountVideoCommand::volume, 0, action.volume / 100.0f))
            video.ui.video.capture_value_valid = false;
        break;
    case UiVideoControl::mute:
        send_video_command(video, AccountVideoCommand::mute, 0, 1, action.value);
        break;
    case UiVideoControl::previous_track:
    case UiVideoControl::next_track:
    case UiVideoControl::track:
        switch_collection_track(video, action.collection_track, action.control);
        return;
    case UiVideoControl::expand:
        set_video_expanded(video, action.value);
        return;
    default:
        break;
    }
    application_request_frame();
}

bool start_video_session(const InlineMediaSource& source) noexcept {
    AccountVideoIdentity retired{};
    close_video_session(&retired);
    void* allocation =
        commit_pages(sizeof(ApplicationVideo));
    if (!allocation)
        return false;
    auto* video = source.audio ? new (allocation) ApplicationVideo(InlineAudioSceneSelection{})
                               : new (allocation) ApplicationVideo(InlineVideoSceneSelection{});
    g_app.video = video;
    video->source = source;
    if (!capture_audio_metadata(*video)) {
        video->~ApplicationVideo();
        pages_release(video);
        g_app.video = nullptr;
        return false;
    }
    video->retiring_identity = retired;
    video->account_serial = application_account_scope();
    video->session_epoch = ++g_app.video_lifetime_serial;
    video->selecting = true;
    video->requested_play = true;
    video->requested_seek = source.start_seconds != 0;
    acquire_video_focus(*video);
    application_request_frame();
    return true;
}
bool switch_collection_track(ApplicationVideo& video, u16 track, UiVideoControl focus) noexcept {
    if (!track || track > video.collection_count || track == video.collection_track || video.selecting) return false;
    const auto source = video.source;
    const u16 count = video.collection_count;
    const bool more = video.collection_more;
    const bool resume = video.collection_continue;
    const bool muted = video.status.playback.muted;
    const float volume = video.status.playback.volume_valid ? video.status.playback.desired_volume : 1;
    if (!start_video_session(source)) return false;
    g_app.video->collection_count = count;
    g_app.video->collection_more = more;
    g_app.video->collection_track = track;
    g_app.video->collection_continue = resume;
    g_app.video->collection_focus = focus;
    g_app.video->requested_play = resume;
    g_app.video->requested_seek = false;
    g_app.video->restore_gain = true;
    g_app.video->restore_volume = volume;
    g_app.video->restore_muted = muted;
    return true;
}
bool restart_live_session(ApplicationVideo& video) noexcept {
    // Copy before close: the new selection waits for joined disposal of the
    // previous session, while keeping viewer geometry and the user's sound settings.
    const auto source = video.source;
    const bool expanded = video.expanded, muted = video.status.playback.muted;
    const float volume = video.status.playback.desired_volume;
    const u8 retries = video.live_restarts;
    if (!start_video_session(source)) return false;
    g_app.video->expanded = expanded;
    g_app.video->status.playback.live = true;
    g_app.video->restore_gain = true;
    g_app.video->restore_muted = muted;
    g_app.video->restore_volume = volume;
    g_app.video->live_restarts = retries;
    return true;
}
bool inline_media_activation_consumed(const InlineMediaSource& source, InlineMediaPolicyUse use) noexcept {
    // Missing sources keep the pump's existing pause/retirement path. Retention
    // changes only when a current media item has an obscuring policy verdict.
    if (!g_app.account_ui)
        return use != InlineMediaPolicyUse::retain;
    ApplicationSearchSourceView searched{};
    const bool search = source.search_source.publication_revision != 0;
    const MessageRecord* message = nullptr;
    const MessageRichStore* store = &g_app.account_ui->message_rich_store;
    if (search) {
        if (!application_search_source_resolve(source.search_source, false, &searched))
            return use != InlineMediaPolicyUse::retain;
        message = searched.message;
        store = searched.rich_store;
    } else {
        const auto found = message_store_find(g_app.message_view.store, source.message_id);
        message = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    }
    auto* searched_scene = search ? application_search_scene() : nullptr;
    auto* scene = search ? (searched_scene && searched_scene == g_app.renderer.search_scene
                               ? ui_search_scene_message_scene(searched_scene)
                               : nullptr)
                        : &g_app.message_scene;
    const auto* outer = message && store ? message_rich_store_find(*store, message->id) : nullptr;
    const auto* rich =
        source.snapshot && store ? message_rich_store_snapshot(*store, source.message_id) : outer;
    if (!scene || !message || !rich || message->revision != source.revision ||
        rich->message_revision != source.revision)
        return use != InlineMediaPolicyUse::retain;
    const auto owner = message_media_owner(*message, outer);
    const auto kind = source.embed ? MessageMediaSource::embed : MessageMediaSource::attachment;
    if ((source.embed && source.index >= rich->embed_count) ||
        (!source.embed && source.index >= rich->attachment_count))
        return use != InlineMediaPolicyUse::retain;
    const auto key = message_media_reveal_key(message->id, kind, source.index, source.snapshot);
    const bool spoiler =
        !source.embed && message_rich_attachment_spoiler(*store, rich->attachments[source.index]);
    const bool explicit_media = source.embed
                                    ? message_rich_embed_explicit(rich->embeds[source.index])
                                    : message_rich_attachment_explicit(rich->attachments[source.index]);
    if (use != InlineMediaPolicyUse::activate) {
        const auto obscure =
            message_scene_media_obscure(*scene, owner, message->id, key, spoiler, explicit_media);
        return use == InlineMediaPolicyUse::retain ? obscure != MessageMediaObscure::none
                                                   : message_media_obscure_notice(obscure);
    }
    return application_media_activation_consumed(*scene, owner, message->id, key, spoiler, explicit_media);
}
bool activate_inline_source(const InlineMediaSource& source) noexcept {
    if (inline_media_activation_consumed(source))
        return true;
    auto* video = g_app.video;
    if (video && media_asset_key_equal(source.key, video->source.key) &&
        source.audio == video->source.audio && source.snapshot == video->source.snapshot &&
        source.message_id == video->source.message_id && source.revision == video->source.revision &&
        source.search_source.index == video->source.search_source.index && video->selected &&
        video->status.selected && !video->presentation_failed &&
        video->status.playback.error == AccountVideoError::none &&
        video->status.playback.rejected_error == AccountVideoError::none) {
        acquire_video_focus(*video);
        if (source.audio)
            ui_audio_scene_focus(&video->ui.audio, UiAudioControl::play_pause, false);
        else
            ui_video_scene_focus(&video->ui.video, UiVideoControl::play_pause, false);
        send_video_command(*video, video->status.playback.playing ? AccountVideoCommand::pause
                                                                  : AccountVideoCommand::play);
        application_request_frame();
        return true;
    }
    return start_video_session(source);
}
void dispatch_audio_event(ApplicationVideo& video, const UiAudioSceneResult& event) noexcept {
    apply_scene_result(event);
    if (!event.action_requested || !video.source.audio ||
        !ui_audio_action_current(project_audio_playback(video), event.action))
        return;
    InlineMediaSource current{};
    if (!resolve_visible_video_source(video.source.message_id, video.source.revision, false,
                                      video.source.index, &current, video.source.snapshot,
                                      &video.source.search_source) ||
        !current.audio || !media_asset_key_equal(current.key, video.source.key) ||
        current.owner.manager != video.ui.audio.owner.manager ||
        !ui_widget_handle_equal(current.owner.handle, video.ui.audio.owner.handle))
        return;
    if (inline_media_activation_consumed(current))
        return;
    switch (event.action.control) {
    case UiAudioControl::play_pause:
        if (project_audio_playback(video).error) {
            start_video_session(current);
            return;
        }
        send_video_command(video,
                           event.action.value ? AccountVideoCommand::play : AccountVideoCommand::pause);
        break;
    case UiAudioControl::seek:
        if (event.action.position_ms <= static_cast<u64>(0x7fffffffffffffffll / 10000))
            send_video_command(video, AccountVideoCommand::seek,
                               static_cast<i64>(event.action.position_ms * 10000));
        break;
    case UiAudioControl::volume:
        send_video_command(video, AccountVideoCommand::volume, 0, event.action.volume / 100.f);
        break;
    case UiAudioControl::mute:
        send_video_command(video, AccountVideoCommand::mute, 0, 1, event.action.value);
        break;
    case UiAudioControl::download:
        if (media_asset_search(current.key))
            application_downloads_begin_search(current.search_source, current.key.id, current.snapshot);
        else
            application_downloads_begin(current.message_id, current.key.id, current.snapshot);
        break;
    default:
        break;
    }
    application_request_frame();
}
// A search-hosted player is valid only while its typed rich target still resolves to the same media;
// otherwise the session closes.
bool search_video_still_current(const ApplicationVideo& video) noexcept {
    if (!media_asset_search(video.source.key))
        return true;
    InlineMediaSource current{};
    if (resolve_search_video_source(video.source.search_source, video.source.embed, video.source.index,
                                    video.source.snapshot, &current) &&
        media_asset_key_equal(current.key, video.source.key))
        return true;
    application_video_close();
    return false;
}
// The first visible inline media source whose clip holds `point`, with its rich record and item index.
bool visible_source_at(UiPoint point, InlineMediaSource* source, const MessageRichRecord** rich_out,
                       u32* item_out) noexcept {
    for (u32 row_index = 0; row_index < g_app.message_scene.count; ++row_index) {
        for (u32 content_scope = 0; content_scope < 2; ++content_scope) {
            const auto& row = g_app.message_scene.rows[row_index];
            const auto* message = message_store_resolve(g_app.message_view.store, row.message);
            const auto& store = g_app.account_ui->message_rich_store;
            const auto* rich = message ? (content_scope ? message_rich_store_snapshot(store, message->id)
                                                        : message_rich_store_find(store, message->id))
                                       : nullptr;
            if (!rich)
                continue;
            for (u32 kind = 0; kind < 2; ++kind) {
                const u32 count = kind ? rich->embed_count : rich->attachment_count;
                for (u32 item = 0; item < count; ++item) {
                    if (resolve_visible_video_source(message->id, message->revision, kind != 0,
                                                     static_cast<u8>(item), source, content_scope != 0) &&
                        ui_rect_contains(source->clip, point.x, point.y)) {
                        *rich_out = rich;
                        *item_out = item;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}
// Routes one pointer event into the video scene; true when a control handled it.
bool video_scene_pointer_event(ApplicationVideo& video, VideoPointer pointer, UiPoint point) noexcept {
    auto& scene = video.ui.video;
    const auto before = ui_video_scene_paint_revision(scene);
    note_video_pointer(video, pointer, point);
    const auto kind = pointer == VideoPointer::down ? UiVideoPointerEvent::down
                      : pointer == VideoPointer::up ? UiVideoPointerEvent::up
                                                    : UiVideoPointerEvent::move;
    const auto result = ui_video_scene_pointer(&scene, kind, point);
    if (before != ui_video_scene_paint_revision(scene))
        application_request_frame();
    if (!result.handled)
        return false;
    if (pointer == VideoPointer::down)
        acquire_video_focus(video);
    dispatch_video_event(video, result);
    return true;
}

} // namespace video_detail

bool application_video_ordinary(const MediaAssetKey& key) noexcept {
    if (key.kind == MediaAssetKind::attachment)
        return media_asset_attachment_format(key) == MediaAttachmentFormat::mp4 ||
               media_attachment_format_audio_supported(media_asset_attachment_format(key));
    if (key.kind != MediaAssetKind::embed ||
        (media_asset_embed_format(key) != MediaAttachmentFormat::mp4 && !media_embed_audio_format_supported(media_asset_embed_format(key))) ||
        !g_app.account_ui)
        return false;
    ApplicationSearchSourceView searched{};
    const auto* rich = media_asset_search(key)
                           ? (application_search_source_resolve(key.id, media_asset_search_revision(key),
                                                                media_asset_embed_snapshot(key), &searched)
                                  ? searched.rich
                                  : nullptr)
                       : media_asset_embed_snapshot(key)
                           ? message_rich_store_snapshot(g_app.account_ui->message_rich_store, key.id)
                           : message_rich_store_find(g_app.account_ui->message_rich_store, key.id);
    const u32 index = media_asset_embed_index(key);
    return rich && index < rich->embed_count && rich->embeds[index].type != GatewayMessageEmbedType::gifv;
}
bool application_video_search_active() noexcept {
    return g_app.video && media_asset_search(g_app.video->source.key);
}
bool application_video_open_search(const UiSearchTarget& target) noexcept {
    if (!video_surface_available() || !d3d11_renderer_search_visible(g_app.renderer) ||
        target.action != UiSearchAction::rich_item || target.save || target.rich_index > 255 ||
        (target.rich_kind != MessageSceneRichItemKind::attachment &&
         target.rich_kind != MessageSceneRichItemKind::embed) ||
        !ui_search_scene_target_current(*g_app.renderer.search_scene, target))
        return false;
    const bool embed = target.rich_kind == MessageSceneRichItemKind::embed;
    if (embed && target.resource == MediaEmbedResource::none)
        return false;
    ApplicationSearchSourceTicket ticket{};
    if (!application_search_source_ticket(target.source_index, &ticket) ||
        ticket.message_id != target.message_id)
        return false;
    InlineMediaSource source{};
    if (!resolve_search_video_source(ticket, embed, static_cast<u8>(target.rich_index), target.snapshot,
                                     &source) ||
        source.channel_id != target.channel_id ||
        (embed && media_asset_embed_resource(source.key) != target.resource &&
         !(source.provider && (target.resource == MediaEmbedResource::thumbnail ||
                               target.resource == MediaEmbedResource::image))))
        return false;
    return activate_inline_source(source);
}
bool application_video_open_focused() noexcept {
    if (!video_surface_available())
        return false;
    if (d3d11_renderer_search_visible(g_app.renderer)) {
        const auto& search = *g_app.renderer.search_scene;
        const auto* scene = ui_search_scene_message_scene(search);
        if (scene && g_app.context_focus == &scene->widgets) {
            const auto target = ui_search_scene_rich_widget_target(search, scene->widgets.focused);
            ApplicationSearchSourceTicket ticket{};
            InlineMediaSource source{};
            if (target.action == UiSearchAction::rich_item &&
                target.rich_kind == MessageSceneRichItemKind::attachment && target.rich_index <= 255 &&
                application_search_source_ticket(target.source_index, &ticket) &&
                resolve_search_video_source(ticket, false, static_cast<u8>(target.rich_index),
                                            target.snapshot, &source) &&
                source.audio)
                return application_video_open_search(target);
        }
    }
    const auto& scene = g_app.message_scene;
    const auto focused = scene.widgets.focused;
    if (!ui_widget_manager_resolve(scene.widgets, focused))
        return false;
    for (u32 i = 0; i < scene.count; ++i) {
        const auto& outer = scene.rows[i];
        const auto* message = message_store_resolve(g_app.message_view.store, outer.message);
        if (!message)
            continue;
        for (u32 scope = 0; scope < 2; ++scope) {
            const auto* row = scope ? message_scene_snapshot_row(scene, outer) : &outer;
            if (!row)
                continue;
            for (u32 item = 0; item < row->attachment_item_count; ++item) {
                const auto widget =
                    message_scene_rich_item_widget(*row, MessageSceneRichItemKind::attachment, item);
                if (!ui_widget_handle_equal(widget, focused))
                    continue;
                InlineMediaSource source{};
                return resolve_visible_video_source(message->id, message->revision, false,
                                                    static_cast<u8>(item), &source, scope != 0) &&
                       source.audio && ui_rect_has_area(source.clip) && activate_inline_source(source);
            }
        }
    }
    return false;
}
void application_video_cancel_pointer() noexcept {
    if (!g_app.video)
        return;
    if (g_app.video->source.audio) {
        auto& scene = g_app.video->ui.audio;
        const auto before = ui_audio_scene_paint_revision(scene);
        const auto result = ui_audio_scene_pointer(&scene, UiAudioPointerEvent::cancel, {});
        if (result.release_capture)
            application_release_capture();
        if (before != ui_audio_scene_paint_revision(scene))
            application_request_frame();
        return;
    }
    auto& scene = g_app.video->ui.video;
    const auto before = ui_video_scene_paint_revision(scene);
    const auto result = ui_video_scene_pointer(&scene, UiVideoPointerEvent::cancel, {});
    if (result.release_capture)
        application_release_capture();
    if (before != ui_video_scene_paint_revision(scene))
        application_request_frame();
}
void application_video_blur() noexcept {
    auto* video = g_app.video;
    if (!video)
        return;
    if (video->source.audio) {
        const auto before = ui_audio_scene_paint_revision(video->ui.audio);
        video->input_focus = false;
        application_video_cancel_pointer();
        ui_audio_scene_blur(&video->ui.audio);
        if (before != ui_audio_scene_paint_revision(video->ui.audio))
            application_request_frame();
        return;
    }
    const auto before = ui_video_scene_paint_revision(video->ui.video);
    video->input_focus = false;
    application_video_cancel_pointer();
    ui_video_scene_blur(&video->ui.video);
    if (before != ui_video_scene_paint_revision(video->ui.video))
        application_request_frame();
}

namespace video_detail {

void close_video_session(AccountVideoIdentity* retired_identity) noexcept {
    if (retired_identity)
        reset_value(retired_identity);
    auto* video = g_app.video;
    if (!video)
        return;
    application_video_cancel_pointer();
    const bool had_scene = unbind_playback_scenes();
    if (video_session_available() && video->account_serial == application_account_scope() &&
        (video->selected || video->selecting)) {
        AccountVideoFrame frame{};
        if (g_app.account.api.video_take(g_app.account.instance, &frame))
            g_app.account.api.video_acknowledge(g_app.account.instance, &frame);
        if (retired_identity)
            *retired_identity = video->retiring_identity;
        if (video->selected && send_video_command(*video, AccountVideoCommand::stop) && retired_identity)
            *retired_identity = video->status.playback.identity;
    }
    g_app.renderer.retained_presentation.valid = false;
    g_app.video = nullptr;
    video->~ApplicationVideo();
    pages_release(video);
    if (had_scene && g_app.renderer_ready &&
        !renderer_rebuild_content_text(&g_app.renderer, application_theme()))
        log_write(LogLevel::warning, "media", "video_controls_text_retirement_failed");
}

} // namespace video_detail

void application_video_close() noexcept {
    close_video_session(nullptr);
}
void application_video_viewer_detach() noexcept {
    auto* video = g_app.video;
    if (!video || !video->expanded)
        return;
    application_video_cancel_pointer();
    video->expanded = false;
    application_request_frame();
}
bool application_video_open_viewer_item(u64 message_id, u32 revision, bool embed, u8 index, bool snapshot,
                                        const ApplicationSearchSourceTicket* search, const UiRect& preview,
                                        MediaAssetKey* key) noexcept {
    if (!key || !video_session_available() || !ui_rect_has_area(preview))
        return false;
    InlineMediaSource candidate{};
    candidate.message_id = message_id;
    candidate.revision = revision;
    candidate.embed = embed;
    candidate.index = index;
    candidate.snapshot = snapshot;
    if (search)
        candidate.search_source = *search;
    if (inline_media_activation_consumed(candidate))
        return false;
    auto* video = g_app.video;
    if (video && !video->source.audio && video->source.message_id == message_id &&
        video->source.revision == revision && video->source.embed == embed && video->source.index == index &&
        video->source.snapshot == snapshot && media_asset_search(video->source.key) == (search != nullptr)) {
        video->expanded = true;
        *key = video->source.key;
        application_request_frame();
        return true;
    }
    const MessageRecord* message = nullptr;
    const MessageRichRecord* rich = nullptr;
    const MessageRichStore* store = &g_app.account_ui->message_rich_store;
    ApplicationSearchSourceView searched{};
    if (search) {
        if (!application_search_source_resolve(*search, snapshot, &searched))
            return false;
        message = searched.message;
        rich = searched.rich;
        store = searched.rich_store;
    } else {
        const auto found = message_store_find(g_app.message_view.store, message_id);
        message = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
        rich = message ? (snapshot ? message_rich_store_snapshot(*store, message_id)
                                   : message_rich_store_find(*store, message_id))
                       : nullptr;
    }
    if (!message || !rich || !store || message->id != message_id || message->revision != revision ||
        rich->message_revision != revision || !application_channel_readable(g_app, message->channel_id))
        return false;
    // Reuse the inline poster tier so the viewer shows the cached first frame
    // until decoded frames replace it under the same key.
    const auto tier = [](u32 source_width, u32 source_height) noexcept {
        float width = 400, height = 225;
        if (source_width && source_height) {
            const float scale = minimum(1.0f, minimum(550.0f / source_width, 350.0f / source_height));
            width = source_width * scale;
            height = source_height * scale;
        }
        return media_presentation_request(source_width, source_height, width, height, g_app.renderer.dpi,
                                          media_preview_extent_cap(MediaAttachmentFormat::mp4))
            .resolution;
    };
    MediaAssetKey selected{};
    bool provider = false;
    u32 start_seconds = 0;
    if (!embed) {
        if (index >= rich->attachment_count ||
            message_rich_attachment_format(*store, rich->attachments[index]) != MediaAttachmentFormat::mp4)
            return false;
        const auto& attachment = rich->attachments[index];
        selected = search ? media_asset_search_attachment(attachment.id, MediaAttachmentFormat::mp4,
                                                          MediaResolutionTier::px512, revision, snapshot)
                          : media_asset_attachment(attachment.id, MediaAttachmentFormat::mp4,
                                                   tier(attachment.width, attachment.height));
    } else {
        MessageRichEmbedMediaSelection media{};
        if (index >= rich->embed_count || rich->embeds[index].type == GatewayMessageEmbedType::gifv ||
            !message_rich_embed_playback_select(*store, rich->embeds[index], &media) ||
            media.format != MediaAttachmentFormat::mp4)
            return false;
        provider = media.provider;
        start_seconds = media.start_seconds;
        selected = search ? media_asset_search_embed(message_id, revision, index, media.resource, media.format,
                                                     media.animated, MediaResolutionTier::px512, snapshot)
                          : media_asset_embed(message_id, revision, index, media.resource, media.format,
                                              media.animated, tier(media.media->width, media.media->height),
                                              snapshot);
    }
    if (!media_asset_key_valid(selected))
        return false;
    InlineMediaSource source{selected, preview, preview, message_id, message->channel_id, revision, index,
                             embed,    snapshot};
    source.provider = provider;
    source.start_seconds = start_seconds;
    if (search)
        source.search_source = *search;
    if (!start_video_session(source))
        return false;
    g_app.video->expanded = true;
    *key = selected;
    return true;
}

#pragma clang attribute pop
namespace video_detail {

bool position_video_scene(ApplicationVideo* video, InlineMediaSource& positioned) noexcept {
    if (video->expanded && !g_app.image_viewer.scene.active)
        video->expanded = false;
    bool visible = false;
    if (video->expanded) {
        // The viewer owns geometry; message/revision authority was checked above.
        positioned = video->source;
        visible = video_surface_available() &&
                  application_image_viewer_video_geometry(video->source.key, &positioned.preview,
                                                          &positioned.clip) &&
                  ui_rect_has_area(positioned.clip);
    } else {
        visible =
            video_surface_available() &&
            resolve_visible_video_source(video->source.message_id, video->source.revision,
                                         video->source.embed, video->source.index, &positioned,
                                         video->source.snapshot, &video->source.search_source) &&
            ui_rect_has_area(positioned.clip) && media_asset_key_equal(positioned.key, video->source.key) &&
            positioned.audio == video->source.audio;
    }
    return visible;
}

} // namespace video_detail
namespace {

void refresh_video_text() noexcept {
    if (!renderer_rebuild_content_text(&g_app.renderer, application_theme())) {
        // Text/layout rejection does not invalidate the account or media owner.
        // Leave its revision pending and use the ordinary presentation retry.
        application_record_presentation_failure("video_text_projection_failed");
        application_request_frame();
    }
}

} // namespace
namespace video_detail {

bool update_video_extent(ApplicationVideo& video, const GpuVideoFrame& frame) noexcept {
    if (!video.source.provider || !video.source.embed)
        return false;
    const u32 width = gpu_video_frame_output_width(frame, 1);
    const u32 height = gpu_video_frame_output_height(frame, 1);
    if (!width || !height || (video.display_width == width && video.display_height == height))
        return false;
    ApplicationSearchSourceView searched{};
    auto* store = &g_app.account_ui->message_rich_store;
    if (media_asset_search(video.source.key)) {
        if (!application_search_source_resolve(video.source.search_source, video.source.snapshot, &searched))
            return false;
        store = searched.rich_store;
    }
    bool changed = false;
    if (!message_rich_store_set_embed_dimensions(store, video.source.message_id, video.source.revision,
            video.source.index, MessageRichEmbedMediaSlot::video, width, height, &changed,
            video.source.snapshot))
        return false;
    video.display_width = width;
    video.display_height = height;
    if (changed) {
        if (media_asset_search(video.source.key))
            application_search_media_dimensions_changed();
        else
            message_scene_invalidate_geometry(&g_app.message_scene);
        g_app.renderer.retained_presentation.valid = false;
    }
    return changed;
}
// The controls' scene projection: the UI library's own work (compiled for size there), here once per pump
// turn of an open player, so it is compiled for size with what it inlines from that library. Positioning
// stays at full speed.
__attribute__((minsize)) bool project_video_scene(ApplicationVideo* video, const InlineMediaSource& positioned,
                                                  bool visible, bool* repaint) noexcept {
    const bool searched = media_asset_search(video->source.key);
    if (visible)
        video->hidden_pause = false;
    if (!visible) {
        video->presentation_wake_ms = 0;
        application_video_blur();
        if (video->source.audio)
            video->ui.audio.active = false;
        else {
            ui_opacity_transition_reset(&video->ui.video.controls_transition);
            video->ui.video.active = false;
        }
        if (unbind_playback_scenes())
            refresh_video_text();
        return true;
    }
    video->source = positioned;
    const MediaAssetKey inline_key = positioned.provider && video->provider_frame_ready && !video->expanded
                                        ? positioned.key : MediaAssetKey{};
    if (!media_asset_key_equal(g_app.renderer.inline_embed_video, inline_key)) {
        g_app.renderer.inline_embed_video = inline_key;
        g_app.renderer.retained_presentation.valid = false;
    }
    if (video->source.audio) {
        const auto before = ui_audio_scene_paint_revision(video->ui.audio);
        const auto capture = video->ui.audio.capture;
        const bool projected = ui_audio_scene_project(
            &video->ui.audio, project_audio_playback(*video), positioned.preview, positioned.clip,
            positioned.owner, searched ? UiAudioSourceSurface::search : UiAudioSourceSurface::timeline,
            kDefaultUiAudioControlsStyle, application_theme());
        video->presentation_wake_ms = 0;
        if (projected) {
            if (video->input_focus && !video_input_focus_current(*video))
                application_video_blur();
            if (video_input_focus_current(*video) && video->ui.audio.focus == UiAudioControl::none)
                ui_audio_scene_focus(&video->ui.audio, UiAudioControl::play_pause,
                                     g_app.keyboard_focus_visible);
            video->ui.audio.keyboard_focus =
                video_input_focus_current(*video) && g_app.keyboard_focus_visible;
            ui_audio_scene_hover(&video->ui.audio, video_pointer_position());
            if (!renderer_bind_audio_scene(&g_app.renderer, &video->ui.audio))
                return false;
            if (capture != UiAudioControl::none && video->ui.audio.capture == UiAudioControl::none)
                application_release_capture();
            if (g_app.renderer.interaction_text_audio_revision != ui_audio_scene_text_revision(video->ui.audio))
                refresh_video_text();
        } else {
            renderer_bind_audio_scene(&g_app.renderer, nullptr);
            if (capture != UiAudioControl::none)
                application_release_capture();
        }
        if (repaint && before != ui_audio_scene_paint_revision(video->ui.audio))
            *repaint = true;
        return true;
    }
    renderer_bind_audio_scene(&g_app.renderer, nullptr);
    const u64 old_paint_revision = ui_video_scene_paint_revision(video->ui.video);
    UiVideoControlsStyle metrics{};
    if (searched) {
        // Result cards and forwarded padding leave less width than timeline
        // media. The existing compact controls still fit at their exact layout
        // minimum without shrinking any button, range, label or inset.
        metrics.minimum_width = 2 * metrics.padding + 2 * metrics.target + 3 * metrics.gap +
                                metrics.time_width + metrics.range_height;
    }
    UiVideoSceneStyle visual{};
    if (video->expanded) {
        // Viewer media is square-cornered and hides idle controls sooner.
        visual.preview_radius = 0;
        visual.idle_ms = kUiVideoControlsExpandedIdleMs;
        visual.scrim_lead = 40;
    }

    const float usable_width = ui_rect_width(positioned.preview) - 2 * visual.inset;
    const float height = ui_video_controls_height(metrics, usable_width, video->collection_count != 0);
    const auto before_capture = video->ui.video.capture;
    const bool full = usable_width >= metrics.minimum_width &&
                      ui_rect_height(positioned.preview) >= height + 2 * visual.inset;
    const bool projected =
        full ? ui_video_scene_project(&video->ui.video, project_video_playback(*video), positioned.preview,
                                      positioned.clip, metrics, visual, application_theme())
             : ui_video_scene_project_play_only(&video->ui.video, project_video_playback(*video),
                                                positioned.preview, positioned.clip, metrics, visual, {},
                                                application_theme());
    if (projected) {
        const u64 now_ms = clock_monotonic_milliseconds();
        if (video->input_focus && !video_input_focus_current(*video))
            application_video_blur();
        if (video_input_focus_current(*video) && video->ui.video.focus == UiVideoControl::none) {
            if (!ui_video_scene_focus(&video->ui.video, video->collection_focus, g_app.keyboard_focus_visible))
                ui_video_scene_focus(&video->ui.video, UiVideoControl::play_pause, g_app.keyboard_focus_visible);
        }
        video->ui.video.keyboard_focus = video_input_focus_current(*video) && g_app.keyboard_focus_visible;
        ui_video_scene_hover(&video->ui.video, video_pointer_position());
        const bool was_idle = video->ui.video.idle;
        ui_video_scene_tick(&video->ui.video, now_ms, g_app.preferences.reduced_motion);
        if (video->collection_count && g_app.account.api.collection_title) {
            MediaTrackTitle current{}, hovered{};
            AccountMediaCollectionTitleRequest query{video->source.key, video->source.revision, video->collection_track, true};
            g_app.account.api.collection_title(g_app.account.instance, &query, &current);
            auto& scene = video->ui.video;
            if (scene.hover_track) {
                query.track = scene.hover_track;
                query.fetch = scene.track_hover_since_ms && now_ms >= scene.track_hover_since_ms + 150;
                const auto result = g_app.account.api.collection_title(g_app.account.instance, &query, &hovered);
                if (query.fetch || result.state == AccountMediaRequestState::ready || result.state == AccountMediaRequestState::retry_wait) scene.track_hover_requested = true;
            }
            ui_video_scene_titles(&scene, {current.text, current.bytes}, scene.hover_track, {hovered.text, hovered.bytes});
        }
        // Idle viewer playback hides the pointer with its controls; motion restores it.
        if (video->expanded && !was_idle && video->ui.video.idle && video->ui.video.preview_hovered)
            application_set_cursor(UiCursor::hidden);
        video->presentation_wake_ms =
            ui_video_scene_next_wake(video->ui.video, now_ms, g_app.preferences.reduced_motion);
        g_app.renderer.video_scene = &video->ui.video;
        g_app.renderer.video_scene_search_result = searched && !video->expanded;
        g_app.renderer.video_scene_viewer = video->expanded;
        if (before_capture != UiVideoControl::none && video->ui.video.capture == UiVideoControl::none)
            application_release_capture();
        if (g_app.renderer.interaction_text_video_revision != ui_video_scene_text_paint_revision(video->ui.video))
            refresh_video_text();
    } else {
        video->presentation_wake_ms = 0;
        g_app.renderer.video_scene = nullptr;
        g_app.renderer.video_scene_search_result = false;
        g_app.renderer.video_scene_viewer = false;
        if (before_capture != UiVideoControl::none)
            application_release_capture();
    }
    if (repaint && old_paint_revision != ui_video_scene_paint_revision(video->ui.video))
        *repaint = true;
    return true;
}

} // namespace video_detail

bool application_video_synchronize() noexcept {
    auto* video = g_app.video;
    if (!video) return true;
    InlineMediaSource positioned{};
    const bool visible = position_video_scene(video, positioned);
    return project_video_scene(video, positioned, visible, nullptr);
}

namespace video_detail {

// One copy of the host's status for every refresh.
__declspec(noinline) void refresh_video_status(ApplicationVideo& video) noexcept {
    video.status = g_app.account.api.video_status(g_app.account.instance);
}

} // namespace video_detail
} // namespace maicord::app_detail
