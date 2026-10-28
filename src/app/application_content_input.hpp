#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/cursor.hpp"
#include "maicord/app/navigation_shortcuts.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {
struct MessageProfileHit;
} // namespace maicord

namespace maicord::app_detail {

// A link opened from outside message text (attachment, button, viewer):
// it is treated as a named link, so untrusted hosts show the warning.
bool application_open_content_url(Utf8View url) noexcept;
// A link in message text, as a click opens it (the context menu's Open Link).
bool application_open_content_link(const MessageTextHit& hit) noexcept;
void application_received_attachment_pump() noexcept;
bool application_received_attachment_activate_focused() noexcept;
// Opens a resident message's attachment (or forwarded snapshot attachment, with
// its origin channel) in the browser: refreshes an expired signed URL, then
// asks through the external-link confirmation.
bool application_received_attachment_open(u64 message_id, u64 attachment_id,
                                           u64 snapshot_origin_channel = 0) noexcept;
// Admit only a real, readable native-model channel. False means no navigation
// was accepted; absent metadata is hydrated by the Search navigation owner.
bool application_navigate_known_message(u64 channel_id, u64 message_id) noexcept;
bool select_content_guild(u64 guild) noexcept;
bool handle_navigation_click(LPARAM lparam) noexcept;
// The click a navigation press performs (selection, folder toggle, voice
// join), replayed by drag-and-drop on a release without a drag.
bool activate_navigation_press(LPARAM lparam) noexcept;
// Moves the selection for a registry navigation shortcut (Alt+arrows and friends).
bool handle_navigation_shortcut(NavigationShortcut shortcut) noexcept;
bool activate_focused_navigation() noexcept;
bool handle_message_action_bar_click(LPARAM lparam) noexcept;
bool handle_message_click(LPARAM lparam) noexcept;
bool handle_content_click(LPARAM lparam) noexcept;
// Reveals a spoiler or explicit-blurred preview under the pointer; true when
// the click was consumed by the reveal.
bool application_message_media_reveal_at(float x, float y) noexcept;
MessageProfileHit application_message_profile_hit_test(float x, float y) noexcept;
// A server tag chip on an author line (it opens the server tag popout).
bool application_message_guild_tag_hit(float x, float y) noexcept;
bool handle_content_hover(LPARAM lparam) noexcept;
// Message pane targets a click would activate: inline players, polls, image
// previews, replies, file cards and declared rich buttons. False defers to
// links, author names and selectable text.
bool application_message_content_cursor(float x, float y, UiCursor* cursor) noexcept;

} // namespace maicord::app_detail
