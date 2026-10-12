#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/ui/geometry.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {
struct MessageActionBarHit;
struct PopupItem;
struct MessageTextHit;
}

namespace maicord::app_detail {
struct ApplicationPluginMenuExtra;

bool clear_message_text_selection(bool rebuild) noexcept;
// `double_click` is true for WM_LBUTTONDBLCLK; a following third press selects the paragraph.
bool begin_message_text_selection(LPARAM lparam, bool double_click = false) noexcept;
void end_message_text_selection() noexcept;
bool handle_message_text_selection_drag(WPARAM buttons, LPARAM lparam) noexcept;
bool copy_message_text_selection() noexcept;

bool dismiss_popup() noexcept;
bool refresh_popup_text() noexcept;
bool open_reaction_picker(u64 message_id) noexcept;
bool open_reaction_picker(u64 message_id, UiRect anchor) noexcept;
bool refresh_reaction_viewer_presentation() noexcept;
bool open_message_context_popup(u64 message_id, UiRect anchor) noexcept;
// The message menu with a right-clicked attachment's media and link groups
// (received_attachment_menu rows) before the developer-mode Copy ID. plugin
// names what the pointer was on for plugin rows (attachment, link, emoji).
bool open_message_media_context_popup(u64 message_id, UiRect anchor, ConstSlice<PopupItem> media,
                                      const ApplicationPluginMenuExtra* plugin = nullptr) noexcept;
// The message menu with a right-clicked link's Copy Link / Open Link group;
// false (nothing opened) when the hit is not a link with a resolvable URL.
bool open_message_link_context_popup(const MessageTextHit& hit, UiRect anchor) noexcept;
bool activate_message_link_action(u32 action) noexcept;
bool activate_popup_action(u32 action, bool alt_pressed, bool shift_pressed) noexcept;
bool open_context_popup(i32 client_x, i32 client_y) noexcept;
bool open_focused_context_popup() noexcept;
void focus_context_at(float x, float y) noexcept;
bool begin_message_reply(u64 message_id) noexcept;
// Hover bar authority (MessageActionBarCapability bits) and dispatch.
u16 application_message_action_bar_capabilities(u64 message_id) noexcept;
bool application_message_action_bar_activate(const MessageActionBarHit& hit, bool shift) noexcept;
// Shift press/release re-expands or collapses the open bar without pointer motion.
bool application_message_action_bar_modifiers_changed() noexcept;
bool handle_reaction_viewer_hover(float pointer_x, float pointer_y) noexcept;
bool handle_reaction_viewer_click(float pointer_x, float pointer_y) noexcept;
bool handle_reaction_viewer_key(WPARAM key) noexcept;
bool handle_popup_hover(LPARAM lparam) noexcept;
bool handle_popup_click(WPARAM wparam, LPARAM lparam) noexcept;
bool handle_popup_key(WPARAM key) noexcept;

} // namespace maicord::app_detail
