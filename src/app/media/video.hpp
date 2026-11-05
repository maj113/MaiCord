#pragma once
#include "maicord/media/core/asset_key.hpp"
#include "maicord/ui/voice/video_scene.hpp"
#include "maicord/ui/widgets/tooltip.hpp"
#include "maicord/ui/core/cursor.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/platform/window/event.hpp"

namespace maicord {
struct UiSearchTarget;
} // namespace maicord
namespace maicord::app_detail {
struct ApplicationVideo;
// What a pointer did over a player: it moved, the primary button went down or up, or the pointer left the window.
enum class VideoPointer : u8 { move, down, up, leave };
struct ApplicationSearchSourceTicket;
bool application_video_ordinary(const MediaAssetKey&) noexcept;
bool application_video_search_active() noexcept;
bool application_video_open_search(const UiSearchTarget&) noexcept;
// Existing message attachment focus routes audio activation through the same
// playback owner before generic file-open handling.
bool application_video_open_focused() noexcept;
bool application_video_pump(bool* repaint) noexcept;
// Reproject borrowed playback state after modal resize layout, without polling
// the account, taking frames or submitting playback/network commands.
bool application_video_synchronize() noexcept;
u64 application_video_next_wake() noexcept;
bool application_video_pointer(const WindowEvent& event) noexcept;
bool application_video_key(u32 key, bool shift, bool repeat) noexcept;
// The character of a key the player took as a shortcut is not text: true for the character event that follows it
// (and clears the mark at the next key release).
bool application_video_discard_character(const WindowEvent& event) noexcept;
bool application_video_tooltip_target(UiPoint point, bool keyboard, u64 scope,
                                      UiTooltipTarget* target) noexcept;
void application_video_cancel_pointer() noexcept;
void application_video_blur() noexcept;
void application_video_close() noexcept;
// Media viewer hosting: the one session moves between inline and viewer
// geometry; position and play state stay with the session.
void application_video_viewer_detach() noexcept;
bool application_video_viewer_pointer(VideoPointer kind, UiPoint point) noexcept;
bool application_video_viewer_hidden_cursor() noexcept;
// Viewer toolbar opacity: it hides with the idle player's controls.
float application_video_viewer_chrome() noexcept;
// Viewer input restarts the idle countdown of the hosted player.
void application_video_viewer_activity() noexcept;
// Inline media under the pointer: selected players and unselected cards.
bool application_video_cursor(UiPoint, UiCursor*) noexcept;
// Expanded viewer controls (or the idle hidden pointer); false elsewhere.
bool application_video_viewer_cursor(UiPoint, UiCursor*) noexcept;
// Reuses the current session when it already plays this item.
bool application_video_open_viewer_item(u64 message_id, u32 revision, bool embed, u8 index, bool snapshot,
                                        const ApplicationSearchSourceTicket* search, const UiRect& preview,
                                        MediaAssetKey* key) noexcept;
} // namespace maicord::app_detail
