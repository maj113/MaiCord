#pragma once
#include "maicord/app/voice/go_live_session.hpp"
#include "maicord/platform/window/event.hpp"
#include "maicord/ui/core/geometry.hpp"
#include "maicord/ui/core/input_router.hpp"
#include "maicord/ui/voice/call_scene.hpp"

namespace maicord {
struct VideoStreamFrame;
}
namespace maicord::app_detail {
// Go Live sending on the client: the screen share dialog (MaiCord's source picker with live thumbnails, or the
// system's picker on Linux), the stream settings (Stream Mode, resolution and frame rate within the user's Nitro and
// the server's boosts, stream audio, preview), the stream menu while live (stop, change windows, quality and audio),
// the local user's own stream tile with its self-preview and viewers, and the errors that end a stream. The stream
// itself is the account module's GoLiveSession (AccountModuleApi::go_live*).

// The call bar's and the voice card's screen-share button for a call in channel_id.
UiCallShare application_go_live_share(u64 channel_id) noexcept;
// The screen-share button: the dialog, or the stream menu while live, anchored at `anchor`.
bool application_go_live_button(const UiRect& anchor) noexcept;

// The local user's stream tile in channel_id's call while starting or live: stream state, viewers, paused. The
// self-preview picture is call video under kGoLivePreviewSsrc (application_go_live_preview).
inline constexpr u32 kGoLivePreviewSsrc = 0x7fff'fff1u;
bool application_go_live_tile(u64 channel_id, UiCallParticipant* participant) noexcept;
bool application_go_live_preview(VideoStreamFrame* frame) noexcept;
// Your stream tile's viewer count: who is watching, anchored at `anchor`.
bool application_go_live_viewers_open(const UiRect& anchor) noexcept;
// "Stop Streaming".
void application_go_live_stop() noexcept;

// The dialog (a modal in the popup layer).
bool application_go_live_modal_active() noexcept;
void application_go_live_modal_close() noexcept;
bool application_go_live_modal_synchronize() noexcept;
bool application_go_live_modal_input(UiInputKind kind, u32 code, bool repeated) noexcept;
bool application_go_live_modal_window_event(const WindowEvent& event) noexcept;

// Its menus ride the stream menu's popup owner (application_stream_menu_action).
bool application_go_live_menu_action(u32 action) noexcept;
bool application_go_live_menu_activate(u32 action) noexcept;

// Session snapshot polling, thumbnail refreshes, error reports.
void application_go_live_pump() noexcept;
u64 application_go_live_next_wake() noexcept;
void application_go_live_release() noexcept;
} // namespace maicord::app_detail
