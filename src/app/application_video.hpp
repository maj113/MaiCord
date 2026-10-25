#pragma once
#include "maicord/media/asset_key.hpp"
#include "maicord/ui/video_scene.hpp"
#include "maicord/ui/tooltip.hpp"
#include "maicord/ui/cursor.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {
struct UiSearchTarget;
} // namespace maicord
namespace maicord::app_detail {
struct ApplicationVideo;
struct ApplicationSearchSourceTicket;
bool application_video_ordinary(MediaAssetKey) noexcept;
bool application_video_search_active() noexcept;
bool application_video_open_search(UiSearchTarget) noexcept;
// Existing message attachment focus routes audio activation through the same
// playback owner before generic file-open handling.
bool application_video_open_focused() noexcept;
bool application_video_pump(bool* repaint) noexcept;
// Reproject borrowed playback state after modal resize layout, without polling
// the account, taking frames or submitting playback/network commands.
bool application_video_synchronize() noexcept;
u64 application_video_next_wake() noexcept;
bool application_video_pointer(UINT message, LPARAM) noexcept;
bool application_video_key(UINT key, bool shift, bool repeat) noexcept;
bool application_video_tooltip_target(UiPoint point, bool keyboard, u64 scope,
                                      UiTooltipTarget* target) noexcept;
void application_video_cancel_pointer() noexcept;
void application_video_blur() noexcept;
void application_video_close() noexcept;
// Media viewer hosting: the one session moves between inline and viewer
// geometry; position and play state stay with the session.
bool application_video_expanded() noexcept;
void application_video_viewer_detach() noexcept;
bool application_video_viewer_pointer(UINT message, UiPoint) noexcept;
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
                                        const ApplicationSearchSourceTicket* search, UiRect preview,
                                        MediaAssetKey* key) noexcept;
} // namespace maicord::app_detail
