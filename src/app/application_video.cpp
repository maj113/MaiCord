#include "application_downloads.hpp"
#include "application_content_point.hpp"
#include "application_video.hpp"
#include "application_frame_presentation.hpp"
#include "application_account_presentation.hpp"
#include "application_call.hpp"
#include "application_video_request.hpp"
#include "application_state.hpp"
#include "application_channel_authority.hpp"
#include "application_settings.hpp"
#include "application_friends.hpp"
#include "application_composer_text_input.hpp"
#include "application_search.hpp"
#include "application_image_viewer.hpp"
#include "application_media_policy.hpp"
#include "application_cursor.hpp"
#include "maicord/media/presentation_policy.hpp"
#include "maicord/media/gpu_video_frame.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/ui/audio_presentation.hpp"
#include "maicord/ui/scene_cursor.hpp"
#include "../render/d3d11_renderer_interaction_state.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/app/account_video_playback.hpp"

namespace maicord::app_detail {
struct InlineMediaSource {
    MediaAssetKey key{};
    UiRect preview{};
    UiRect clip{};
    u64 message_id{};
    u64 channel_id{};
    u32 revision{};
    u8 index{};
    bool embed{};
    bool snapshot{};
    ApplicationSearchSourceTicket search_source{};
    UiWidgetReference owner{};
    bool audio{};
    bool provider{};
    u32 start_seconds{};
    bool audio_preview{};
};
struct InlineAudioSceneSelection {};
struct InlineVideoSceneSelection {};
union InlinePlaybackSceneStorage {
    UiVideoScene video;
    UiAudioScene audio;
    explicit InlinePlaybackSceneStorage(InlineAudioSceneSelection) noexcept : audio{} {
    }
    explicit InlinePlaybackSceneStorage(InlineVideoSceneSelection) noexcept : video{} {
    }
    ~InlinePlaybackSceneStorage() noexcept {
    }
};
struct ApplicationVideo {
    InlinePlaybackSceneStorage ui;
    char audio_title[512]{};
    u8 audio_waveform[kUiAudioWaveformMaximum]{};
    u32 audio_title_bytes{}, audio_waveform_bytes{}, audio_declared_duration_ms{};
    bool audio_duration_known{}, audio_voice{};
    InlineMediaSource source{};
    AccountVideoPlaybackStatus status{};
    AccountVideoIdentity retiring_identity{};
    u64 account_serial{};
    u64 session_epoch{};
    u64 command_serial{};
    u64 presentation_wake_ms{};
    bool selecting{};
    bool selected{};
    bool requested_play{};
    bool requested_seek{};
    bool provider_frame_ready{};
    u32 display_width{}, display_height{};
    bool hidden_pause{};
    bool input_focus{};
    bool presentation_failed{};
    bool pointer_leave_armed{};
    // The media lane reported an expired signed URL; the next source request forces a refresh.
    bool source_refresh{};
    // The media viewer hosts this same session; geometry comes from the viewer.
    bool expanded{};
    bool live_started{}, live_restart{}, restore_gain{}, restore_muted{};
    u8 live_restarts{};
    float restore_volume{1};
    u16 collection_count{}, collection_track{};
    bool collection_more{};
    bool collection_continue{true};
    UiVideoControl collection_focus{};
    UiPoint pointer{-kMaximumUiCoordinate, -kMaximumUiCoordinate};
    static void* operator new(usize, void* storage) noexcept {
        return storage;
    }
    static void operator delete(void*, void*) noexcept {
    }
    // MSVC emits a scalar deleting destructor even for this placement-only owner.
    // Its valid cleanup is explicit destruction followed by VirtualFree.
    static void operator delete(void*, usize) noexcept {
        __fastfail(FAST_FAIL_INVALID_ARG);
    }
    explicit ApplicationVideo(InlineAudioSceneSelection selected_scene) noexcept : ui(selected_scene) {
        source.audio = true;
    }
    explicit ApplicationVideo(InlineVideoSceneSelection selected_scene) noexcept : ui(selected_scene) {
    }
    ~ApplicationVideo() noexcept {
        if (source.audio)
            ui.audio.~UiAudioScene();
        else
            ui.video.~UiVideoScene();
    }
};
static_assert(sizeof(ApplicationVideo) <= 4096);
namespace {
CdnImageType audio_source_type(MediaAttachmentFormat format) noexcept {
    if (format == MediaAttachmentFormat::mp3)
        return CdnImageType::mp3;
    if (format == MediaAttachmentFormat::wav)
        return CdnImageType::wav;
    if (format == MediaAttachmentFormat::m4a)
        return CdnImageType::m4a;
    return CdnImageType::ogg_opus;
}
AccountPlaybackKind audio_playback_kind(MediaAttachmentFormat format) noexcept {
    if (format == MediaAttachmentFormat::mp3)
        return AccountPlaybackKind::mp3;
    if (format == MediaAttachmentFormat::wav)
        return AccountPlaybackKind::wave_pcm;
    if (format == MediaAttachmentFormat::m4a)
        return AccountPlaybackKind::mp4;
    return AccountPlaybackKind::ogg_opus;
}
void close_video_session(AccountVideoIdentity* retired_identity) noexcept;
bool content_surface_covers_video() noexcept {
    const auto* browser = g_app.renderer.thread_browser_scene;
    // Active panel ownership also covers a temporarily stale publication.
    return (g_app.renderer.poll_voters_scene && g_app.renderer.poll_voters_scene->active) ||
           (browser && browser->active) ||
           (g_app.renderer.forum_form && g_app.renderer.forum_form->scene &&
            g_app.renderer.forum_form->scene->active);
}
void release_video_pointer_capture() noexcept {
    if (GetCapture() == g_app.window)
        ReleaseCapture();
}
// Detaches the audio and video scenes from the renderer; true when one was bound.
bool unbind_playback_scenes() noexcept {
    const bool had_scene = g_app.renderer.video_scene != nullptr || g_app.renderer.audio_scene != nullptr;
    renderer_bind_audio_scene(&g_app.renderer, nullptr);
    g_app.renderer.video_scene = nullptr;
    if (g_app.renderer.inline_embed_video.id) {
        reset_value(&g_app.renderer.inline_embed_video);
        g_app.renderer.retained_presentation.valid = false;
    }
    g_app.renderer.video_scene_search_result = false;
    g_app.renderer.video_scene_viewer = false;
    return had_scene;
}
UiRect clip_video_panel(const UiRect& clip, const UiRect& panel) noexcept {
    if (!ui_rect_has_area(ui_rect_intersection(clip, panel)))
        return clip;
    // The results pane occupies the right edge. Preserve the visible portion
    // of a playing preview and use this same clip for controls, text and hits.
    return panel.left <= clip.left
               ? UiRect{}
               : UiRect{clip.left, clip.top, minimum(clip.right, panel.left), clip.bottom};
}
UiRect available_video_clip(UiRect clip) noexcept {
    const auto& renderer = g_app.renderer;
    if (content_surface_covers_video())
        return {};
    // Query authority can turn stale during permission dispatch; the modal
    // still owns the underlying timeline until its active lifetime ends.
    if ((application_downloads_active() ||
         (g_app.renderer.message_forward_scene && g_app.renderer.message_forward_scene->active)))
        return {};
    if (renderer.quick_switcher_scene && renderer.quick_switcher_scene->active)
        return {};
    if (d3d11_renderer_search_visible(renderer) && renderer.search_scene->content->panel)
        clip = clip_video_panel(clip, renderer.search_scene->panel);
    if (d3d11_renderer_pinned_messages_visible(renderer)) {
        const auto& pins = *renderer.pinned_messages_scene;
        // Compact pins own the whole viewport as a modal overlay. A docked
        // panel leaves the visible left side of the selected preview usable.
        if (pins.overlay && ui_rect_has_area(ui_rect_intersection(clip, pins.viewport)))
            return {};
        clip = clip_video_panel(clip, pins.panel);
    }
    return clip;
}
bool video_session_available() noexcept {
    return g_app.account_ui && g_app.account.instance && !g_app.account.stop_failed &&
           !g_app.login.visible && g_app.account.api.video_select && g_app.account.api.video_control &&
           g_app.account.api.video_status && g_app.account.api.video_take &&
           g_app.account.api.video_acknowledge;
}
bool video_viewer_hosted() noexcept {
    return g_app.video && g_app.video->expanded && g_app.image_viewer.scene.active;
}
bool video_surface_available() noexcept {
    return !content_surface_covers_video() &&
           !(application_downloads_active() ||
             (g_app.renderer.message_forward_scene && g_app.renderer.message_forward_scene->active)) &&
           video_session_available() && !IsIconic(g_app.window) && !application_settings_active() &&
           !application_friends_active() && !g_app.popup_scene.active && !g_app.reaction_viewer.active &&
           (!g_app.image_viewer.scene.active || video_viewer_hosted()) && !g_app.text_form &&
           !(g_app.renderer.quick_switcher_scene && g_app.renderer.quick_switcher_scene->active) &&
           !(g_app.renderer.profile_scene && g_app.renderer.profile_scene->active);
}
UiPoint video_pointer_position() noexcept {
    POINT pointer{};
    if (!GetCursorPos(&pointer) || WindowFromPoint(pointer) != g_app.window ||
        !application_screen_to_content(&pointer))
        return {-kMaximumUiCoordinate, -kMaximumUiCoordinate};
    const float scale = 96.0f / static_cast<float>(g_app.renderer.dpi);
    return {pointer.x * scale, pointer.y * scale};
}
void track_video_pointer(ApplicationVideo& video) noexcept {
    const bool hovered =
        video.source.audio ? video.ui.audio.hover != UiAudioControl::none : video.ui.video.preview_hovered;
    if (hovered && !video.pointer_leave_armed) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, g_app.window, 0};
        video.pointer_leave_armed = TrackMouseEvent(&tracking) != FALSE;
    }
}
void note_video_pointer(ApplicationVideo& video, UINT message, UiPoint point) noexcept {
    // Windows repeats WM_MOUSEMOVE without motion; only real movement is activity.
    if (message == WM_MOUSEMOVE && video.pointer.x == point.x && video.pointer.y == point.y)
        return;
    video.pointer = point;
    if (!video.source.audio && ui_video_scene_activity(&video.ui.video, clock_monotonic_milliseconds()))
        application_request_frame();
}
bool video_input_focus_current(const ApplicationVideo& video) noexcept {
    return video.input_focus && GetFocus() == g_app.window && !g_app.context_focus &&
           !g_app.message_composer.focused;
}
void acquire_video_focus(ApplicationVideo& video) noexcept {
    video.input_focus = true;
    if (video.source.audio)
        video.ui.audio.keyboard_focus = false;
    else
        video.ui.video.keyboard_focus = false;
    application_composer_text_input_suspend();
    ui_button_keyboard_cancel(&g_app.button_keyboard);
    g_app.context_focus = nullptr;
    g_app.message_composer.focused = false;
    message_action_bar_close(&g_app.message_action_bar);
    SetFocus(g_app.window);
}
UiWidgetReference inline_attachment_owner(const MessageScene& scene, u64 message_id, u8 item,
                                          bool snapshot) noexcept {
    for (u32 i = 0; i < scene.count; ++i) {
        const auto& outer = scene.rows[i];
        if (outer.message_id != message_id)
            continue;
        const auto* row = snapshot ? message_scene_snapshot_row(scene, outer) : &outer;
        if (!row)
            return {};
        const auto widget =
            message_scene_rich_item_widget(*row, MessageSceneRichItemKind::attachment, item);
        if (ui_widget_manager_resolve(scene.widgets, widget) &&
            ui_widget_manager_input_enabled(scene.widgets, widget))
            return {&scene.widgets, widget};
    }
    return {};
}
bool resolve_search_video_source(const ApplicationSearchSourceTicket& ticket, bool embed, u8 item,
                                 bool snapshot, InlineMediaSource* out) noexcept {
    if (!video_session_available() || !d3d11_renderer_search_visible(g_app.renderer))
        return false;
    ApplicationSearchSourceView source{};
    if (!application_search_source_resolve(ticket, snapshot, &source) || !source.message || !source.rich ||
        !source.rich_store || !source.message->revision)
        return false;
    const auto& search = *g_app.renderer.search_scene;
    UiSearchTarget target{};
    target.action = UiSearchAction::rich_item;
    target.channel_id = source.message->channel_id;
    target.message_id = source.message->id;
    target.source_revision = search.source_revision;
    target.source_index = ticket.index;
    target.rich_kind = embed ? MessageSceneRichItemKind::embed : MessageSceneRichItemKind::attachment;
    target.rich_index = item;
    target.snapshot = snapshot;
    MediaAssetKey key{};
    bool provider = false;
    u32 start_seconds = 0;
    if (embed) {
        MessageRichEmbedMediaSelection selected{};
        if (item >= source.rich->embed_count ||
            source.rich->embeds[item].type == GatewayMessageEmbedType::gifv ||
            !message_rich_embed_playback_select(*source.rich_store, source.rich->embeds[item], &selected) ||
            (selected.format != MediaAttachmentFormat::mp4 && !media_embed_audio_format_supported(selected.format)))
            return false;
        provider = selected.provider;
        start_seconds = selected.start_seconds;
        MessageRichEmbedMediaSelection poster{};
        target.resource = provider && message_rich_embed_media_select(*source.rich_store,
                              source.rich->embeds[item], &poster) ? poster.resource : selected.resource;
        key = media_asset_search_embed(source.message->id, source.message->revision, item,
                                       selected.resource, selected.format, selected.animated,
                                       MediaResolutionTier::px512, snapshot);
    } else {
        if (item >= source.rich->attachment_count)
            return false;
        const auto& attachment = source.rich->attachments[item];
        const auto audio_format = message_rich_attachment_audio_format(*source.rich_store, attachment);
        const bool audio = media_attachment_format_audio_supported(audio_format);
        if (!audio &&
            message_rich_attachment_format(*source.rich_store, attachment) != MediaAttachmentFormat::mp4)
            return false;
        key = media_asset_search_attachment(
            attachment.id, audio ? audio_format : MediaAttachmentFormat::mp4,
            audio ? MediaResolutionTier::original : MediaResolutionTier::px512, source.message->revision,
            snapshot);
    }
    UiRect preview{}, clip{};
    if (!media_asset_key_valid(key) ||
        !ui_search_scene_rich_preview_geometry(search, target, &preview, &clip) || !ui_rect_has_area(clip))
        return false;
    *out = {key,
            preview,
            clip,
            source.message->id,
            source.message->channel_id,
            source.message->revision,
            item,
            embed,
            snapshot,
            ticket};
    out->provider = provider;
    out->audio_preview = embed && media_embed_audio_format_supported(media_asset_embed_format(key));
    out->start_seconds = start_seconds;
    out->audio = !embed && media_attachment_format_audio_supported(media_asset_attachment_format(key));
    if (out->audio) {
        const auto* rich_scene = ui_search_scene_message_scene(search);
        out->owner = rich_scene ? inline_attachment_owner(*rich_scene, source.message->id, item, snapshot)
                                : UiWidgetReference{};
        if (!out->owner.manager)
            return false;
    }
    return true;
}
bool resolve_visible_video_source(u64 message_id, u32 revision, bool embed, u8 item, InlineMediaSource* out,
                                  bool snapshot = false,
                                  const ApplicationSearchSourceTicket* search_source = nullptr) noexcept {
    if (search_source && search_source->publication_revision)
        return search_source->message_id == message_id && search_source->publication_revision == revision &&
               resolve_search_video_source(*search_source, embed, item, snapshot, out);
    if (!video_session_available())
        return false;
    const auto& scene = g_app.message_scene;
    const auto& store = g_app.account_ui->message_rich_store;
    for (u32 row_index = 0; row_index < scene.count; ++row_index) {
        const auto* selected_row =
            snapshot ? message_scene_snapshot_row(scene, scene.rows[row_index]) : &scene.rows[row_index];
        if (!selected_row)
            continue;
        const auto& row = *selected_row;
        const auto* message = message_store_resolve(g_app.message_view.store, row.message);
        const auto* rich = message ? (snapshot ? message_rich_store_snapshot(store, message->id)
                                               : message_rich_store_find(store, message->id))
                                   : nullptr;
        if (!rich || !application_channel_readable(g_app, message->channel_id) ||
            message->id != message_id || message->revision != revision ||
            rich->message_revision != revision ||
            message->channel_id != g_app.account_ui->active_channel_id)
            continue;
        const auto* node = message_scene_rich_item_layout_node(
            scene, row, embed ? MessageSceneRichItemKind::embed : MessageSceneRichItemKind::attachment,
            item);
        if (!node)
            return false;
        float width = 0, height = 0;
        u32 source_width = 0, source_height = 0;
        MediaAssetKey key{};
        bool provider = false;
        u32 start_seconds = 0;
        UiRect rectangle{};
        if (!embed) {
            if (item >= rich->attachment_count)
                return false;
            const auto& attachment = rich->attachments[item];
            const auto audio_format = message_rich_attachment_audio_format(store, attachment);
            if (media_attachment_format_audio_supported(audio_format)) {
                const auto owning = inline_attachment_owner(scene, message_id, item, snapshot);
                if (!owning.manager)
                    return false;
                UiAudioCardGeometry card{};
                if (!ui_audio_card_geometry(node->content_rectangle, kDefaultUiAudioControlsStyle, &card,
                                            message_rich_record_voice(*rich)))
                    return false;
                *out = {media_asset_attachment(attachment.id, audio_format, MediaResolutionTier::original),
                        card.bounds,
                        available_video_clip(ui_rect_intersection(
                            card.bounds, ui_rect_intersection(node->clip_rectangle, scene.viewport))),
                        message_id,
                        message->channel_id,
                        revision,
                        item,
                        false,
                        snapshot,
                        {},
                        owning,
                        true};
                return true;
            }
            if (message_rich_attachment_format(store, attachment) != MediaAttachmentFormat::mp4 ||
                !message_rich_attachment_preview_extent(
                    store, attachment, ui_rect_width(node->content_rectangle), &width, &height))
                return false;
            source_width = attachment.width;
            source_height = attachment.height;
            rectangle = {node->rectangle.left, node->rectangle.top, node->rectangle.left + width,
                         node->rectangle.top + height};
            const auto proxy =
                media_presentation_request(source_width, source_height, width, height, g_app.renderer.dpi,
                                           media_preview_extent_cap(MediaAttachmentFormat::mp4));
            key = media_asset_attachment(attachment.id, MediaAttachmentFormat::mp4, proxy.resolution);
        } else {
            if (item >= rich->embed_count || rich->embeds[item].type == GatewayMessageEmbedType::gifv)
                return false;
            MessageRichEmbedMediaSelection media{};
            if (!message_rich_embed_playback_select(store, rich->embeds[item], &media) ||
                (media.format != MediaAttachmentFormat::mp4 && !media_embed_audio_format_supported(media.format)))
                return false;
            provider = media.provider;
            start_seconds = media.start_seconds;
            width = row.rich_item_layout.embed_media_widths[item];
            height = row.rich_item_layout.embed_media_heights[item];
            if (width <= 0 || height <= 0)
                return false;
            source_width = media.media->width;
            source_height = media.media->height;
            const float left = node->content_rectangle.left +
                               message_rich_embed_media_left(message_rich_embed_media_only(rich->embeds[item]));
            const float top = node->content_rectangle.top + row.rich_item_layout.embed_media_tops[item];
            rectangle = {left, top, left + width, top + height};
            const auto proxy =
                media_presentation_request(source_width, source_height, width, height, g_app.renderer.dpi,
                                           media_preview_extent_cap(MediaAttachmentFormat::mp4));
            key = media_asset_embed(message->id, revision, item, media.resource, media.format,
                                    media.animated, proxy.resolution, snapshot);
        }
        *out = {key,
                rectangle,
                available_video_clip(ui_rect_intersection(rectangle, scene.viewport)),
                message_id,
                message->channel_id,
                revision,
                item,
                embed,
                snapshot};
        out->provider = provider;
        out->audio_preview = embed && media_embed_audio_format_supported(media_asset_embed_format(key));
        out->start_seconds = start_seconds;
        return true;
    }
    return false;
}
// *refresh requests a forced signed-URL refresh for an attachment and reports
// whether the media lane asked for one (403/410 or an expired URL).
bool request_video_source(const InlineMediaSource& source, bool* refresh = nullptr, ApplicationVideo* playback = nullptr) noexcept {
    const MessageRecord* message = nullptr;
    Utf8View content{};
    const MessageRichRecord* rich = nullptr;
    const MessageRichStore* store = &g_app.account_ui->message_rich_store;
    ApplicationSearchSourceView searched{};
    if (media_asset_search(source.key)) {
        if (!application_search_source_resolve(source.search_source, source.snapshot, &searched))
            return false;
        message = searched.message;
        rich = searched.rich;
        store = searched.rich_store;
        content = message_store_content(*searched.store, searched.handle);
    } else {
        const auto found = message_store_find(g_app.message_view.store, source.message_id);
        message = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
        if (message) content = message_store_content(g_app.message_view.store, found.handle);
        rich = message ? (source.snapshot ? message_rich_store_snapshot(*store, source.message_id)
                                          : message_rich_store_find(*store, source.message_id))
                       : nullptr;
    }
    if (!message || !application_channel_readable(g_app, message->channel_id) ||
        message->revision != source.revision || !rich || rich->message_revision != source.revision)
        return false;
    Utf8View url{};
    u32 width = 0, height = 0;
    if (!source.embed) {
        if (source.index >= rich->attachment_count)
            return false;
        const auto& attachment = rich->attachments[source.index];
        if (attachment.id != source.key.id ||
            (source.audio && message_rich_attachment_audio_format(*store, attachment) !=
                                 media_asset_attachment_format(source.key)))
            return false;
        url = message_rich_store_string(*store, attachment.url);
        width = attachment.width;
        height = attachment.height;
    } else {
        if (source.index >= rich->embed_count)
            return false;
        MessageRichEmbedMediaSelection media{};
        if (!message_rich_embed_playback_select(*store, rich->embeds[source.index], &media))
            return false;
        // Embed URLs can lose the playlist query. Recover it from this exact
        // message (or forwarded snapshot), never from another visible row.
        if (source.snapshot) content = message_rich_store_string(*store, rich->snapshot.content);
        url = youtube_collection_source(media.source, content);
        u8 target[kCdnMaximumTargetBytes];
        if (!parse_media_url(url, {target, sizeof(target)}).ok() && media.fallback_source.bytes &&
            parse_media_url(media.fallback_source, {target, sizeof(target)}).ok())
            url = media.fallback_source;
        width = media.media->width;
        height = media.media->height;
    }
    if (!url.data || !url.bytes || url.bytes > kCdnMaximumTargetBytes)
        return false;
    const u64 now = clock_monotonic_milliseconds(), unix_seconds = clock_unix_milliseconds() / 1000;
    const auto original_url = url;
    url = media_url_resolve(&g_app.media_urls, url, now, unix_seconds, refresh && *refresh && !source.embed);
    if (!url.bytes) {
        const bool pending =
            media_url_resolution_state(g_app.media_urls, original_url) != MediaUrlState::failed;
        if (!pending)
            log_write(LogLevel::warning, "media", "video_source_refresh_failed");
        return pending;
    }
    const auto proxy = media_presentation_request(width, height, ui_rect_width(source.preview),
                                                  ui_rect_height(source.preview), g_app.renderer.dpi, 512);
    // Provider artwork describes the poster, not playback quality. Resolve up
    // to 1080p within the existing media budgets so the same session can expand.
    // Frame uploads remain sized to the actual display rectangle.
    const AccountMediaAttachmentRequest request{
        url,
        source.key,
        source.message_id,
        source.revision,
        static_cast<u16>(source.audio || source.audio_preview ? 0 : source.provider ? 1920 : proxy.width),
        static_cast<u16>(source.audio || source.audio_preview ? 0 : source.provider ? 1080 : proxy.height),
        source.audio || source.audio_preview ? audio_source_type(media_asset_audio_format(source.key))
                     : CdnImageType::mp4,
        {static_cast<u8>(source.embed && (!source.audio_preview || soundcloud_track_url(original_url) || embed_collection_url(original_url))),
         {0, 0}, playback ? playback->collection_track : u16{0}},
        source.audio || source.audio_preview ? nullptr : g_app.renderer.graphics.device};
    const auto result =
        g_app.account.api.media_attachment_request(g_app.account.instance, &request, now, unix_seconds);
    if (refresh)
        *refresh = result.state == AccountMediaRequestState::refresh_required;
    if (playback && result.collection_count && result.collection_track && result.collection_track <= result.collection_count) {
        playback->collection_count = result.collection_count;
        playback->collection_track = result.collection_track;
        playback->collection_more = result.collection_more;
    }
    return application_video_source_pending(result);
}
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
bool send_video_command(ApplicationVideo& video, AccountVideoCommand kind, i64 seek = 0, float volume = 1,
                        bool muted = false) noexcept {
    if (!video.selected || !video_session_available() || video.command_serial == ~u64{0})
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
enum class InlineMediaPolicyUse : u8 {
    activate,
    cursor,
    retain
};
bool inline_media_activation_consumed(const InlineMediaSource& source,
                                      InlineMediaPolicyUse use = InlineMediaPolicyUse::activate) noexcept;
bool switch_collection_track(ApplicationVideo&, u16 track, UiVideoControl focus) noexcept;
void dispatch_video_event(ApplicationVideo& video, const UiVideoSceneResult& event) noexcept {
    if (event.request_capture)
        SetCapture(g_app.window);
    if (event.release_capture)
        release_video_pointer_capture();
    if (event.handled)
        application_request_frame();
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
    if (g_app.video_lifetime_serial == ~u64{0})
        return false;
    void* allocation =
        VirtualAlloc(nullptr, sizeof(ApplicationVideo), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!allocation)
        return false;
    auto* video = source.audio ? new (allocation) ApplicationVideo(InlineAudioSceneSelection{})
                               : new (allocation) ApplicationVideo(InlineVideoSceneSelection{});
    g_app.video = video;
    video->source = source;
    if (!capture_audio_metadata(*video)) {
        video->~ApplicationVideo();
        VirtualFree(video, 0, MEM_RELEASE);
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
    if (event.request_capture)
        SetCapture(g_app.window);
    if (event.release_capture)
        release_video_pointer_capture();
    if (event.handled)
        application_request_frame();
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
// Routes one pointer message into the video scene; true when a control handled it.
bool video_scene_pointer_event(ApplicationVideo& video, UINT message, UiPoint point) noexcept {
    auto& scene = video.ui.video;
    const auto before = ui_video_scene_paint_revision(scene);
    note_video_pointer(video, message, point);
    const auto kind = message == WM_LBUTTONDOWN ? UiVideoPointerEvent::down
                      : message == WM_LBUTTONUP ? UiVideoPointerEvent::up
                                                : UiVideoPointerEvent::move;
    const auto result = ui_video_scene_pointer(&scene, kind, point);
    track_video_pointer(video);
    if (before != ui_video_scene_paint_revision(scene))
        application_request_frame();
    if (!result.handled)
        return false;
    if (message == WM_LBUTTONDOWN)
        acquire_video_focus(video);
    dispatch_video_event(video, result);
    return true;
}
} // namespace
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
            release_video_pointer_capture();
        if (before != ui_audio_scene_paint_revision(scene))
            application_request_frame();
        return;
    }
    auto& scene = g_app.video->ui.video;
    const auto before = ui_video_scene_paint_revision(scene);
    const auto result = ui_video_scene_pointer(&scene, UiVideoPointerEvent::cancel, {});
    if (result.release_capture)
        release_video_pointer_capture();
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
namespace {
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
    VirtualFree(video, 0, MEM_RELEASE);
    if (had_scene && g_app.renderer_ready &&
        !renderer_rebuild_content_text(&g_app.renderer, application_theme()))
        log_write(LogLevel::warning, "media", "video_controls_text_retirement_failed");
}
} // namespace
void application_video_close() noexcept {
    close_video_session(nullptr);
}
bool application_video_expanded() noexcept {
    return g_app.video && g_app.video->expanded;
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

namespace {
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
void refresh_video_text() noexcept {
    if (!renderer_rebuild_content_text(&g_app.renderer, application_theme())) {
        // Text/layout rejection does not invalidate the account or media owner.
        // Leave its revision pending and use the ordinary presentation retry.
        application_record_presentation_failure("video_text_projection_failed");
        application_request_frame();
    }
}
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
bool project_video_scene(ApplicationVideo* video, const InlineMediaSource& positioned, bool visible, bool* repaint) noexcept {
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
            track_video_pointer(*video);
            if (!renderer_bind_audio_scene(&g_app.renderer, &video->ui.audio))
                return false;
            if (capture != UiAudioControl::none && video->ui.audio.capture == UiAudioControl::none)
                release_video_pointer_capture();
            if (g_app.renderer.interaction_text_audio_revision != ui_audio_scene_text_revision(video->ui.audio))
                refresh_video_text();
        } else {
            renderer_bind_audio_scene(&g_app.renderer, nullptr);
            if (capture != UiAudioControl::none)
                release_video_pointer_capture();
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
        track_video_pointer(*video);
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
            release_video_pointer_capture();
        if (g_app.renderer.interaction_text_video_revision != ui_video_scene_text_paint_revision(video->ui.video))
            refresh_video_text();
    } else {
        video->presentation_wake_ms = 0;
        g_app.renderer.video_scene = nullptr;
        g_app.renderer.video_scene_search_result = false;
        g_app.renderer.video_scene_viewer = false;
        if (before_capture != UiVideoControl::none)
            release_video_pointer_capture();
    }
    if (repaint && old_paint_revision != ui_video_scene_paint_revision(video->ui.video))
        *repaint = true;
    return true;
}
} // namespace
bool application_video_synchronize() noexcept {
    auto* video = g_app.video;
    if (!video) return true;
    InlineMediaSource positioned{};
    const bool visible = position_video_scene(video, positioned);
    return project_video_scene(video, positioned, visible, nullptr);
}

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
        const auto existing = g_app.account.api.video_status(g_app.account.instance);
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
                video->status = g_app.account.api.video_status(g_app.account.instance);
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
        video->status = g_app.account.api.video_status(g_app.account.instance);
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
        log_write(LogLevel::warning, "media", "video_playback_status_failed");
        log_u64(LogLevel::warning, "media", "video_playback_error",
                static_cast<u64>(current_playback.error));
        log_u64(LogLevel::warning, "media", "video_native_error", current_playback.native_error);
        log_u64(LogLevel::warning, "media", "video_rejected_error",
                static_cast<u64>(current_playback.rejected_error));
        log_u64(LogLevel::warning, "media", "video_position_seconds",
                current_playback.position_100ns > 0
                    ? static_cast<u64>(current_playback.position_100ns / 10000000)
                    : 0);
        log_u64(LogLevel::warning, "media", "video_has_audio", current_playback.has_audio);
        log_u64(LogLevel::warning, "media", "video_selected", video->status.selected);
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
bool application_video_pointer(UINT message, LPARAM lparam) noexcept {
    if (message == WM_MOUSELEAVE) {
        if (g_app.video) {
            g_app.video->pointer_leave_armed = false;
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
    const UiPoint point{static_cast<float>(static_cast<i16>(LOWORD(lparam))) * scale,
                        static_cast<float>(static_cast<i16>(HIWORD(lparam))) * scale};
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
        const auto kind = message == WM_LBUTTONDOWN ? UiAudioPointerEvent::down
                          : message == WM_LBUTTONUP ? UiAudioPointerEvent::up
                                                    : UiAudioPointerEvent::move;
        const auto result = ui_audio_scene_pointer(&video->ui.audio, kind, point);
        track_video_pointer(*video);
        if (before != ui_audio_scene_paint_revision(video->ui.audio))
            application_request_frame();
        if (result.handled) {
            if (message == WM_LBUTTONDOWN)
                acquire_video_focus(*video);
            dispatch_audio_event(*video, result);
            return true;
        }
    }
    if (video && !video->source.audio && video->ui.video.active && video_scene_pointer_event(*video, message, point))
        return true;
    if (message != WM_LBUTTONDOWN)
        return false;
    // Search's typed rich target owns preview activation. A miss on its player
    // controls must not fall through into a covered live message preview.
    if (video && media_asset_search(video->source.key)) {
        application_video_blur();
        return false;
    }
    for (u32 row_index = 0; row_index < g_app.message_scene.count; ++row_index) {
        for (u32 content_scope = 0; content_scope < 2; ++content_scope) {
            const auto& row = g_app.message_scene.rows[row_index];
            const auto* resolved_message = message_store_resolve(g_app.message_view.store, row.message);
            const auto* rich =
                resolved_message
                    ? (content_scope ? message_rich_store_snapshot(g_app.account_ui->message_rich_store,
                                                                   resolved_message->id)
                                     : message_rich_store_find(g_app.account_ui->message_rich_store,
                                                               resolved_message->id))
                    : nullptr;
            if (!rich)
                continue;
            for (u32 kind = 0; kind < 2; ++kind) {
                const u32 count = kind ? rich->embed_count : rich->attachment_count;
                for (u32 item_index = 0; item_index < count; ++item_index) {
                    InlineMediaSource source{};
                    if (resolve_visible_video_source(resolved_message->id, resolved_message->revision,
                                                     kind != 0, static_cast<u8>(item_index), &source,
                                                     content_scope != 0) &&
                        ui_rect_contains(source.clip, point.x, point.y)) {
                        if (inline_media_activation_consumed(source))
                            return true;
                        if (source.audio) {
                            UiAudioControlsProjection controls{};
                            if (!ui_audio_attachment_project(
                                    g_app.account_ui->message_rich_store, rich->attachments[item_index],
                                    video ? video->session_epoch : 1, u64{resolved_message->revision} + 1,
                                    message_rich_record_voice(*rich), source.preview, application_theme(),
                                    {}, &controls))
                                return false;
                            UiAudioAction action{};
                            if (!ui_audio_controls_pointer_action(controls, point, &action))
                                return false;
                            if (action.control == UiAudioControl::download)
                                return application_downloads_begin(source.message_id, source.key.id,
                                                                   source.snapshot);
                            return action.control == UiAudioControl::play_pause &&
                                   activate_inline_source(source);
                        }
                        if (video && !video->source.audio &&
                            media_asset_key_equal(source.key, video->source.key) && video->selected &&
                            video->status.selected && !video->presentation_failed &&
                            video->status.playback.error == AccountVideoError::none) {
                            acquire_video_focus(*video);
                            ui_video_scene_focus(&video->ui.video, UiVideoControl::play_pause, false);
                            send_video_command(*video, video->status.playback.playing
                                                           ? AccountVideoCommand::pause
                                                           : AccountVideoCommand::play);
                            application_request_frame();
                            return true;
                        }
                        return start_video_session(source);
                    }
                }
            }
        }
    }
    if (video) {
        application_video_blur();
    }
    return false;
}
bool application_video_viewer_pointer(UINT message, UiPoint point) noexcept {
    auto* video = g_app.video;
    if (!video || !video->expanded || video->source.audio || !video_surface_available() ||
        !video->ui.video.active)
        return false;
    auto& scene = video->ui.video;
    if (video_scene_pointer_event(*video, message, point))
        return true;
    // A click on the picture itself toggles playback, as in the official client's viewer.
    if (message == WM_LBUTTONDOWN && ui_rect_contains(ui_rect_intersection(scene.preview, scene.clip), point.x,
                                                      point.y)) {
        acquire_video_focus(*video);
        dispatch_video_event(*video, ui_video_scene_player_key(&scene, UiVideoPlayerKey::toggle_play, 0,
                                                               clock_monotonic_milliseconds()));
        return true;
    }
    return message != WM_MOUSEMOVE && ui_rect_contains(ui_rect_intersection(scene.preview, scene.clip),
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
    for (u32 row_index = 0; row_index < g_app.message_scene.count; ++row_index) {
        for (u32 content_scope = 0; content_scope < 2; ++content_scope) {
            const auto& row = g_app.message_scene.rows[row_index];
            const auto* message = message_store_resolve(g_app.message_view.store, row.message);
            const auto* rich =
                message ? (content_scope ? message_rich_store_snapshot(g_app.account_ui->message_rich_store,
                                                                        message->id)
                                         : message_rich_store_find(g_app.account_ui->message_rich_store,
                                                                   message->id))
                        : nullptr;
            if (!rich)
                continue;
            for (u32 kind = 0; kind < 2; ++kind) {
                const u32 count = kind ? rich->embed_count : rich->attachment_count;
                for (u32 item_index = 0; item_index < count; ++item_index) {
                    InlineMediaSource source{};
                    if (!resolve_visible_video_source(message->id, message->revision, kind != 0,
                                                      static_cast<u8>(item_index), &source, content_scope != 0) ||
                        !ui_rect_contains(source.clip, point.x, point.y))
                        continue;
                    if (inline_media_activation_consumed(source, InlineMediaPolicyUse::cursor))
                        return false;
                    if (!source.audio) {
                        *cursor = UiCursor::hand;
                        return true;
                    }
                    // An unselected card activates only Play and Download.
                    UiAudioControlsProjection controls{};
                    const auto control =
                        ui_audio_attachment_project(g_app.account_ui->message_rich_store,
                                                    rich->attachments[item_index], video ? video->session_epoch : 1,
                                                    u64{message->revision} + 1, message_rich_record_voice(*rich),
                                                    source.preview, application_theme(), {}, &controls)
                            ? ui_audio_controls_hit(controls, point)
                            : UiAudioControl::none;
                    *cursor = ui_action_cursor(control == UiAudioControl::play_pause ||
                                               control == UiAudioControl::download);
                    return true;
                }
            }
        }
    }
    return false;
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
bool application_video_key(UINT key, bool shift, bool repeat) noexcept {
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
                         GetFocus() == g_app.window && !g_app.message_composer.focused && !g_app.context_focus;
    if (!focused && !hovered && !video->expanded)
        return false;
    if (key == VK_ESCAPE) {
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
        if (key == VK_TAB)
            action = shift ? UiAudioKey::previous : UiAudioKey::next;
        else if (key == VK_LEFT || key == VK_DOWN)
            action = UiAudioKey::decrease;
        else if (key == VK_RIGHT || key == VK_UP)
            action = UiAudioKey::increase;
        else if (key == VK_HOME)
            action = UiAudioKey::home;
        else if (key == VK_END)
            action = UiAudioKey::end;
        else if ((key == VK_SPACE || key == VK_RETURN) && !repeat)
            action = UiAudioKey::activate;
        else
            return false;
        dispatch_audio_event(*video, ui_audio_scene_key(&video->ui.audio, action));
        return true;
    }
    auto& scene = video->ui.video;
    const u64 now = clock_monotonic_milliseconds();
    const bool modified = (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000);
    const bool range_focus = focused && scene.keyboard_focus &&
        (scene.focus == UiVideoControl::volume || scene.focus == UiVideoControl::track);
    const auto player = [&](UiVideoPlayerKey shortcut, u32 percent = 0) noexcept {
        return ui_video_scene_player_key(&scene, shortcut, percent, now);
    };
    u32 digit = 10;
    if (key >= '0' && key <= '9')
        digit = key - '0';
    else if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
        digit = key - VK_NUMPAD0;
    const bool printable = key == VK_SPACE || digit < 10 || key == 'M' || key == 'F';
    if (modified && printable && key != VK_SPACE)
        return false;
    UiVideoSceneResult result{};
    if (key == VK_TAB) {
        if (!focused && !video->expanded)
            return false;
        result = ui_video_scene_key(&scene, shift ? UiVideoKey::previous : UiVideoKey::next);
    } else if (key == VK_RETURN) {
        if (!focused)
            return false;
        if (!repeat)
            result = ui_video_scene_key(&scene, UiVideoKey::activate);
    } else if (key == VK_SPACE) {
        if (!repeat)
            result = player(UiVideoPlayerKey::toggle_play);
    } else if (key == VK_LEFT || key == VK_RIGHT) {
        result = range_focus ? ui_video_scene_key(&scene, key == VK_LEFT ? UiVideoKey::decrease
                                                                          : UiVideoKey::increase)
                              : player(key == VK_LEFT ? UiVideoPlayerKey::seek_backward
                                                      : UiVideoPlayerKey::seek_forward);
    } else if (key == VK_UP || key == VK_DOWN) {
        result = player(key == VK_UP ? UiVideoPlayerKey::volume_up : UiVideoPlayerKey::volume_down);
    } else if (key == VK_HOME || key == VK_END) {
        result = range_focus ? ui_video_scene_key(&scene, key == VK_HOME ? UiVideoKey::home : UiVideoKey::end)
                              : player(UiVideoPlayerKey::seek_percent, key == VK_HOME ? 0 : 100);
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
    if (printable) {
        // TranslateMessage already queued this key's character; it is not text.
        MSG character{};
        PeekMessageW(&character, g_app.window, WM_CHAR, WM_CHAR, PM_REMOVE);
    }
    result.handled = true;
    dispatch_video_event(*video, result);
    return true;
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
        if (!control || !control->enabled || !control->visible)
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
        if (control.id != control_id || !control.enabled || !control.visible || !ui_rect_has_area(anchor))
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
} // namespace maicord::app_detail
