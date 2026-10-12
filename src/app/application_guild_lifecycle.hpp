#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/protocol/guild_admin.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/input_router.hpp"

namespace maicord::app_detail {
// Server lifecycle: Invite people (a modal that creates an invite link with
// expiry and use options), Create a server (name and optional icon), Join a
// server (an invite link form that hands over to the invite dialog), Leave
// server (a confirmation; owners get an explanation instead), Folder
// settings (name and colour) and a server's Privacy settings. Drag-and-drop channel moves are sent from here
// too. Requests use the DETAILS lane through application_channel_overview,
// which forwards to this owner while its modal is open.

bool application_guild_lifecycle_open_invite(u64 guild_id) noexcept;
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
bool application_guild_lifecycle_input(UiInputKind, u32, bool) noexcept;
struct SettingsLayer;
bool application_guild_lifecycle_window_message(UINT, WPARAM, LPARAM) noexcept;
bool application_guild_lifecycle_submit(u64 now) noexcept;
bool application_guild_lifecycle_owns_response(u64 request_id) noexcept;
bool application_guild_lifecycle_response(const RestClientResponse&, Utf8View) noexcept;
u64 application_guild_lifecycle_next_wake() noexcept;
u64 application_guild_lifecycle_caret_wake() noexcept;
void application_guild_lifecycle_tick(u64 now) noexcept;
bool application_guild_lifecycle_edit_focused() noexcept;
MessageComposer* application_guild_lifecycle_edit_model() noexcept;
TextInputTarget application_guild_lifecycle_edit_target() noexcept;
void application_guild_lifecycle_edit_cancel_native_group(TextInputTarget) noexcept;
UiRect application_guild_lifecycle_edit_bounds() noexcept;
bool application_guild_lifecycle_edit_changed(bool content) noexcept;
u32 application_guild_lifecycle_owner_bytes() noexcept;

// Invite options as the official client offers them; exposed for tests.
inline constexpr u32 kGuildInviteExpiryCount = 7;
inline constexpr u32 kGuildInviteExpirySeconds[kGuildInviteExpiryCount]{1'800,  3'600,   21'600, 43'200,
                                                                        86'400, 604'800, 0};
inline constexpr u32 kGuildInviteDefaultExpiry = 5; // 7 days
inline constexpr u32 kGuildInviteUsesCount = 7;
inline constexpr u32 kGuildInviteUses[kGuildInviteUsesCount]{0, 1, 5, 10, 25, 50, 100};
} // namespace maicord::app_detail
