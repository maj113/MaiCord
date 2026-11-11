#pragma once
#include "maicord/media/core/asset_key.hpp"
#include "maicord/platform/window/event.hpp"
#include "maicord/ui/media/image_viewer.hpp"

namespace maicord::app_detail {
struct ApplicationMediaPump;

// Media pages: an attachment or embed opened as its own page (a title bar media tab), shown in the content column
// with the sidebar and member list beside it. A page owns a copy of what it shows ({url, key}, title, source line,
// the message it came from), so it outlives its message, channel and the message store; only Jump to message needs
// them. Pages are session-only (signed URLs expire): sign out and account switches reset them. One page shows at a
// time, in one viewer in page mode; an image page keeps its zoom and pan while hidden and holds no picture.
enum class MediaPageKind : u8 { image, video, audio };
enum MediaPageFlag : u8 {
    media_page_embed = 1,
    media_page_snapshot = 2,
    // An embed playlist: the page reserves its queue column.
    media_page_playlist = 4,
    // A provider embed (YouTube and the like): played at up to 1080p.
    media_page_provider = 8,
};
struct MediaPageSource {
    MediaPageKind kind;
    u8 flags;
    // url: the original CDN URL (the media proxy URL of an embed image); title: file name or embed title; source:
    // "#channel · Server · author".
    Utf8View url, title, source;
    // An audio page's artwork is `fallback` (its key; zero without artwork, `pixels` its size) and `art` (its URL,
    // an embed's thumbnail). The key of an audio page is its audio's.
    MediaAssetKey key, fallback;
    UiSize pixels;
    u64 channel_id, message_id, author_id;
    u32 revision;
    Utf8View art;
    // What Open original opens: an embed's own link (the page it describes); empty for an attachment, whose URL it is.
    Utf8View link;
};
inline constexpr u32 kMediaPageCapacity = 8;

// Copies the source into a new page and returns its id (1 to kMediaPageCapacity), or the id of the page already
// showing the same media; 0 when every page is taken or the source is invalid.
u8 application_media_page_open(const MediaPageSource& source) noexcept;
// Shows the page in the content column (closing a message viewer first); false for an unknown id.
bool application_media_page_show(u8 id) noexcept;
void application_media_page_hide() noexcept;
// Hides the page first when it shows.
void application_media_page_close(u8 id) noexcept;
// Closes every page (sign out, account switch).
void application_media_page_reset() noexcept;
// The page in the content column: 0 when none shows, also while a message viewer covers it.
u8 application_media_page_shown() noexcept;
MediaPageKind application_media_page_kind(u8 id) noexcept;
Utf8View application_media_page_title(u8 id) noexcept;
// The line under the title: "#channel · Server · author" as it was when the page opened.
Utf8View application_media_page_source(u8 id) noexcept;
// The page's media cannot load ("Media unavailable") until Retry.
bool application_media_page_missing(u8 id) noexcept;
// The content column the page fills.
UiRect application_media_page_rect() noexcept;
// The shown page's viewer (accessibility, probes), null when none shows.
const UiImageViewer* application_media_page_viewer() noexcept;

// The player (video.cpp hosts the one session in a page): what a video or audio page plays from, the resume point
// a replaced session left, the playlist track it was on and whether it ever played.
struct MediaPagePlayback {
    Utf8View url, title;
    MediaAssetKey key;
    UiSize pixels;
    u64 channel_id, message_id;
    u32 revision, resume_ms;
    u16 track;
    u8 flags;
    bool played;
};
bool application_media_page_playback(u8 id, MediaPagePlayback* out) noexcept;
// Where the shown page's player goes (its stage) and the owner of its controls; false while the page is hidden,
// covered or too small.
bool application_media_page_player(u8 id, UiRect* stage, UiWidgetReference* owner) noexcept;
// The page's session ended or was replaced: the page resumes there (`track` 0 outside a playlist).
void application_media_page_note_playback(u8 id, u32 position_ms, u16 track) noexcept;
// The shown page fills the window as all player (the window goes full screen with it).
bool application_media_page_fullscreen() noexcept;
void application_media_page_set_fullscreen(bool fullscreen) noexcept;

// Frame and input plumbing.
bool application_media_page_synchronize() noexcept;
bool application_media_page_window_event(const WindowEvent& event) noexcept;
// The shown page's viewer while its loading spinner turns.
UiImageViewer* application_media_page_spinner() noexcept;
// The media pump: requests the shown image page's picture through its own {url, key} (signed URLs refreshed by
// media_url_resolve). A refresh that fails, or a request that cannot load, marks the page unavailable, and it is
// not asked again until Retry.
bool application_media_page_pump(ApplicationMediaPump& pump, bool* stop) noexcept;
bool application_media_page_key_current(const MediaAssetKey& key) noexcept;
} // namespace maicord::app_detail
