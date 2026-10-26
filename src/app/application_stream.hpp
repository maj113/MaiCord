#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/app/stream_watch_view.hpp"
#include "maicord/media/asset_key.hpp"
#include "maicord/ui/geometry.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {
struct UiStreamCardScene;
}

namespace maicord::app_detail {
// Go Live watching on the client: the single watched stream (one at a time, as
// in the official client; picking another switches without asking), its call-view projection,
// stream volume, preview thumbnails (REST + media, cached 120 s with backoff)
// and the preview card shown when hovering a live user under a voice channel.

// Watches key: inside its voice channel at once, otherwise joins the channel
// first and watches once the call is there. Opens the call view on the stream.
bool application_stream_watch(const GatewayStreamKey& key) noexcept;
// Stops watching (also clears an ended stream's tile).
bool application_stream_stop() noexcept;
StreamWatchView application_stream_view() noexcept;
// The call-view tile ID of owner's stream (never a user ID).
u64 application_stream_tile(u64 owner_id) noexcept;
// Marks key's preview as shown now (fetching it when due) and returns its
// image key once a URL is known; {} otherwise. loading reports a first fetch.
MediaAssetKey application_stream_preview(const GatewayStreamKey& key, bool* loading = nullptr) noexcept;
bool application_stream_set_volume(u32 percent) noexcept;

// Stream tile menu: volume slider (0-200 %), full screen, stop watching.
bool application_stream_menu_open(u64 tile_id, const UiRect& anchor, bool fullscreen) noexcept;
bool application_stream_menu_action(u32 action) noexcept;
bool application_stream_menu_activate(u32 action) noexcept;
bool application_stream_menu_pointer(float x, float y, bool press) noexcept;

// Snapshot polling, pending watches, hover-card timing.
void application_stream_pump() noexcept;
u64 application_stream_next_wake() noexcept;
// Voice channel rows: hover card, LIVE badge clicks. True when consumed.
bool application_stream_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
// Paint-only spinner phase for the card (host frame clock); null when hidden.
UiStreamCardScene* application_stream_card_spinner_scene() noexcept;

// Preview REST lane (details) and media.
bool application_stream_preview_pending(u64 now_ms) noexcept;
bool application_stream_preview_submit(u64 request_id, u64 now_ms) noexcept;
bool application_stream_preview_response(const RestClientResponse& response, Utf8View body) noexcept;
u64 application_stream_preview_next_wake() noexcept;
bool application_stream_preview_key_current(const MediaAssetKey& key) noexcept;
struct StreamPreviewMedia {
    Utf8View url; // Borrowed until the next stream call; never log it.
    MediaAssetKey key;
};
// Previews shown now whose images should be resident.
u32 application_stream_preview_media(StreamPreviewMedia* output, u32 capacity) noexcept;

void application_stream_release() noexcept;
} // namespace maicord::app_detail
