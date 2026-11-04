#include "video_internal.hpp"
#include "../settings/settings.hpp"
#include "../account/friends.hpp"
#include "../chat/composer/text_input.hpp"
#include "maicord/media/core/presentation_policy.hpp"
#include "maicord/protocol/media/embed_provider.hpp"

namespace maicord::app_detail {

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

} // namespace
namespace video_detail {

AccountPlaybackKind audio_playback_kind(MediaAttachmentFormat format) noexcept {
    if (format == MediaAttachmentFormat::mp3)
        return AccountPlaybackKind::mp3;
    if (format == MediaAttachmentFormat::wav)
        return AccountPlaybackKind::wave_pcm;
    if (format == MediaAttachmentFormat::m4a)
        return AccountPlaybackKind::mp4;
    return AccountPlaybackKind::ogg_opus;
}

bool content_surface_covers_video() noexcept {
    const auto* browser = g_app.renderer.thread_browser_scene;
    // Active panel ownership also covers a temporarily stale publication.
    return (g_app.renderer.poll_voters_scene && g_app.renderer.poll_voters_scene->active) ||
           (browser && browser->active) ||
           (g_app.renderer.forum_form && g_app.renderer.forum_form->scene &&
            g_app.renderer.forum_form->scene->active);
}

} // namespace video_detail
namespace {

bool modal_overlay_covers_video() noexcept {
    return application_downloads_active() ||
           (g_app.renderer.message_forward_scene && g_app.renderer.message_forward_scene->active);
}

} // namespace
namespace video_detail {

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

} // namespace video_detail
namespace {

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
    if (modal_overlay_covers_video())
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

} // namespace
namespace video_detail {

bool video_session_available() noexcept {
    return g_app.account_ui && g_app.account.instance && !g_app.account.stop_failed &&
           !g_app.login.visible && g_app.account.api.video_select && g_app.account.api.video_control &&
           g_app.account.api.video_status && g_app.account.api.video_take &&
           g_app.account.api.video_acknowledge;
}

} // namespace video_detail
namespace {

bool video_viewer_hosted() noexcept {
    return g_app.video && g_app.video->expanded && g_app.image_viewer.scene.active;
}

} // namespace
namespace video_detail {

bool video_surface_available() noexcept {
    return !content_surface_covers_video() &&
           !modal_overlay_covers_video() &&
           video_session_available() && !application_window_minimized() && !application_settings_active() &&
           !application_friends_active() && !g_app.popup_scene.active && !g_app.reaction_viewer.active &&
           (!g_app.image_viewer.scene.active || video_viewer_hosted()) && !g_app.text_form &&
           !(g_app.renderer.quick_switcher_scene && g_app.renderer.quick_switcher_scene->active) &&
           !(g_app.renderer.profile_scene && g_app.renderer.profile_scene->active);
}
UiPoint video_pointer_position() noexcept {
    POINT pointer{};
    const WindowMetrics client = window_metrics(g_app.window);
    if (!application_cursor_client(&pointer) || pointer.x < 0 || pointer.y < 0 ||
        pointer.x >= static_cast<LONG>(client.width_px) || pointer.y >= static_cast<LONG>(client.height_px))
        return {-kMaximumUiCoordinate, -kMaximumUiCoordinate};
    application_client_to_content(&pointer);
    const float scale = 96.0f / static_cast<float>(g_app.renderer.dpi);
    return {pointer.x * scale, pointer.y * scale};
}
void note_video_pointer(ApplicationVideo& video, VideoPointer kind, UiPoint point) noexcept {
    // Windows repeats WM_MOUSEMOVE without motion; only real movement is activity.
    if (kind == VideoPointer::move && video.pointer.x == point.x && video.pointer.y == point.y)
        return;
    video.pointer = point;
    if (!video.source.audio && ui_video_scene_activity(&video.ui.video, clock_monotonic_milliseconds()))
        application_request_frame();
}
bool video_input_focus_current(const ApplicationVideo& video) noexcept {
    return video.input_focus && application_window_keyboard_focus() && !g_app.context_focus &&
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
    window_focus(g_app.window);
}

} // namespace video_detail
namespace {

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

} // namespace
namespace video_detail {

bool resolve_search_video_source(const ApplicationSearchSourceTicket& ticket, bool embed, u8 item,
                                 bool snapshot, InlineMediaSource* out) noexcept {
    if (!video_session_available() || !d3d11_renderer_search_visible(g_app.renderer))
        return false;
    ApplicationSearchSourceView source{};
    if (!application_search_source_resolve(ticket, snapshot, &source) || !source.message || !source.rich ||
        !source.rich_store || !source.message->revision)
        return false;
    const auto& search = *g_app.renderer.search_scene;
    auto target = application_search_rich_target(
        search, source.message->channel_id, source.message->id, ticket.index,
        embed ? MessageSceneRichItemKind::embed : MessageSceneRichItemKind::attachment, item, snapshot);
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
                                  bool snapshot,
                                  const ApplicationSearchSourceTicket* search_source) noexcept {
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
bool request_video_source(const InlineMediaSource& source, bool* refresh, ApplicationVideo* playback) noexcept {
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

} // namespace video_detail
} // namespace maicord::app_detail
