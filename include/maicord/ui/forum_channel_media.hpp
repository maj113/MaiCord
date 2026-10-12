#pragma once

#include "maicord/ui/thread_browser_scene.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/media/asset.hpp"
#include "maicord/media/presentation_policy.hpp"

namespace maicord {

struct ForumChannelAvatarMedia {
    MediaAssetKey key;
    Utf8View hash;
    UiWidgetReference owner;
    UiRect rectangle, clip;
};

// Borrow only a visible card's author identity from the current forum view.
// The scene validates publication/revision and culls before media admission.
inline bool forum_channel_avatar_media_resolve(const UiThreadBrowserScene& scene, u32 index,
                                               ForumChannelAvatarMedia* out) noexcept {
    UiRect rectangle{}, clip{};
    if (!out || !ui_forum_channel_avatar(scene, index, &rectangle, &clip))
        return false;
    const auto& post = scene.view->forum->posts[index];
    const u32 variant = user_avatar_variant(post.avatar_hash);
    if (!post.author_id || !variant)
        return false;
    *out = {media_asset_user_avatar(post.author_id, variant, asset_hash_animated(post.avatar_hash)),
            post.avatar_hash, {&scene.widgets, scene.backing->rows[index]}, rectangle, clip};
    return true;
}

// First image of a visible post's starter message. The proxy request follows
// the card's media well at the current DPI; rectangle is the aspect-fitted
// image inside the well and clip keeps it there.
struct ForumChannelThumbnailMedia {
    MediaAssetKey key;
    Utf8View url;
    u64 message_id;
    MediaPresentationRequest proxy;
    MediaAttachmentFormat format;
    UiWidgetReference owner;
    UiRect rectangle, clip;
};
inline bool forum_channel_thumbnail_resolve(const UiThreadBrowserScene& scene, u32 index, u32 dpi,
                                            ForumChannelThumbnailMedia* out) noexcept {
    UiRect well{}, clip{};
    if (!out || !dpi || !ui_forum_channel_media(scene, index, &well, &clip))
        return false;
    const auto& post = scene.view->forum->posts[index];
    const auto format = static_cast<MediaAttachmentFormat>(post.media_format);
    if (!post.media_id || !post.media_url.bytes || !post.media_width || !post.media_height ||
        (format != MediaAttachmentFormat::png && format != MediaAttachmentFormat::jpeg &&
         format != MediaAttachmentFormat::webp))
        return false;
    const float scale = minimum(ui_rect_width(well) / static_cast<float>(post.media_width),
                                ui_rect_height(well) / static_cast<float>(post.media_height));
    const float width = static_cast<float>(post.media_width) * scale;
    const float height = static_cast<float>(post.media_height) * scale;
    const float left = (well.left + well.right - width) * .5f, top = (well.top + well.bottom - height) * .5f;
    const UiRect fitted{left, top, left + width, top + height};
    const auto proxy = media_presentation_request(post.media_width, post.media_height, width, height, dpi,
                                                  media_preview_extent_cap(format));
    *out = {media_asset_attachment(post.media_id, format, proxy.resolution), post.media_url, post.thread_id, proxy,
            format, {&scene.widgets, scene.backing->rows[index]}, fitted, ui_rect_intersection(clip, fitted)};
    return ui_rect_has_area(out->clip) && media_asset_key_valid(out->key);
}
inline bool forum_channel_thumbnail_current(const UiThreadBrowserScene& scene, u32 dpi, MediaAssetKey key) noexcept {
    if (key.kind != MediaAssetKind::attachment)
        return false;
    for (u32 i = 0; i < scene.count && i < kUiThreadBrowserPageCapacity; ++i) {
        ForumChannelThumbnailMedia media{};
        if (forum_channel_thumbnail_resolve(scene, i, dpi, &media) && media_asset_key_equal(media.key, key))
            return true;
    }
    return false;
}

inline bool forum_channel_avatar_media_current(const UiThreadBrowserScene& scene, MediaAssetKey key) noexcept {
    for (u32 i = 0; i < scene.count && i < kUiThreadBrowserPageCapacity; ++i) {
        ForumChannelAvatarMedia media{};
        if (forum_channel_avatar_media_resolve(scene, i, &media) && media_asset_key_equal(media.key, key))
            return true;
    }
    return false;
}

} // namespace maicord
