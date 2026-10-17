#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Guild permission bitfield. The wire encoding is a decimal string of this
// 64-bit value; bit positions are fixed by the service.
using PermissionSet = u64;

inline constexpr PermissionSet kPermissionCreateInvite = 1ull << 0;
inline constexpr PermissionSet kPermissionKickMembers = 1ull << 1;
inline constexpr PermissionSet kPermissionBanMembers = 1ull << 2;
inline constexpr PermissionSet kPermissionAdministrator = 1ull << 3;
inline constexpr PermissionSet kPermissionManageChannels = 1ull << 4;
inline constexpr PermissionSet kPermissionManageGuild = 1ull << 5;
inline constexpr PermissionSet kPermissionAddReactions = 1ull << 6;
inline constexpr PermissionSet kPermissionViewAuditLog = 1ull << 7;
inline constexpr PermissionSet kPermissionPrioritySpeaker = 1ull << 8;
inline constexpr PermissionSet kPermissionStream = 1ull << 9;
inline constexpr PermissionSet kPermissionViewChannel = 1ull << 10;
inline constexpr PermissionSet kPermissionSendMessages = 1ull << 11;
inline constexpr PermissionSet kPermissionSendSpeechMessages = 1ull << 12;
inline constexpr PermissionSet kPermissionManageMessages = 1ull << 13;
inline constexpr PermissionSet kPermissionEmbedLinks = 1ull << 14;
inline constexpr PermissionSet kPermissionAttachFiles = 1ull << 15;
inline constexpr PermissionSet kPermissionReadMessageHistory = 1ull << 16;
inline constexpr PermissionSet kPermissionMentionEveryone = 1ull << 17;
inline constexpr PermissionSet kPermissionUseExternalEmojis = 1ull << 18;
inline constexpr PermissionSet kPermissionViewGuildInsights = 1ull << 19;
inline constexpr PermissionSet kPermissionConnect = 1ull << 20;
inline constexpr PermissionSet kPermissionSpeak = 1ull << 21;
inline constexpr PermissionSet kPermissionMuteMembers = 1ull << 22;
inline constexpr PermissionSet kPermissionDeafenMembers = 1ull << 23;
inline constexpr PermissionSet kPermissionMoveMembers = 1ull << 24;
inline constexpr PermissionSet kPermissionUseVoiceActivity = 1ull << 25;
inline constexpr PermissionSet kPermissionChangeNickname = 1ull << 26;
inline constexpr PermissionSet kPermissionManageNicknames = 1ull << 27;
inline constexpr PermissionSet kPermissionManageRoles = 1ull << 28;
inline constexpr PermissionSet kPermissionManageWebhooks = 1ull << 29;
inline constexpr PermissionSet kPermissionManageExpressions = 1ull << 30;
inline constexpr PermissionSet kPermissionUseApplicationCommands = 1ull << 31;
inline constexpr PermissionSet kPermissionRequestToSpeak = 1ull << 32;
inline constexpr PermissionSet kPermissionManageEvents = 1ull << 33;
inline constexpr PermissionSet kPermissionManageThreads = 1ull << 34;
inline constexpr PermissionSet kPermissionCreatePublicThreads = 1ull << 35;
inline constexpr PermissionSet kPermissionCreatePrivateThreads = 1ull << 36;
inline constexpr PermissionSet kPermissionUseExternalStickers = 1ull << 37;
inline constexpr PermissionSet kPermissionSendMessagesInThreads = 1ull << 38;
inline constexpr PermissionSet kPermissionUseActivities = 1ull << 39;
inline constexpr PermissionSet kPermissionModerateMembers = 1ull << 40;
inline constexpr PermissionSet kPermissionUseSoundboard = 1ull << 42;
inline constexpr PermissionSet kPermissionCreateExpressions = 1ull << 43;
inline constexpr PermissionSet kPermissionCreateEvents = 1ull << 44;
inline constexpr PermissionSet kPermissionUseExternalSounds = 1ull << 45;
inline constexpr PermissionSet kPermissionSendVoiceMessages = 1ull << 46;
inline constexpr PermissionSet kPermissionSetVoiceChannelStatus = 1ull << 48;
inline constexpr PermissionSet kPermissionSendPolls = 1ull << 49;
inline constexpr PermissionSet kPermissionUseExternalApps = 1ull << 50;
inline constexpr PermissionSet kPermissionPinMessages = 1ull << 51;
inline constexpr PermissionSet kPermissionBypassSlowmode = 1ull << 52;
inline constexpr PermissionSet kPermissionManageOfficialMessages = 1ull << 53;

inline constexpr PermissionSet kPermissionAll = ~PermissionSet{0};
// Every currently assigned bit; editors must preserve unknown bits they read.
inline constexpr PermissionSet kPermissionKnownMask = ((1ull << 54) - 1) & ~(1ull << 47);
// Stripped when the guild requires two-factor moderation and the acting
// account has not enabled it. Applies to the owner as well.
inline constexpr PermissionSet kPermissionElevatedMask =
    kPermissionKickMembers | kPermissionBanMembers | kPermissionAdministrator | kPermissionManageChannels |
    kPermissionManageGuild | kPermissionManageRoles | kPermissionManageMessages | kPermissionManageThreads |
    kPermissionModerateMembers;
// What a timed-out non-administrator member keeps in every channel.
inline constexpr PermissionSet kPermissionTimeoutAllowedMask =
    kPermissionViewChannel | kPermissionReadMessageHistory;
inline constexpr PermissionSet kPermissionQuarantineAllowedMask =
    kPermissionTimeoutAllowedMask | kPermissionChangeNickname;

constexpr bool permission_has(PermissionSet permissions, PermissionSet required) noexcept {
    return (permissions & kPermissionAdministrator) != 0 || (permissions & required) == required;
}

} // namespace maicord
