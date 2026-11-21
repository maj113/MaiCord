#pragma once
#include "maicord/core/base.hpp"

namespace maicord::app_detail {
// Channels & Roles: the official client's page of a community server (/channels/{g}/customize-community) with its
// Customize and Browse Channels tabs, a content page (application_content_page) in place of the chat column and the
// member list, opened from its row in the channel list or the server menu. Customize shows the onboarding questions
// (GET /guilds/{g}/onboarding at each opening) as answer tiles and My Profile; a change saves a second after the last
// one, on leaving the tab and when the page goes (PUT /guilds/{g}/onboarding-responses). Browse Channels follows
// channels and categories (application_notification_controls_follow_channel). A server without questions shows
// Browse Channels alone and reads nothing. A server whose page was showing when another server was chosen shows it
// again when chosen again, until a channel of it is chosen (for the session).
bool application_guild_channels_roles_open(u64 guild_id) noexcept;
bool application_guild_channels_roles_remembered(u64 guild_id) noexcept;
void application_guild_channels_roles_forget(u64 guild_id) noexcept;
} // namespace maicord::app_detail
