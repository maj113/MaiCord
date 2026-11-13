#include "downloads.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "../shell/toast.hpp"
#include "../shell/overlay_input.hpp"
#include "image_viewer.hpp"
#include "../shell/cursor.hpp"
#include "../shell/state.hpp"
#include "../chat/channel_authority.hpp"
#include "../chat/composer/text_input.hpp"
#include "../shell/content_input.hpp"
#include "policy.hpp"
#include "../shell/input.hpp"
#include "video_internal.hpp"
#include "../shell/tabs.hpp"
#include "../shell/window.hpp"
#include "maicord/core/time.hpp"
#include "maicord/render/media/atlas.hpp"
#include "maicord/media/core/presentation_policy.hpp"
#include "maicord/ui/widgets/button.hpp"
#include "maicord/platform/window/clipboard.hpp"
#include "maicord/render/text/form_surface.hpp"

#include "maicord/platform/window/key.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/protocol/media/embed_provider.hpp"
namespace maicord::app_detail {
namespace {
// The official client's modal spring (tension 1000, friction 48); exit settles faster.
constexpr UiMotionStyle kRevealOpen{360, UiMotionCurve::spring}, kRevealClose{180, UiMotionCurve::ease_out};
UiRect viewport() noexcept {
    const float scale = 96.0f / static_cast<float>(g_app.renderer.dpi);
    return {0, 0, g_app.renderer.width * scale, g_app.renderer.height * scale};
}
UiMotionScope reveal_scope() noexcept {
    return {0x4d45444941564945ull, g_app.image_viewer.reveal_generation};
}
ApplicationMediaViewerItem* current_item() noexcept {
    auto& viewer = g_app.image_viewer;
    return viewer.item_index < viewer.item_count ? &viewer.items[viewer.item_index] : nullptr;
}
bool current_video() noexcept {
    const auto* item = current_item();
    return item && item->video;
}
bool over_image(const UiImageViewer& scene, UiPoint point) noexcept {
    return !current_video() && ui_rect_contains(scene.image, point.x, point.y) &&
           ui_rect_contains(scene.body, point.x, point.y);
}
MessageScene* viewer_media_scene(bool searched) noexcept {
    if (!searched)
        return &g_app.message_scene;
    auto* scene = application_search_scene();
    return scene && scene == g_app.renderer.search_scene ? ui_search_scene_message_scene(scene) : nullptr;
}
MessageMediaObscure viewer_obscure(const ApplicationSearchSourceView& source, bool snapshot,
                                   MessageMediaSource kind, u32 index, bool spoiler,
                                   bool explicit_media) noexcept {
    auto* scene = viewer_media_scene(g_app.image_viewer.search_result);
    if (!scene || !source.message || !source.rich_store)
        return MessageMediaObscure::blocked;
    return message_scene_media_obscure(
        *scene,
        message_media_owner(*source.message,
                            message_rich_store_find(*source.rich_store, source.message->id)),
        source.message->id, message_media_reveal_key(source.message->id, kind, index, snapshot), spoiler,
        explicit_media);
}
bool viewer_item_visible(const ApplicationSearchSourceView& source,
                         const ApplicationMediaViewerItem& item) noexcept {
    const bool attachment = item.kind == MessageSceneRichItemKind::attachment;
    if (!source.rich || !source.rich_store ||
        (attachment ? item.index >= source.rich->attachment_count : item.index >= source.rich->embed_count))
        return false;
    return viewer_obscure(
               source, g_app.image_viewer.attachment_snapshot,
               attachment ? MessageMediaSource::attachment : MessageMediaSource::embed, item.index,
               attachment && message_rich_attachment_spoiler(*source.rich_store,
                                                             source.rich->attachments[item.index]),
               attachment ? message_rich_attachment_explicit(source.rich->attachments[item.index])
                          : message_rich_embed_explicit(source.rich->embeds[item.index])) ==
           MessageMediaObscure::none;
}
MediaAssetKey attachment_key(const ApplicationImageViewer* viewer, u64 id, MediaAttachmentFormat format,
                             MediaResolutionTier tier) noexcept {
    return viewer && viewer->search_result
               ? media_asset_search_attachment(id, format, tier, viewer->search_source.publication_revision,
                                               viewer->attachment_snapshot)
               : media_asset_attachment(id, format, tier);
}
// An image or animation attachment as the viewer previews it and a page shows it: its size (the file's, else its
// card's) and two decode keys, the largest tier (the viewer requests it once; zoom reuses that texture and never
// re-encodes or copies the pixels) and the tier the card shows. `viewer` is the open viewer (a search result's assets
// are its own), null for a message of the timeline.
void attachment_picture(const ApplicationImageViewer* viewer, const MessageRichStore& store,
                        const MessageRichAttachment& attachment, MediaAttachmentFormat format, UiSize* pixels,
                        MediaAssetKey* key, MediaAssetKey* fallback) noexcept {
    float width = 0, height = 0;
    if (!message_rich_attachment_preview_extent(store, attachment, 550, &width, &height))
        width = 400, height = 225;
    *pixels = attachment.width && attachment.height
                  ? UiSize{static_cast<float>(attachment.width), static_cast<float>(attachment.height)}
                  : UiSize{width, height};
    const u16 cap = media_preview_extent_cap(format);
    *fallback = attachment_key(viewer, attachment.id, format,
                               media_resolution_tier_for_display(width, height, g_app.renderer.dpi, cap));
    *key = attachment_key(viewer, attachment.id, format,
                          cap > 512 ? MediaResolutionTier::px2048 : MediaResolutionTier::px512);
}
// The same for an embed's picture: its media's size (else 400 by 225) and the two decode keys.
void embed_picture(bool searched, u64 message_id, u32 revision, u8 index, bool snapshot,
                   const MessageRichEmbedMediaSelection& selected, UiSize* pixels, MediaAssetKey* key,
                   MediaAssetKey* fallback) noexcept {
    const u32 width = selected.media->width, height = selected.media->height;
    *pixels = width && height ? UiSize{static_cast<float>(width), static_cast<float>(height)} : UiSize{400, 225};
    const float scale = minimum(1.0f, minimum(400.0f / pixels->width, 300.0f / pixels->height));
    const u16 cap = media_preview_extent_cap(selected.format, selected.animated);
    const auto make = [&](MediaResolutionTier resolution) noexcept {
        return searched ? media_asset_search_embed(message_id, revision, index, selected.resource, selected.format,
                                                   selected.animated, resolution, snapshot)
                        : media_asset_embed(message_id, revision, index, selected.resource, selected.format,
                                            selected.animated, resolution, snapshot);
    };
    *fallback = make(media_resolution_tier_for_display(pixels->width * scale, pixels->height * scale,
                                                       g_app.renderer.dpi, cap));
    *key = make(cap > 512 ? MediaResolutionTier::px2048 : MediaResolutionTier::px512);
}
// Every previewable item of one message, in the official client's order: attachments,
// then embed media. Audio and unknown files are skipped; a looping GIFV embed is
// shown as the animation it plays inline, not as a video.
u32 collect_items(const MessageRichStore& store, const MessageRichRecord& rich, bool embed_snapshot,
                  ApplicationMediaViewerItem* items) noexcept {
    const auto& viewer = g_app.image_viewer;
    u32 count = 0;
    ApplicationSearchSourceView source{};
    if (!application_image_viewer_source(&source))
        return 0;
    for (u32 i = 0; i < rich.attachment_count && count < kApplicationMediaViewerItems; ++i) {
        const auto& attachment = rich.attachments[i];
        if (viewer_obscure(source, embed_snapshot, MessageMediaSource::attachment, i,
                           message_rich_attachment_spoiler(store, attachment),
                           message_rich_attachment_explicit(attachment)) != MessageMediaObscure::none)
            continue;
        const auto format = message_rich_attachment_format(store, attachment);
        if (format == MediaAttachmentFormat::unknown || format == MediaAttachmentFormat::lottie ||
            media_attachment_format_audio_supported(format))
            continue;
        auto& item = items[count++];
        reset_value(&item);
        item.kind = MessageSceneRichItemKind::attachment;
        item.index = static_cast<u8>(i);
        item.attachment_id = attachment.id;
        item.video = format == MediaAttachmentFormat::mp4;
        MediaAssetKey key{}, fallback{};
        attachment_picture(&viewer, store, attachment, format, &item.pixels, &key, &fallback);
        if (!item.video) {
            item.key = key;
            item.fallback = fallback;
        }
    }
    for (u32 i = 0; i < rich.embed_count && count < kApplicationMediaViewerItems; ++i) {
        if (viewer_obscure(source, embed_snapshot, MessageMediaSource::embed, i, false,
                           message_rich_embed_explicit(rich.embeds[i])) != MessageMediaObscure::none)
            continue;
        MessageRichEmbedMediaSelection selected{};
        // Gallery images (all of them, as the official client's viewer) in gallery order;
        // a merged embed outside a shown gallery draws nothing.
        const u32 host = message_rich_embed_merge_host(store, rich, i);
        MessageRichEmbedGallery gallery{};
        bool member = false;
        if (message_rich_embed_gallery(store, rich, host, &gallery))
            for (u32 m = 0; m < gallery.count; ++m)
                member = member || gallery.members[m] == i;
        if (host != i && !member)
            continue;
        // Provider posters are images, but expanding them hosts the same
        // native video session. Gallery tiles and GIFV keep their image path.
        const bool playable = !member && rich.embeds[i].type != GatewayMessageEmbedType::gifv &&
                              message_rich_embed_playback_select(store, rich.embeds[i], &selected) &&
                              selected.format == MediaAttachmentFormat::mp4;
        if ((!playable && !(member ? message_rich_embed_gallery_select(store, rich.embeds[i], &selected)
                                  : message_rich_embed_media_select(store, rich.embeds[i], &selected))) ||
            selected.format == MediaAttachmentFormat::unknown)
            continue;
        auto& item = items[count++];
        reset_value(&item);
        item.kind = MessageSceneRichItemKind::embed;
        item.index = static_cast<u8>(i);
        item.resource = selected.resource;
        item.video = selected.format == MediaAttachmentFormat::mp4 &&
                     rich.embeds[i].type != GatewayMessageEmbedType::gifv;
        MediaAssetKey key{}, fallback{};
        embed_picture(viewer.search_result, viewer.message_id,
                      viewer.search_result ? viewer.search_source.publication_revision : viewer.message_revision,
                      static_cast<u8>(i), embed_snapshot, selected, &item.pixels, &key, &fallback);
        if (!item.video) {
            item.key = key;
            item.fallback = fallback;
        }
    }
    return count;
}
// Alt text when the author wrote one, otherwise the file name.
Utf8View item_caption(const ApplicationSearchSourceView& source, const ApplicationMediaViewerItem& item) noexcept {
    const auto& store = *source.rich_store;
    if (item.kind == MessageSceneRichItemKind::attachment && item.index < source.rich->attachment_count) {
        const auto& attachment = source.rich->attachments[item.index];
        const Utf8View description = message_rich_store_string(store, attachment.description);
        return description.bytes ? description : message_rich_store_string(store, attachment.filename);
    }
    MessageRichEmbedMediaSelection selected{};
    if (item.kind == MessageSceneRichItemKind::embed && item.index < source.rich->embed_count &&
        message_rich_embed_resource_select(store, source.rich->embeds[item.index], item.resource, &selected))
        return message_rich_store_string(store, selected.media->description);
    return {};
}
void select_item(u32 index) noexcept {
    auto& viewer = g_app.image_viewer;
    if (index >= viewer.item_count)
        return;
    ApplicationSearchSourceView source{};
    if (!application_image_viewer_source(&source) || !viewer_item_visible(source, viewer.items[index]))
        return;
    const bool was_video = current_video();
    viewer.item_index = static_cast<u8>(index);
    application_image_zoom_end();
    auto& item = viewer.items[index];
    viewer.attachment_id = item.attachment_id;
    reset_value(&viewer.failed_key);
    if (!ui_image_viewer_set_item(&viewer.scene, item.pixels, index, viewer.item_count, item.video))
        return;
    ui_image_viewer_set_caption(&viewer.scene, item_caption(source, item));
    if (item.video) {
        MediaAssetKey key{};
        if (application_video_open_viewer_item(viewer.message_id, viewer.message_revision,
                                               item.kind == MessageSceneRichItemKind::embed, item.index,
                                               viewer.attachment_snapshot,
                                               viewer.search_result ? &viewer.search_source : nullptr,
                                               viewer.scene.image, &key))
            item.key = item.fallback = key;
    } else if (was_video) {
        application_video_viewer_detach();
    }
    viewer.key = item.key;
    viewer.fallback = item.fallback;
    application_request_frame();
}
void navigate(i32 direction) noexcept {
    const auto& viewer = g_app.image_viewer;
    if (viewer.item_count < 2 || viewer.closing)
        return;
    select_item(ui_image_viewer_step(viewer.item_index, viewer.item_count, direction));
}
// The current item's original CDN URL, borrowed from the rich store.
Utf8View current_url() noexcept {
    ApplicationSearchSourceView source{};
    const auto* item = current_item();
    if (!item || !application_image_viewer_source(&source) || !viewer_item_visible(source, *item))
        return {};
    Utf8View url{};
    if (item->kind == MessageSceneRichItemKind::attachment) {
        if (item->index < source.rich->attachment_count &&
            source.rich->attachments[item->index].id == item->attachment_id)
            url = message_rich_store_string(*source.rich_store, source.rich->attachments[item->index].url);
    } else if (item->index < source.rich->embed_count) {
        MessageRichEmbedMediaSelection selected{};
        if (message_rich_embed_resource_select(*source.rich_store, source.rich->embeds[item->index], item->resource,
                                               &selected))
            url = message_rich_store_string(*source.rich_store, selected.media->url);
    }
    return url;
}
bool copy_image() noexcept {
    const auto& viewer = g_app.image_viewer;
    if (current_video() || !application_image_copy(viewer.key, viewer.fallback, current_url()))
        return false;
    auto& scene = g_app.image_viewer.scene;
    scene.copied = true;
    ++scene.revision;
    g_app.image_viewer.copied_until_ms = clock_monotonic_milliseconds() + 1500;
    application_request_frame();
    return true;
}
// Full screen the viewer turned on; it never takes over the call view's.
void set_fullscreen(bool on) noexcept {
    auto& viewer = g_app.image_viewer;
    if (on && !viewer.fullscreen && !application_window_fullscreen())
        viewer.fullscreen = application_window_set_fullscreen(true);
    else if (!on && viewer.fullscreen) {
        application_window_set_fullscreen(false);
        viewer.fullscreen = false;
    }
    ui_image_viewer_set_fullscreen(&viewer.scene, viewer.fullscreen);
    application_request_frame();
}
void activate(u32 index) noexcept {
    ApplicationSearchSourceView guarded_source{};
    const auto* guarded_item = current_item();
    if (!guarded_item || !application_image_viewer_source(&guarded_source) ||
        !viewer_item_visible(guarded_source, *guarded_item)) {
        application_image_viewer_close();
        return;
    }
    auto& scene = g_app.image_viewer.scene;
    switch (index) {
    case ui_image_viewer_save: {
        const auto& viewer = g_app.image_viewer;
        if (viewer.search_result) {
            if (viewer.attachment_id)
                application_downloads_begin_search(viewer.search_source, viewer.attachment_id, viewer.attachment_snapshot);
            return;
        }
        const auto source = message_store_find(g_app.message_view.store, viewer.message_id);
        const auto* record = source.ok() ? message_store_resolve(g_app.message_view.store, source.handle) : nullptr;
        const bool current = viewer.account_scope == g_app.plugin_account_scope_serial &&
                             viewer.profile_id == g_app.account.status.profile_id && g_app.account_ui &&
                             viewer.self_user_id == g_app.account_ui->chat_state.self_user_id && record &&
                             record->revision == viewer.message_revision &&
                             source.handle.slot == viewer.source_slot &&
                             source.handle.generation == viewer.source_generation;
        const auto* shown = current_item();
        if (current && shown && shown->kind == MessageSceneRichItemKind::embed)
            application_downloads_begin_embed(viewer.message_id, shown->index, viewer.attachment_snapshot);
        else if (current && viewer.attachment_id)
            application_downloads_begin(viewer.message_id, viewer.attachment_id, viewer.attachment_snapshot);
        return;
    }
    case ui_image_viewer_close:
        application_image_viewer_dismiss();
        return;
    case ui_image_viewer_open_original:
        if (const Utf8View url = current_url(); url.bytes)
            application_open_content_url(url);
        return;
    case ui_image_viewer_new_tab: {
        // The page copies what the viewer shows, then shows itself, which closes the viewer.
        MediaPageSource page;
        if (application_image_viewer_page_source(&page))
            application_tabs_open_media(page, false);
        return;
    }
    case ui_image_viewer_copy:
        copy_image();
        return;
    case ui_image_viewer_fullscreen:
        set_fullscreen(!g_app.image_viewer.fullscreen);
        return;
    case ui_image_viewer_previous:
    case ui_image_viewer_next:
        navigate(index == ui_image_viewer_next ? 1 : -1);
        return;
    default:
        ui_image_viewer_apply(&scene, index == ui_image_viewer_zoom_out  ? UiImageViewerCommand::zoom_out
                                      : index == ui_image_viewer_zoom_in ? UiImageViewerCommand::zoom_in
                                                                         : UiImageViewerCommand::fit);
    }
}
u32 find_item(MessageSceneRichItemKind kind, u32 index, MediaEmbedResource resource) noexcept {
    const auto& viewer = g_app.image_viewer;
    for (u32 i = 0; i < viewer.item_count; ++i)
        if (viewer.items[i].kind == kind && viewer.items[i].index == index &&
            (resource == MediaEmbedResource::none || viewer.items[i].resource == resource))
            return i;
    return kApplicationMediaViewerItems;
}
// The clicked item; its painted tier/size override the gallery estimates.
struct OpenRequest {
    UiRect origin;
    MessageSceneRichItemKind kind;
    u32 index;
    MediaEmbedResource resource;
    bool embed_snapshot;
    MediaAssetKey key, fallback;
    UiSize pixels;
};
// Shared tail of every open path once source identity fields are recorded.
bool begin(const OpenRequest& request) noexcept {
    auto& viewer = g_app.image_viewer;
    ApplicationSearchSourceView source{};
    viewer.item_count = 0;
    application_image_zoom_end();
    viewer.item_index = 0;
    if (application_image_viewer_source(&source))
        viewer.item_count =
            static_cast<u8>(collect_items(*source.rich_store, *source.rich, request.embed_snapshot, viewer.items));
    u32 start = find_item(request.kind, request.index, request.resource);
    if (start >= viewer.item_count) {
        // A painted embed thumbnail is not the embed's primary media; show it alone.
        if (!media_asset_key_valid(request.key) || request.kind != MessageSceneRichItemKind::embed)
            return false;
        start = 0;
        viewer.item_count = 1;
        viewer.items[0] = {request.key,      request.fallback, request.pixels, 0, request.kind, request.resource,
                           static_cast<u8>(request.index), false};
    }
    auto& item = viewer.items[start];
    if (media_asset_key_valid(request.key))
        item.key = request.key;
    if (media_asset_key_valid(request.fallback))
        item.fallback = request.fallback;
    if (ui_size_valid(request.pixels))
        item.pixels = request.pixels;
    viewer.origin_item = static_cast<u8>(start);
    viewer.closing = false;
    viewer.origin = request.origin;
    ++viewer.reveal_generation;
    ui_motion_transition_reset(&viewer.reveal, reveal_scope(), application_reduced_motion() ? 1.0f : 0.0f);
    ui_image_viewer_set_reveal(&viewer.scene, viewer.reveal.value, request.origin);
    viewer.item_index = static_cast<u8>(start);
    select_item(start);
    application_composer_text_input_suspend();
    message_action_bar_close(&g_app.message_action_bar);
    if (!application_image_viewer_synchronize()) {
        application_image_viewer_close();
        return false;
    }
    window_focus(g_app.window);
    application_request_frame();
    return true;
}
void record_identity(const MessageRecord& message, MessageHandle handle, bool snapshot) noexcept {
    auto& viewer = g_app.image_viewer;
    viewer.message_id = message.id;
    viewer.channel_id = message.channel_id;
    viewer.message_revision = message.revision;
    viewer.source_slot = handle.slot;
    viewer.source_generation = handle.generation;
    viewer.attachment_snapshot = snapshot;
    viewer.account_scope = g_app.plugin_account_scope_serial;
    viewer.profile_id = g_app.account.status.profile_id;
    viewer.self_user_id = g_app.account_ui->chat_state.self_user_id;
}
// Opens the viewer on a timeline message's item; request.pixels sizes its first layout.
void open_local(const MessageRecord& message, MessageHandle handle, bool snapshot, const OpenRequest& request) noexcept {
    auto& viewer = g_app.image_viewer;
    if (!ui_image_viewer_open(&viewer.scene, viewport(), request.pixels))
        return;
    reset_value(&viewer.search_source);
    viewer.search_result = false;
    record_identity(message, handle, snapshot);
    if (!begin(request))
        application_image_viewer_close();
}
// The line under a media page's title: "#channel · Server · author", each part that is known. The view stays
// valid until the next call.
char g_page_source_line[192];
Utf8View page_source_line(u64 channel_id, u64 author_id) noexcept {
    u32 bytes = 0;
    const auto append = [&](Utf8View prefix, Utf8View part) noexcept {
        if (!part.bytes)
            return;
        const Utf8View separator = bytes ? utf8_literal(" \xC2\xB7 ") : Utf8View{};
        const u32 room = sizeof(g_page_source_line) - bytes;
        if (separator.bytes + prefix.bytes >= room)
            return;
        copy_bytes(g_page_source_line + bytes, separator.data, separator.bytes);
        copy_bytes(g_page_source_line + bytes + separator.bytes, prefix.data, prefix.bytes);
        bytes += separator.bytes + prefix.bytes;
        const u32 kept = utf8_truncated_length(part, sizeof(g_page_source_line) - bytes);
        copy_bytes(g_page_source_line + bytes, part.data, kept);
        bytes += kept;
    };
    const auto& chat = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(chat, channel_id);
    const auto* guild = channel && channel->guild_id ? chat_state_store_find_guild(chat, channel->guild_id) : nullptr;
    const auto* author = user_store_find(g_app.account_ui->user_store, author_id);
    if (channel)
        append(guild ? utf8_literal("#") : Utf8View{}, chat_state_store_channel_name(chat, *channel));
    if (guild)
        append({}, chat_state_store_guild_name(chat, *guild));
    if (author)
        append({}, user_store_name(g_app.account_ui->user_store, *author));
    return {g_page_source_line, bytes};
}
// The tier a video's poster is requested at: the inline card's, so the page shows its cached first frame until
// decoded frames replace it under the same key.
MediaResolutionTier page_video_tier(u32 source_width, u32 source_height) noexcept {
    float width = 400, height = 225;
    if (source_width && source_height) {
        const float scale = minimum(1.0f, minimum(550.0f / source_width, 350.0f / source_height));
        width = source_width * scale;
        height = source_height * scale;
    }
    return media_presentation_request(source_width, source_height, width, height, g_app.renderer.dpi,
                                      media_preview_extent_cap(MediaAttachmentFormat::mp4))
        .resolution;
}
// A page's title: the media's own, else its kind's name.
Utf8View page_title(Utf8View title, MediaPageKind kind) noexcept {
    return title.bytes                  ? title
           : kind == MediaPageKind::video ? utf8_literal("Video")
           : kind == MediaPageKind::audio ? utf8_literal("Audio")
                                          : utf8_literal("Image");
}
} // namespace
UiImageViewerKey application_image_viewer_key(u32 code) noexcept {
    switch (code) {
    case key::escape: return UiImageViewerKey::escape;
    case key::tab: return UiImageViewerKey::tab;
    case key::enter: return UiImageViewerKey::enter;
    case key::space: return UiImageViewerKey::space;
    case key::left: return UiImageViewerKey::left;
    case key::right: return UiImageViewerKey::right;
    case key::up: return UiImageViewerKey::up;
    case key::down: return UiImageViewerKey::down;
    case key::home: return UiImageViewerKey::home;
    case key::add: case key::equals: return UiImageViewerKey::plus;
    case key::subtract: case key::minus: return UiImageViewerKey::minus;
    case '0': case key::numpad0: return UiImageViewerKey::zero;
    case '1': case key::numpad1: return UiImageViewerKey::one;
    case 'F': return UiImageViewerKey::f;
    case 'S': return UiImageViewerKey::s;
    case 'C': return UiImageViewerKey::c;
    default: return UiImageViewerKey::other;
    }
}
bool application_image_copy(const MediaAssetKey& key, const MediaAssetKey& fallback, Utf8View url) noexcept {
    // The decoded viewer texture is bounded by the 2048 DIP decode tier.
    constexpr u32 capacity = 2048u * 2048u * 4u;
    auto* pixels = static_cast<u8*>(commit_pages(capacity));
    if (!pixels)
        return false;
    u32 width = 0, height = 0;
    const bool read = renderer_read_media_rgba(&g_app.renderer, key, {pixels, capacity}, &width, &height) ||
                      renderer_read_media_rgba(&g_app.renderer, fallback, {pixels, capacity}, &width, &height);
    const bool copied = read && clipboard_write_image(pixels, width, height, width * 4, url);
    pages_release(pixels);
    application_toast_result(copied, kToastImageCopied, kToastImageCopyFailed);
    return copied;
}
bool application_image_viewer_page_source(MediaPageSource* out) noexcept {
    const auto& viewer = g_app.image_viewer;
    const auto* item = current_item();
    ApplicationSearchSourceView source{};
    if (!item || viewer.closing || !media_asset_key_valid(item->key) || !application_image_viewer_source(&source) ||
        !viewer_item_visible(source, *item))
        return false;
    const auto& store = *source.rich_store;
    const auto& rich = *source.rich;
    // An embed picture is fetched through its media proxy URL; the rest through the URL they open. An embed video
    // plays from what its session plays (the provider page, its playlist recovered from the message).
    Utf8View url{}, title{};
    bool provider = false;
    if (item->kind == MessageSceneRichItemKind::attachment) {
        if (item->index >= rich.attachment_count || rich.attachments[item->index].id != item->attachment_id)
            return false;
        url = message_rich_store_string(store, rich.attachments[item->index].url);
        title = message_rich_store_string(store, rich.attachments[item->index].filename);
    } else {
        MessageRichEmbedMediaSelection selected{};
        if (item->index >= rich.embed_count ||
            !message_rich_embed_resource_select(store, rich.embeds[item->index], item->resource, &selected) ||
            (item->video && !application_video_viewer_url(&url, &provider)))
            return false;
        if (!item->video)
            url = selected.source;
        title = message_rich_store_string(store, rich.embeds[item->index].title);
    }
    if (!url.bytes)
        return false;
    if (!out)
        return true;
    if (!title.bytes)
        title = item_caption(source, *item);
    const bool embed = item->kind == MessageSceneRichItemKind::embed;
    const MediaPageKind kind = item->video ? MediaPageKind::video : MediaPageKind::image;
    *out = {kind,
            static_cast<u8>((embed ? media_page_embed : 0) | (viewer.attachment_snapshot ? media_page_snapshot : 0) |
                            (provider ? media_page_provider : 0) |
                            (item->video && embed_collection_url(url) ? media_page_playlist : 0)),
            url,
            page_title(title, kind),
            page_source_line(viewer.channel_id, source.message->author_id),
            item->key,
            item->fallback,
            item->pixels,
            viewer.channel_id,
            viewer.message_id,
            viewer.message_revision};
    // Open original leaves for the embed's own link (the page it describes, with the message's playlist), not the
    // media it plays.
    if (embed)
        out->link = youtube_collection_source(message_rich_store_string(store, rich.embeds[item->index].url),
                                              viewer.attachment_snapshot
                                                  ? message_rich_store_string(store, rich.snapshot.content)
                                                  : message_store_content(*source.store, source.handle));
    return true;
}
bool application_image_viewer_item_page_source(u64 message_id, u32 revision, bool snapshot, bool embed, u64 item,
                                               MediaPageSource* out) noexcept {
    if (!g_app.account_ui || g_app.login.visible)
        return false;
    const auto found = message_store_find(g_app.message_view.store, message_id);
    const auto* message = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    const auto& store = g_app.account_ui->message_rich_store;
    const auto* rich = !message ? nullptr
                       : snapshot ? message_rich_store_snapshot(store, message_id)
                                  : message_rich_store_find(store, message_id);
    if (!rich || message->revision != revision || rich->message_revision != revision ||
        message->channel_id != g_app.account_ui->active_channel_id ||
        !application_channel_readable(g_app, message->channel_id))
        return false;
    Utf8View url{}, title{}, art{}, link{};
    MediaAssetKey key{}, fallback{};
    UiSize pixels{};
    MediaPageKind kind = MediaPageKind::image;
    u8 flags = snapshot ? media_page_snapshot : 0;
    if (!embed) {
        u32 index = 0;
        while (index < rich->attachment_count && rich->attachments[index].id != item)
            ++index;
        if (index == rich->attachment_count)
            return false;
        const auto& attachment = rich->attachments[index];
        const auto format = message_rich_attachment_format(store, attachment);
        const auto audio = message_rich_attachment_audio_format(store, attachment);
        url = message_rich_store_string(store, attachment.url);
        title = message_rich_store_string(store, attachment.filename);
        if (media_attachment_format_audio_supported(audio)) {
            kind = MediaPageKind::audio;
            key = media_asset_attachment(attachment.id, audio, MediaResolutionTier::original);
        } else if (format == MediaAttachmentFormat::mp4) {
            kind = MediaPageKind::video;
            key = fallback = media_asset_attachment(attachment.id, format, page_video_tier(attachment.width, attachment.height));
            if (attachment.width && attachment.height)
                pixels = {static_cast<float>(attachment.width), static_cast<float>(attachment.height)};
        } else if (format != MediaAttachmentFormat::unknown && format != MediaAttachmentFormat::lottie) {
            attachment_picture(nullptr, store, attachment, format, &pixels, &key, &fallback);
        } else {
            return false;
        }
    } else {
        if (item >= rich->embed_count || item > 0xFF)
            return false;
        const u8 index = static_cast<u8>(item);
        const auto& value = rich->embeds[index];
        flags |= media_page_embed;
        title = message_rich_store_string(store, value.title);
        link = youtube_collection_source(message_rich_store_string(store, value.url),
                                         snapshot ? message_rich_store_string(store, rich->snapshot.content)
                                                  : message_store_content(g_app.message_view.store, found.handle));
        MessageRichEmbedMediaSelection media{};
        if (value.type != GatewayMessageEmbedType::gifv && message_rich_embed_playback_select(store, value, &media) &&
            (media.format == MediaAttachmentFormat::mp4 || media_embed_audio_format_supported(media.format))) {
            // A provider or direct video or audio; its playlist is recovered from the message as the player does.
            kind = media.format == MediaAttachmentFormat::mp4 ? MediaPageKind::video : MediaPageKind::audio;
            key = media_asset_embed(message_id, revision, index, media.resource, media.format, media.animated,
                                    page_video_tier(media.media->width, media.media->height), snapshot);
            InlineMediaSource playing{};
            playing.key = key;
            playing.message_id = message_id;
            playing.channel_id = message->channel_id;
            playing.revision = revision;
            playing.index = index;
            playing.embed = true;
            playing.snapshot = snapshot;
            u32 width = 0, height = 0;
            if (!video_detail::resolve_message_video_url(playing, &url, &width, &height))
                return false;
            if (width && height)
                pixels = {static_cast<float>(width), static_cast<float>(height)};
            flags |= (media.provider ? media_page_provider : 0) | (embed_collection_url(url) ? media_page_playlist : 0);
            if (kind == MediaPageKind::video)
                fallback = key;
            // An audio embed's own picture, when it has one, is the artwork the page shows.
            MessageRichEmbedMediaSelection thumbnail{};
            if (kind == MediaPageKind::audio && message_rich_embed_media_select(store, value, &thumbnail) &&
                thumbnail.format != MediaAttachmentFormat::unknown && thumbnail.format != MediaAttachmentFormat::mp4 &&
                !media_embed_audio_format_supported(thumbnail.format)) {
                MediaAssetKey large{};
                embed_picture(false, message_id, revision, index, snapshot, thumbnail, &pixels, &large, &fallback);
                art = thumbnail.source;
            }
        } else {
            MessageRichEmbedGalleryPlace place{};
            const bool tile = message_rich_embed_gallery_place(store, *rich, index, &place);
            MessageRichEmbedMediaSelection selected{};
            if ((!tile && message_rich_embed_merge_host(store, *rich, index) != index) ||
                !(tile ? message_rich_embed_gallery_select(store, value, &selected)
                       : message_rich_embed_media_select(store, value, &selected)) ||
                selected.format == MediaAttachmentFormat::unknown ||
                (selected.format == MediaAttachmentFormat::mp4 && value.type != GatewayMessageEmbedType::gifv))
                return false;
            url = selected.source;
            embed_picture(false, message_id, revision, index, snapshot, selected, &pixels, &key, &fallback);
        }
    }
    if (!url.bytes || !media_asset_key_valid(key))
        return false;
    if (!out)
        return true;
    *out = {kind,
            flags,
            url,
            page_title(title, kind),
            page_source_line(message->channel_id, message->author_id),
            key,
            fallback,
            pixels,
            message->channel_id,
            message_id,
            revision,
            art,
            link};
    return true;
}
bool application_image_viewer_source(ApplicationSearchSourceView* out) noexcept {
    const auto& viewer = g_app.image_viewer;
    reset_value(out);
    if (!viewer.scene.active || !g_app.account_ui || !g_app.account.instance || g_app.account.stop_failed || g_app.login.visible ||
        viewer.account_scope != g_app.plugin_account_scope_serial || viewer.profile_id != g_app.account.status.profile_id ||
        viewer.self_user_id != g_app.account_ui->chat_state.self_user_id) return false;
    if (viewer.search_result) {
        if (!application_search_source_resolve(viewer.search_source, viewer.attachment_snapshot, out)) return false;
    } else {
        const auto found = message_store_find(g_app.message_view.store, viewer.message_id);
        const auto* message = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
        if (!message || g_app.account_ui->active_channel_id != viewer.channel_id) return false;
        out->store = &g_app.message_view.store; out->message = message; out->handle = found.handle;
        out->rich_store = &g_app.account_ui->message_rich_store;
        out->rich = viewer.attachment_snapshot ? message_rich_store_snapshot(*out->rich_store, message->id)
                                               : message_rich_store_find(*out->rich_store, message->id);
        out->snapshot = viewer.attachment_snapshot;
    }
    return out->message && application_channel_readable(g_app, out->message->channel_id) &&
        out->rich_store && out->rich && out->message->id == viewer.message_id &&
        out->message->channel_id == viewer.channel_id && out->message->revision == viewer.message_revision &&
        out->rich->message_revision == viewer.message_revision && out->handle.slot == viewer.source_slot &&
        out->handle.generation == viewer.source_generation;
}
void application_image_viewer_close() noexcept {
    auto& scene = g_app.image_viewer.scene;
    if (!scene.active)
        return;
    scene.active = false;
    application_image_zoom_end();
    ++scene.revision;
    g_app.image_viewer.closing = false;
    g_app.image_viewer.item_count = 0;
    if (g_app.image_viewer.fullscreen) {
        application_window_set_fullscreen(false);
        g_app.image_viewer.fullscreen = false;
    }
    const bool capture = scene.dragging || scene.widgets.captured.generation;
    g_app.image_viewer.pointer_tail = g_app.image_viewer.pointer_tail || capture;
    if (scene.keyboard.pressed.manager && !g_app.image_viewer.held_key) g_app.image_viewer.held_key = key::space;
    scene.dragging = false;
    ui_button_pointer(&scene.widgets, UiButtonPointerEvent::cancel, {});
    ui_button_keyboard_cancel(&scene.keyboard);
    if (capture)
        application_release_capture();
    // The playing session returns to its inline preview at the same position.
    application_video_viewer_detach();
    g_app.renderer.image_viewer = nullptr;
    application_repaint();
}
void application_image_viewer_dismiss() noexcept {
    auto& viewer = g_app.image_viewer;
    if (!viewer.scene.active || viewer.closing)
        return;
    if (application_reduced_motion()) {
        application_image_viewer_close();
        return;
    }
    // Restore the window first so the exit motion lands on the inline preview.
    set_fullscreen(false);
    viewer.closing = true;
    // Only the item that opened from a preview can travel back into it.
    if (viewer.item_index != viewer.origin_item)
        reset_value(&viewer.origin);
    ui_button_pointer(&viewer.scene.widgets, UiButtonPointerEvent::cancel, {});
    application_video_cancel_pointer();
    application_request_frame();
}
bool application_image_viewer_video_geometry(const MediaAssetKey& key, UiRect* preview, UiRect* clip) noexcept {
    const auto& viewer = g_app.image_viewer;
    if (!viewer.scene.active || !current_video() || !media_asset_key_valid(viewer.key) ||
        !media_asset_key_equal(key, viewer.key))
        return false;
    *preview = viewer.scene.display;
    *clip = viewer.scene.viewport;
    return true;
}
void application_image_viewer_request_failed(const MediaAssetKey& key) noexcept {
    auto& viewer = g_app.image_viewer;
    if (!viewer.scene.active || !media_asset_key_valid(key) || !media_asset_key_equal(key, viewer.key) ||
        media_asset_key_equal(key, viewer.failed_key))
        return;
    viewer.failed_key = key;
    application_request_frame();
}
u32 application_image_viewer_request_keys(MediaAssetKey* keys, u32 capacity) noexcept {
    const auto& viewer = g_app.image_viewer;
    if (!viewer.scene.active || viewer.closing)
        return 0;
    ApplicationSearchSourceView source{};
    if (!application_image_viewer_source(&source))
        return 0;
    u32 count = 0;
    if (!current_video() && current_item() && viewer_item_visible(source, *current_item()) &&
        media_asset_key_valid(viewer.key))
        keys[count++] = viewer.key;
    // One neighbour on each side keeps arrow navigation instant without
    // letting a large gallery evict the preview atlas.
    static constexpr i32 steps[]{1, -1};
    for (const i32 step : steps) {
        if (viewer.item_count < 2 || count >= capacity)
            break;
        const u32 index = ui_image_viewer_step(viewer.item_index, viewer.item_count, step);
        const auto& item = viewer.items[index];
        if (index == viewer.item_index || item.video || !viewer_item_visible(source, item) ||
            !media_asset_key_valid(item.key) || (count && media_asset_key_equal(keys[count - 1], item.key)))
            continue;
        keys[count++] = item.key;
    }
    return count;
}
bool application_image_viewer_key_current(const MediaAssetKey& key) noexcept {
    MediaAssetKey keys[3]{};
    const u32 count = application_image_viewer_request_keys(keys, 3);
    for (u32 i = 0; i < count; ++i)
        if (media_asset_key_equal(key, keys[i]))
            return true;
    return false;
}
bool application_image_viewer_synchronize() noexcept {
    auto& viewer = g_app.image_viewer;
    auto& scene = viewer.scene;
    if (!scene.active)
        return !g_app.account_ui || !g_app.renderer.interaction_text_image_viewer_revision ||
               renderer_rebuild_content_text(&g_app.renderer, application_theme());
    ApplicationSearchSourceView source{};
    if (!application_image_viewer_source(&source)) {
        application_image_viewer_close();
        return renderer_rebuild_content_text(&g_app.renderer, application_theme());
    }
    auto* item = current_item();
    if (item && !viewer_item_visible(source, *item)) {
        application_image_viewer_close();
        return renderer_rebuild_content_text(&g_app.renderer, application_theme());
    }
    UiSize pixels = scene.pixels;
    if (item && item->kind == MessageSceneRichItemKind::attachment) {
        for (u32 i = 0; i < source.rich->attachment_count; ++i) {
            const auto& attachment = source.rich->attachments[i];
            if (attachment.id == item->attachment_id && attachment.width && attachment.height)
                pixels = {static_cast<float>(attachment.width), static_cast<float>(attachment.height)};
        }
    } else if (item && item->kind == MessageSceneRichItemKind::embed) {
        MessageRichEmbedMediaSelection media{};
        if (item->index < source.rich->embed_count && message_rich_embed_resource_select(*source.rich_store,
            source.rich->embeds[item->index], item->resource, &media) && media.media->width && media.media->height)
            pixels = {static_cast<float>(media.media->width), static_cast<float>(media.media->height)};
    }
    if (item && item->video) {
        MediaAtlasResult frame{};
        frame.error = MediaAtlasError::not_found;
        if (g_app.renderer.preview_media_atlas)
            frame = media_atlas_peek(g_app.renderer.preview_media_atlas, viewer.key);
        if (!frame.ok() && g_app.renderer.media_atlas)
            frame = media_atlas_peek(g_app.renderer.media_atlas, viewer.key);
        if (frame.ok() && frame.width && frame.height)
            pixels = {static_cast<float>(frame.width), static_cast<float>(frame.height)};
    }
    if (pixels.width != scene.pixels.width || pixels.height != scene.pixels.height) {
        scene.pixels = pixels; reset_value(&scene.viewport);
        if (item) item->pixels = pixels;
    }
    ui_image_viewer_set_dpi(&scene, g_app.renderer.dpi);
    if (!ui_image_viewer_layout(&scene, viewport())) {
        application_image_viewer_close();
        return renderer_rebuild_content_text(&g_app.renderer, application_theme());
    }
    const u64 now = clock_monotonic_milliseconds();
    ui_motion_transition_tick(&viewer.reveal, reveal_scope(), viewer.closing ? 0.0f : 1.0f, now,
                              viewer.closing ? kRevealClose : kRevealOpen, application_reduced_motion());
    ui_image_viewer_set_reveal(&scene, viewer.reveal.value, viewer.origin);
    if (viewer.closing && !viewer.reveal.active && viewer.reveal.value <= 0) {
        application_image_viewer_close();
        return renderer_rebuild_content_text(&g_app.renderer, application_theme());
    }
    if (viewer.reveal.active)
        application_request_frame();
    if (scene.copied && now >= viewer.copied_until_ms) {
        scene.copied = false;
        ++scene.revision;
    } else if (scene.copied) {
        application_request_frame();
    }
    const bool embed = item && item->kind == MessageSceneRichItemKind::embed && !viewer.search_result;
    const bool save =
        embed ? application_downloads_embed_available(viewer.message_id, item->index,
                                                      viewer.attachment_snapshot)
              : viewer.attachment_id &&
                    (viewer.search_result
                         ? application_downloads_search_available(
                               viewer.search_source, viewer.attachment_id, viewer.attachment_snapshot)
                         : application_downloads_available(viewer.message_id, viewer.attachment_id,
                                                           viewer.attachment_snapshot));
    ui_image_viewer_set_save_available(&scene, save);
    ui_image_viewer_set_actions(&scene, ui_image_viewer_action_open | ui_image_viewer_action_copy |
                                            (application_tabs_visible() && application_image_viewer_page_source(nullptr)
                                                 ? ui_image_viewer_action_tab
                                                 : 0));
    ui_image_viewer_set_fullscreen(&scene, viewer.fullscreen);
    ui_image_viewer_set_chrome(&scene, current_video() ? application_video_viewer_chrome() : 1.0f);
    if (!current_video()) {
        // What the renderer draws: the large tier, the inline tier it opened
        // from, or presented animation frames in the primary atlas.
        auto* preview = g_app.renderer.preview_media_atlas;
        auto* primary = g_app.renderer.media_atlas;
        const bool resident = (preview && (media_atlas_peek(preview, viewer.key).ok() ||
                                           media_atlas_peek(preview, viewer.fallback).ok())) ||
                              (primary && media_atlas_peek(primary, viewer.key).ok());
        const bool failed =
            media_asset_key_valid(viewer.failed_key) && media_asset_key_equal(viewer.failed_key, viewer.key);
        ui_image_viewer_set_media(&scene, resident ? UiImageViewerMedia::ready
                                          : failed ? UiImageViewerMedia::failed
                                                   : UiImageViewerMedia::loading);
    }
    g_app.renderer.image_viewer = &scene;
    g_app.renderer.image_viewer_key = viewer.key;
    g_app.renderer.image_viewer_fallback = viewer.fallback;
    if (scene.keyboard_focus != g_app.keyboard_focus_visible) {
        scene.keyboard_focus = g_app.keyboard_focus_visible;
        ++scene.revision;
    }
    return g_app.renderer.interaction_text_image_viewer_revision == scene.revision ||
           renderer_rebuild_content_text(&g_app.renderer, application_theme());
}
bool application_image_viewer_open_at(float x, float y, bool probe) noexcept {
    if (!g_app.account_ui || g_app.login.visible || g_app.popup_scene.active ||
        g_app.reaction_viewer.active || (g_app.renderer.text_form && g_app.renderer.text_form->scene && g_app.renderer.text_form->scene->active))
        return false;
    const auto& scene = g_app.message_scene;
    if (!scene.active || !ui_rect_contains(scene.viewport, x, y))
        return false;
    const auto& store = g_app.account_ui->message_rich_store;
    for (u32 row_index = 0; row_index < scene.count; ++row_index) {
        for (u32 content_scope = 0; content_scope < 2; ++content_scope) {
        const auto* row_pointer = content_scope ? message_scene_snapshot_row(scene, scene.rows[row_index])
                                               : &scene.rows[row_index];
        if (!row_pointer) continue;
        const auto& row = *row_pointer;
        const auto* message = message_store_resolve(g_app.message_view.store, row.message);
        const auto* rich = message ? (content_scope ? message_rich_store_snapshot(store, message->id)
                                                  : message_rich_store_find(store, message->id)) : nullptr;
        if (!rich || !application_channel_readable(g_app, message->channel_id) || rich->message_revision != message->revision)
            continue;
        for (u32 index = 0; index < rich->attachment_count; ++index) {
            const auto* node = message_scene_rich_item_layout_node(
                scene, row, MessageSceneRichItemKind::attachment, index);
            const auto& attachment = rich->attachments[index];
            const auto format = message_rich_attachment_format(store, attachment);
            float width = 0, height = 0;
            if (!node || format == MediaAttachmentFormat::mp4 ||
                !message_rich_attachment_preview_extent(
                    store, attachment, ui_rect_width(node->content_rectangle), &width, &height))
                continue;
            const UiRect preview{node->rectangle.left, node->rectangle.top, node->rectangle.left + width,
                                 node->rectangle.top + height};
            if (!ui_rect_contains(preview, x, y))
                continue;
            const auto owner = message_media_owner(*message, message_rich_store_find(store, message->id));
            const auto key = message_media_reveal_key(message->id, MessageMediaSource::attachment, index,
                                                      content_scope != 0);
            if (probe)
                return !message_media_obscure_notice(message_scene_media_obscure(
                    scene, owner, message->id, key, message_rich_attachment_spoiler(store, attachment),
                    message_rich_attachment_explicit(attachment)));
            if (application_media_activation_consumed(g_app.message_scene, owner, message->id, key,
                                                      message_rich_attachment_spoiler(store, attachment),
                                                      message_rich_attachment_explicit(attachment)))
                return true;
            OpenRequest request{preview, MessageSceneRichItemKind::attachment, index, MediaEmbedResource::none,
                                row.forwarded_snapshot};
            // Keep the clicked preview's exact painted tier as the fallback.
            request.fallback =
                media_asset_attachment(attachment.id, format,
                                       media_resolution_tier_for_display(width, height, g_app.renderer.dpi,
                                                                         media_preview_extent_cap(format)));
            request.pixels = attachment.width && attachment.height
                                 ? UiSize{static_cast<float>(attachment.width), static_cast<float>(attachment.height)}
                                 : UiSize{width, height};
            open_local(*message, row.message, content_scope != 0, request);
            return true;
        }
        for (u32 member = 0; member < rich->embed_count; ++member) {
            // A gallery image opens from its tile in the first embed's gallery.
            MessageRichEmbedGalleryPlace place{};
            const bool tile = message_rich_embed_gallery_place(store, *rich, member, &place);
            const u32 index = tile ? place.host : member;
            const auto* node =
                message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::embed, index);
            MessageRichEmbedMediaSelection selected{};
            const auto& embed = rich->embeds[index];
            if (!node || (!tile && message_rich_embed_merge_host(store, *rich, member) != member) ||
                !(tile ? message_rich_embed_gallery_select(store, rich->embeds[member], &selected)
                       : message_rich_embed_media_select(store, embed, &selected)) ||
                (selected.format == MediaAttachmentFormat::mp4 &&
                 rich->embeds[member].type != GatewayMessageEmbedType::gifv))
                continue;
            UiRect preview = message_rich_embed_media_rect(row.rich_item_layout, index, node->content_rectangle,
                                                           message_rich_embed_media_only(embed));
            if (tile)
                preview = message_rich_embed_gallery_tile(preview, place.count, place.ordinal);
            const float width = ui_rect_width(preview), height = ui_rect_height(preview);
            if (!ui_rect_has_area(preview) || !ui_rect_contains(preview, x, y))
                continue;
            const auto owner = message_media_owner(*message, message_rich_store_find(store, message->id));
            const auto key = message_media_reveal_key(message->id, MessageMediaSource::embed, member,
                                                      content_scope != 0);
            if (probe)
                return !message_media_obscure_notice(
                    message_scene_media_obscure(scene, owner, message->id, key, false,
                                                message_rich_embed_explicit(rich->embeds[member])));
            if (application_media_activation_consumed(g_app.message_scene, owner, message->id, key, false,
                                                      message_rich_embed_explicit(rich->embeds[member])))
                return true;
            OpenRequest request{preview, MessageSceneRichItemKind::embed, member, MediaEmbedResource::none,
                                row.forwarded_snapshot};
            const auto tier = media_resolution_tier_for_display(width, height, g_app.renderer.dpi,
                                                                media_preview_extent_cap(selected.format, selected.animated));
            request.fallback =
                media_asset_embed(message->id, message->revision, static_cast<u8>(member), selected.resource,
                                  selected.format, selected.animated, tier, row.forwarded_snapshot);
            request.pixels = selected.media->width && selected.media->height
                                 ? UiSize{static_cast<float>(selected.media->width),
                                          static_cast<float>(selected.media->height)}
                                 : UiSize{width, height};
            open_local(*message, row.message, content_scope != 0, request);
            return true;
        }
    }
        }
    return false;
}
bool application_image_viewer_open_search(const ApplicationSearchSourceTicket& ticket,
    MessageSceneRichItemKind kind, u32 index, bool snapshot, const UiRect& painted_preview, MediaEmbedResource resource) noexcept {
    if (!g_app.account_ui || g_app.login.visible || g_app.popup_scene.active || application_downloads_active() ||
        g_app.reaction_viewer.active || !ui_rect_has_area(painted_preview) ||
        (g_app.renderer.text_form && g_app.renderer.text_form->scene && g_app.renderer.text_form->scene->active)) return false;
    ApplicationSearchSourceView source{};
    if (!application_search_source_resolve(ticket, snapshot, &source) || !source.message || !source.rich_store || !source.rich ||
        source.message->delivery != MessageDeliveryState::authoritative || source.rich->message_revision != source.message->revision) return false;
    const auto& store = *source.rich_store;
    const auto& rich = *source.rich;
    auto* policy_scene = viewer_media_scene(true);
    if (!policy_scene)
        return false;
    const float width = ui_rect_width(painted_preview), height = ui_rect_height(painted_preview);
    MediaAttachmentFormat format{}; MediaAssetKey key{}, fallback{}; UiSize pixels{};
    MessageRichEmbedMediaSelection selected{};
    if (kind == MessageSceneRichItemKind::attachment) {
        if (index >= rich.attachment_count || resource != MediaEmbedResource::none) return false;
        const auto& attachment = rich.attachments[index];
        format = message_rich_attachment_format(store, attachment);
        if (format == MediaAttachmentFormat::unknown || format == MediaAttachmentFormat::mp4) return false;
        pixels = attachment.width && attachment.height
            ? UiSize{static_cast<float>(attachment.width), static_cast<float>(attachment.height)} : UiSize{width, height};
        fallback = media_asset_search_attachment(attachment.id, format,
            media_resolution_tier_for_display(width, height, g_app.renderer.dpi, media_preview_extent_cap(format)), ticket.publication_revision, snapshot);
        key = media_asset_search_attachment(attachment.id, format,
            media_preview_extent_cap(format) > 512 ? MediaResolutionTier::px2048 : MediaResolutionTier::px512, ticket.publication_revision, snapshot);
    } else if (kind == MessageSceneRichItemKind::embed) {
        if (index >= rich.embed_count) return false;
        const bool selected_ok = resource == MediaEmbedResource::none
            ? message_rich_embed_media_select(store, rich.embeds[index], &selected)
            : message_rich_embed_resource_select(store, rich.embeds[index], resource, &selected);
        if (!selected_ok || selected.format == MediaAttachmentFormat::unknown ||
            (selected.format == MediaAttachmentFormat::mp4 && rich.embeds[index].type != GatewayMessageEmbedType::gifv))
            return false;
        format = selected.format;
        pixels = selected.media->width && selected.media->height
            ? UiSize{static_cast<float>(selected.media->width), static_cast<float>(selected.media->height)} : UiSize{width, height};
        fallback = media_asset_search_embed(source.message->id, ticket.publication_revision, static_cast<u8>(index),
            selected.resource, format, selected.animated,
            media_resolution_tier_for_display(width, height, g_app.renderer.dpi, media_preview_extent_cap(format)), snapshot);
        key = media_asset_search_embed(source.message->id, ticket.publication_revision, static_cast<u8>(index),
            selected.resource, format, selected.animated,
            media_preview_extent_cap(format) > 512 ? MediaResolutionTier::px2048 : MediaResolutionTier::px512, snapshot);
    } else return false;
    if (!media_asset_key_valid(key) || !media_asset_key_valid(fallback)) return false;
    if (kind == MessageSceneRichItemKind::attachment) {
        const auto& attachment = rich.attachments[index];
        if (application_media_activation_consumed(
                *policy_scene,
                message_media_owner(*source.message, message_rich_store_find(store, source.message->id)),
                source.message->id,
                message_media_reveal_key(source.message->id, MessageMediaSource::attachment, index,
                                         snapshot),
                message_rich_attachment_spoiler(store, attachment),
                message_rich_attachment_explicit(attachment)))
            return true;
    } else {
        if (application_media_activation_consumed(
                *policy_scene,
                message_media_owner(*source.message, message_rich_store_find(store, source.message->id)),
                source.message->id,
                message_media_reveal_key(source.message->id, MessageMediaSource::embed, index, snapshot),
                false, message_rich_embed_explicit(rich.embeds[index])))
            return true;
    }

    auto& viewer = g_app.image_viewer;
    if (!ui_image_viewer_open(&viewer.scene, viewport(), pixels)) return false;
    viewer.search_source = ticket; viewer.search_result = true;
    record_identity(*source.message, source.handle, snapshot);
    OpenRequest request{painted_preview, kind, index, resource, snapshot, key, fallback, pixels};
    if (kind == MessageSceneRichItemKind::embed)
        request.resource = selected.resource;
    if (!begin(request)) {
        application_image_viewer_close();
        return false;
    }
    return true;
}
bool application_image_viewer_open_video(u64 message_id, u32 revision, bool embed, u8 index, bool snapshot,
                                         const ApplicationSearchSourceTicket* search, const MediaAssetKey& key,
                                         const UiRect& origin) noexcept {
    if (!g_app.account_ui || g_app.login.visible || g_app.popup_scene.active || application_downloads_active() ||
        g_app.reaction_viewer.active || !media_asset_key_valid(key))
        return false;
    ApplicationSearchSourceView source{};
    MessageHandle handle{};
    const MessageRecord* message = nullptr;
    if (search) {
        if (!application_search_source_resolve(*search, snapshot, &source) || !source.message)
            return false;
        message = source.message;
        handle = source.handle;
    } else {
        const auto found = message_store_find(g_app.message_view.store, message_id);
        message = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
        handle = found.handle;
    }
    if (!message || message->id != message_id || message->revision != revision ||
        !application_channel_readable(g_app, message->channel_id))
        return false;
    const auto* store = search ? source.rich_store : &g_app.account_ui->message_rich_store;
    const auto* outer = store ? message_rich_store_find(*store, message_id) : nullptr;
    const auto* rich = snapshot && store ? message_rich_store_snapshot(*store, message_id) : outer;
    auto* policy_scene = viewer_media_scene(search != nullptr);
    if (!policy_scene || !rich || rich->message_revision != revision ||
        (embed ? index >= rich->embed_count : index >= rich->attachment_count))
        return false;
    if (application_media_activation_consumed(
            *policy_scene, message_media_owner(*message, outer), message_id,
            message_media_reveal_key(message_id,
                                     embed ? MessageMediaSource::embed : MessageMediaSource::attachment,
                                     index, snapshot),
            !embed && message_rich_attachment_spoiler(*store, rich->attachments[index]),
            embed ? message_rich_embed_explicit(rich->embeds[index])
                  : message_rich_attachment_explicit(rich->attachments[index])))
        return false;
    auto& viewer = g_app.image_viewer;
    if (!ui_image_viewer_open(&viewer.scene, viewport(), {16, 9}))
        return false;
    viewer.search_result = search != nullptr;
    viewer.search_source = search ? *search : ApplicationSearchSourceTicket{};
    record_identity(*message, handle, snapshot);
    const auto kind = embed ? MessageSceneRichItemKind::embed : MessageSceneRichItemKind::attachment;
    OpenRequest request{origin, kind, index, MediaEmbedResource::none, snapshot, key, key};
    if (!begin(request)) {
        application_image_viewer_close();
        return false;
    }
    return true;
}
bool application_image_viewer_event(const WindowEvent& event) noexcept {
    auto& viewer = g_app.image_viewer;
    auto& scene = viewer.scene;
    auto& held = viewer.held_key;
    auto& pointer_tail = viewer.pointer_tail;
    if (input_release(event) && pointer_tail) { pointer_tail = false; return true; }
    if (held) {
        if (input_plain_key_up(event) && event.key.key == held) {
            held = 0;
            return true;
        }
        if (event.type == EventType::character || (input_plain_key_down(event) && event.key.key == held))
            return true;
    }
    if (!scene.active)
        return false;
    const bool video = current_video();
    if (event.type == EventType::cursor_query) {
        // Idle fullscreen-style playback hides the pointer with the controls.
        if (!application_video_viewer_hidden_cursor())
            return false;
        application_set_cursor(UiCursor::hidden);
        return true;
    }
    // The exit motion is presentation only; input already belongs underneath
    // once it completes, so it is neither handled nor forwarded meanwhile.
    if (viewer.closing)
        return input_pointer(event) || input_plain_key(event) ||
               (event.type == EventType::character && (event.flags & ev_system) == 0);
    if (input_plain_key(event) && event.key.key == key::ctrl) {
        UiPoint cursor{};
        if (application_pointer_dip(&cursor))
            application_image_zoom_viewer_pointer(cursor, over_image(scene, cursor), g_app.image_zoom.held);
    }
    if (event.type == EventType::pointer_leave || input_focus_lost(event))
        application_image_zoom_end();

    const UiPoint point = input_pointer(event) ? UiPoint{client_dip_x(event), client_dip_y(event)} : UiPoint{};
    if (input_pointer(event) || input_plain_key_down(event))
        application_video_viewer_activity();
    // A double click on an image toggles fit and actual size; elsewhere it is
    // another press.
    if (input_double(event)) {
        const auto hit = ui_widget_manager_hit_test(scene.widgets, point.x, point.y);
        if (!hit.generation && over_image(scene, point)) {
            ui_image_viewer_toggle_zoom(&scene, point);
            scene.dragging = true;
            scene.pointer = point;
            window_capture_pointer(g_app.window);
            application_request_frame();
            return true;
        }
    }
    if (input_down(event)) {
        pointer_tail = false;
        scene.keyboard_focus = false;
        if (const auto button = ui_button_pointer(&scene.widgets, UiButtonPointerEvent::press, point);
            button.handled) {
            scene.focus = static_cast<u8>(scene.widgets.storage[button.target.slot].id - 10);
            ++scene.revision;
            window_capture_pointer(g_app.window);
            application_request_frame();
            return true;
        }
        if (video && application_video_viewer_pointer(VideoPointer::down, point))
            return true;
        if (over_image(scene, point)) {
            scene.dragging = true;
            scene.pointer = point;
            window_capture_pointer(g_app.window);
            // A fitted image has nothing to pan, so a button held on it shows the lens.
            application_image_zoom_viewer_pointer(point, true, ui_image_viewer_fitted(scene));
        } else {
            pointer_tail = true;
            application_image_viewer_dismiss();
        }
        return true;
    }
    if (event.type == EventType::pointer_move) {
        if (const auto button = ui_button_pointer(&scene.widgets, UiButtonPointerEvent::move, point);
            button.changed)
            ++scene.revision;
        if (video)
            application_video_viewer_pointer(VideoPointer::move, point);
        if (scene.dragging) {
            ui_image_viewer_pan(&scene, {point.x - scene.pointer.x, point.y - scene.pointer.y});
            scene.pointer = point;
        }
        application_image_zoom_viewer_pointer(point, over_image(scene, point), g_app.image_zoom.held);
        application_request_frame();
        return true;
    }
    if (input_release(event)) {
        const auto button = ui_button_pointer(&scene.widgets, UiButtonPointerEvent::release, point);
        if (video && !button.handled)
            application_video_viewer_pointer(VideoPointer::up, point);
        scene.dragging = false;
        application_release_capture();
        application_image_zoom_viewer_pointer(point, over_image(scene, point), false);
        if (button.activate)
            activate(scene.widgets.storage[button.target.slot].id - 10);
        if (button.changed)
            ++scene.revision;
        application_request_frame();
        return true;
    }
    if (event.type == EventType::capture_lost || event.type == EventType::cancel_input) {
        scene.dragging = false;
        application_image_zoom_end();
        if (video)
            application_video_cancel_pointer();
        if (ui_button_pointer(&scene.widgets, UiButtonPointerEvent::cancel, {}).changed)
            ++scene.revision;
        return false;
    }
    if (input_vertical_wheel(event)) {
        // Plain and Ctrl+wheel both zoom about the pointer.
        if (video)
            return true;
        if (application_image_zoom_wheel(static_cast<i16>(event.wheel.dy120), input_shift(event)))
            return true;
        ui_image_viewer_wheel(&scene, static_cast<i16>(event.wheel.dy120), client_wheel_dip(event));
        application_request_frame();
        return true;
    }
    if (input_plain_key_down(event)) {
        const u32 code = event.key.key;
        const bool repeat = input_repeated(event);
        const UiImageViewerKeyModifiers modifiers{application_control_down(), application_shift_down(),
                                                  application_alt_down()};
        const auto command = ui_image_viewer_key_command(scene, application_image_viewer_key(code), modifiers);
        // Keys that close or act stay swallowed until released, so their
        // key-up and character never reach the surface underneath.
        if (command == UiImageViewerCommand::dismiss || command == UiImageViewerCommand::save ||
            command == UiImageViewerCommand::copy || code == key::enter)
            held = code;
        switch (command) {
        case UiImageViewerCommand::dismiss:
            application_image_viewer_dismiss();
            break;
        case UiImageViewerCommand::exit_fullscreen:
            set_fullscreen(false);
            break;
        case UiImageViewerCommand::toggle_fullscreen:
            if (!repeat)
                set_fullscreen(!viewer.fullscreen);
            break;
        case UiImageViewerCommand::save:
            if (!repeat)
                activate(ui_image_viewer_save);
            break;
        case UiImageViewerCommand::copy:
            if (!repeat)
                activate(ui_image_viewer_copy);
            break;
        case UiImageViewerCommand::previous:
        case UiImageViewerCommand::next:
            if (!repeat || !video)
                navigate(command == UiImageViewerCommand::next ? 1 : -1);
            break;
        case UiImageViewerCommand::activate: {
            const auto button =
                ui_button_keyboard_key(&scene.keyboard, &scene.widgets,
                                       code == key::enter ? UiButtonKey::enter : UiButtonKey::space, true, repeat);
            if (button.activate)
                activate(scene.widgets.storage[button.target.slot].id - 10);
            break;
        }
        case UiImageViewerCommand::video:
            application_video_key(code, modifiers.shift, repeat);
            break;
        default:
            ui_image_viewer_apply(&scene, command);
            break;
        }
        application_request_frame();
        return true;
    }
    if (input_plain_key_up(event)) {
        if (event.key.key == key::space && !video) {
            const auto button =
                ui_button_keyboard_key(&scene.keyboard, &scene.widgets, UiButtonKey::space, false);
            if (button.activate)
                activate(scene.widgets.storage[button.target.slot].id - 10);
            application_request_frame();
        }
        return true;
    }
    if (input_press(event, PointerButton::right)) {
        // A right press holds the lens at any zoom.
        if (over_image(scene, point) && g_app.preferences.image_zoom) {
            window_capture_pointer(g_app.window);
            application_image_zoom_viewer_pointer(point, true, true);
        }
        return true;
    }
    if (input_release(event, PointerButton::right)) {
        if (g_app.image_zoom.held) {
            application_release_capture();
            application_image_zoom_viewer_pointer(point, over_image(scene, point), false);
        }
        return true;
    }
    return (event.type == EventType::character && (event.flags & ev_system) == 0) ||
           input_press(event, PointerButton::middle) || input_release(event, PointerButton::middle) ||
           event.type == EventType::context_menu || input_horizontal_wheel(event);
}
} // namespace maicord::app_detail
