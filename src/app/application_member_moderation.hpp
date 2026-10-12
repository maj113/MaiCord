#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/ui/popup_scene.hpp"

namespace maicord::app_detail {

// Popup action range owned by member moderation (outside every other range).
inline constexpr u32 kMemberModerationActionBase = 0x4D440000u;
bool application_member_moderation_action(u32 action) noexcept;
// Appends the moderation section for this member to a context menu.
u32 application_member_moderation_menu_items(u64 guild_id, u64 user_id, Slice<PopupItem> output) noexcept;
// Handles moderation popup actions; the caller has not dismissed the popup.
bool application_member_moderation_activate(u32 action) noexcept;
// Starts one moderation action outside the context menu (Server Settings).
// action is a ModerationAction bit; timeouts and bans take their duration or
// message deletion window from the matching kModeration*Choices table.
bool application_member_moderation_start(u64 guild_id, u64 user_id, u8 action, u32 choice) noexcept;
bool application_member_moderation_pending(u64 now_ms) noexcept;
bool application_member_moderation_submit(u64 request_id, u64 now_ms) noexcept;
bool application_member_moderation_response(const RestClientResponse&, Utf8View body) noexcept;
u64 application_member_moderation_next_wake() noexcept;
void application_member_moderation_release() noexcept;

} // namespace maicord::app_detail
