#pragma once
#include "maicord/core/base.hpp"

namespace maicord {

// v119: GatewayLocalPresence carries a plugin rich presence activity (type,
// details, state, end time) next to the detected game.
// v120: profile_edit and profile_image (User Settings > Profiles).
// v121: decoders (message JSON and channel routing) live only in the
// module; regional flags arrive as worker-compiled paint (frame kind 11).
// v122: cdn_urls (CDN URL parsing and target writers) live only in the module.
// v123: MemberPatch carries the timeout end in milliseconds; member roles change
// through guild_admin member_update (profile_get operations 5 and 6 are gone).
// v124: guild create/update records carry a details record after the banner;
// guild_admin drops guild_fetch, adds widget_get, widget_update and profile_get,
// and GuildSettingsPatch appends system_channel_flags, features and the widget.
// v125: guild_admin adds profile_update, vanity_get and vanity_update;
// GuildSettingsPatch appends the splash, progress bar, MD5 sources, server
// profile and vanity code; guild details carry the splash hash (header 120
// bytes); guild_moderation (members search, bulk ban, ban search) is
// appended; GUILD_BULK_BAN_UPDATE arrives as guild_bulk_ban_update.
// v126: message events carry the author's member.nick (member_nick_bytes,
// split from author_name_bytes) after the avatar hash in the payload text.
// v127: independently cancels the guild-member name query, including a
// server-rejected request retained for replay; author batches are unaffected.
// v128: Gateway descriptors identify quarantined self membership and thread
// creation without changing the interface layout; host and module must agree.
// v129: Guild and self-member Gateway descriptors distinguish omitted roles
// from known-empty roles; matching host/module semantics retain the same layout.
// v130: guild_admin accepts atomic member-role additions and removals without
// replacing the member's cached role list. The interface layout is unchanged.
// v131: Gateway descriptors carry channel activity cursors for passive/unread
// updates. Matching host/module semantics retain the same interface layout.
// v132: Gateway descriptors preserve account media eligibility, age status and
// parental policy. Matching host/module semantics retain the same layout.
// v133: Gateway descriptors invalidate guild command indexes. Matching
// host/module semantics retain the same layout.
// v134: channel_info (opcode 43) is appended; Gateway descriptors carry
// CHANNEL_INFO and the voice channel start time and status updates; guild
// sticker events carry the sticker array from READY, GUILD_CREATE and
// GUILD_STICKERS_UPDATE.
inline constexpr u32 kAccountModuleApiVersion = 134;
inline constexpr u16 kAccountModuleApiOrdinal = 134;

static_assert(kAccountModuleApiOrdinal == kAccountModuleApiVersion);

} // namespace maicord
