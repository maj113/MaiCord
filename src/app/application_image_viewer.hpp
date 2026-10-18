#pragma once
#include "maicord/ui/image_viewer.hpp"
#include "maicord/ui/transition.hpp"
#include "maicord/media/asset.hpp"
#include "application_search.hpp"
#include "maicord/ui/message_scene.hpp"
namespace maicord::app_detail {
// One previewable message item. Video keys are bound when the item becomes
// current, because the playback session owns their decode identity.
struct ApplicationMediaViewerItem {
    MediaAssetKey key, fallback;
    UiSize pixels;
    u64 attachment_id;
    MessageSceneRichItemKind kind;
    MediaEmbedResource resource;
    u8 index;
    bool video;
};
inline constexpr u32 kApplicationMediaViewerItems = 20;
struct ApplicationImageViewer {
    UiImageViewer scene;
    MediaAssetKey key, fallback;
    u64 message_id, channel_id, attachment_id, account_scope, profile_id, self_user_id;
    bool attachment_snapshot;
    u32 message_revision;
    u32 source_slot, source_generation;
    u32 held_key;
    ApplicationSearchSourceTicket search_source;
    bool search_result, pointer_tail;
    ApplicationMediaViewerItem items[kApplicationMediaViewerItems];
    u8 item_count, item_index, origin_item;
    bool closing;
    // The viewer put the window into full screen and restores it on close.
    bool fullscreen;
    UiRect origin;
    UiMotionTransition reveal;
    u64 reveal_generation, copied_until_ms;
    // The current item's request failed; cleared when the item changes.
    MediaAssetKey failed_key;
};
// Probe reports whether a click would open a preview without opening it.
bool application_image_viewer_open_at(float x, float y, bool probe = false) noexcept;
bool application_image_viewer_open_search(const ApplicationSearchSourceTicket&, MessageSceneRichItemKind,
                                          u32 index, bool snapshot, UiRect painted_preview,
                                          MediaEmbedResource resource = MediaEmbedResource::none) noexcept;
// Expands an inline video: the viewer opens on that message's gallery with
// the playing session as its current item.
bool application_image_viewer_open_video(u64 message_id, u32 revision, bool embed, u8 index, bool snapshot,
                                         const ApplicationSearchSourceTicket* search, MediaAssetKey key,
                                         UiRect origin) noexcept;
// Current viewer rectangle for the expanded video session, if it is shown.
bool application_image_viewer_video_geometry(MediaAssetKey key, UiRect* preview, UiRect* clip) noexcept;
// Current item first, then bounded neighbour images for prefetch.
u32 application_image_viewer_request_keys(MediaAssetKey* keys, u32 capacity) noexcept;
bool application_image_viewer_key_current(MediaAssetKey key) noexcept;
// Scoped borrowed content for the foreground high-resolution request owner.
bool application_image_viewer_source(ApplicationSearchSourceView*) noexcept;
void application_image_viewer_close() noexcept;
// A request for the current viewer key failed for good (not a refresh).
void application_image_viewer_request_failed(MediaAssetKey key) noexcept;
// User dismissal: animates back toward the inline preview, then closes.
void application_image_viewer_dismiss() noexcept;
bool application_image_viewer_synchronize() noexcept;
bool application_image_viewer_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
}
