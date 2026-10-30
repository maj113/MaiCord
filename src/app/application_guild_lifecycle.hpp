#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/protocol/guild_admin.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
// Server lifecycle: Invite people (a modal that creates an invite link with
// expiry and use options), Create a server (name and optional icon), Join a
// server (an invite link form that hands over to the invite dialog), Leave
// server (a confirmation; owners get an explanation instead), Folder
// settings (name and colour) and a server's Privacy settings. Drag-and-drop
// channel moves are sent from here too. Requests use the DETAILS lane through application_channel_overview,
// which forwards to this owner while its modal is open.

// channel_id: the channel to invite to when it allows invites (else the
// server's usual invite channel).
bool application_guild_lifecycle_open_invite(u64 guild_id, u64 channel_id = 0) noexcept;
bool application_guild_lifecycle_open_create() noexcept;
bool application_guild_lifecycle_open_join() noexcept;
bool application_guild_lifecycle_leave(u64 guild_id) noexcept;
bool application_guild_lifecycle_open_folder(u64 folder_id) noexcept;
// Privacy settings for one server (the official client's per-server privacy modal): DMs,
// message requests, activity status and activity joining, each a switch over
// the synced privacy.*_guild_ids lists.
bool application_guild_lifecycle_open_privacy(u64 guild_id) noexcept;
// Queues PATCH /guilds/{id}/channels; false while another move is unsent or
// in flight.
bool application_guild_lifecycle_reorder_channels(u64 guild_id, ConstSlice<GuildAdminPosition> positions) noexcept;
// Frame time: hands a joined invite to the invite dialog and opens a newly
// created server once its GUILD_CREATE arrives.
void application_guild_lifecycle_frame() noexcept;

// Settings-modal host contract (see application_channel_overview).
bool application_guild_lifecycle_active() noexcept;
void application_guild_lifecycle_close(bool restore_focus = true) noexcept;
void application_guild_lifecycle_release() noexcept;
bool application_guild_lifecycle_synchronize() noexcept;
bool application_guild_lifecycle_submit(u64 now) noexcept;
bool application_guild_lifecycle_owns_response(u64 request_id) noexcept;
bool application_guild_lifecycle_response(const RestClientResponse&, Utf8View) noexcept;
u64 application_guild_lifecycle_next_wake() noexcept;
u64 application_guild_lifecycle_caret_wake() noexcept;
struct SettingsLayer;
SettingsLayer* application_guild_lifecycle_settings_layer() noexcept;
} // namespace maicord::app_detail
