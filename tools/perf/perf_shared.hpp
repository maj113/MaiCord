#pragma once
// Control surface between MaiCordPerfProbe (the client built with
// MAICORD_PERF_PROBE) and MaiCordPerfAccount.dll (the synthetic account
// module it loads instead of MaiCordAccount.dll). Nothing here ships.
#include "maicord/core/base.hpp"

#include <Windows.h>

namespace maicord::perf {

inline constexpr wchar_t kPerfAccountModuleFileName[] = L"MaiCordPerfAccount.dll";

// The synthetic account shape. One large "Perf Lab" guild carries the
// scenario channels; the remaining guilds fill READY like a busy account.
inline constexpr u32 kPerfGuilds = 100;
inline constexpr u32 kPerfLabCategories = 6;
inline constexpr u32 kPerfLabTextPerCategory = 9;
inline constexpr u32 kPerfOtherTextChannels = 20;
inline constexpr u32 kPerfUsers = 2'000;
inline constexpr u32 kPerfFriends = 300;
inline constexpr u32 kPerfDirectChannels = 150;
inline constexpr u32 kPerfGroupChannels = 30;
inline constexpr u32 kPerfLabMembers = 48'000;
inline constexpr u32 kPerfLabOnline = 6'000;

inline constexpr u64 kPerfSelfUserId = 600'000'000'000'000'001ull;
inline constexpr u64 perf_guild_id(u32 guild) noexcept {
    return 700'000'000'000'000'000ull + static_cast<u64>(guild) * 10'000ull;
}
// Channel 0 of every guild is its first category; Perf Lab channels are
// laid out category, text x N, voice per category.
inline constexpr u64 perf_channel_id(u32 guild, u32 channel) noexcept {
    return perf_guild_id(guild) + 1 + channel;
}
inline constexpr u64 perf_user_id(u32 user) noexcept {
    return 800'000'000'000'000'000ull + static_cast<u64>(user);
}
inline constexpr u64 perf_role_id(u32 guild, u32 role) noexcept {
    return perf_guild_id(guild) + 5'000 + role;
}
inline constexpr u64 perf_emoji_id(u32 guild, u32 emoji) noexcept {
    return perf_guild_id(guild) + 7'000 + emoji;
}
inline constexpr u64 perf_private_id(u32 index) noexcept {
    return 900'000'000'000'000'000ull + static_cast<u64>(index) * 16ull;
}
inline constexpr u32 perf_lab_channel_count() noexcept {
    return kPerfLabCategories * (kPerfLabTextPerCategory + 2);
}
// Text channel `index` (0-based over all Perf Lab text channels).
inline constexpr u64 perf_lab_text_channel(u32 index) noexcept {
    const u32 category = index / kPerfLabTextPerCategory;
    const u32 slot = index % kPerfLabTextPerCategory;
    return perf_channel_id(0, category * (kPerfLabTextPerCategory + 2) + 1 + slot);
}

enum class PerfAccountCommand : u32 {
    // Typing starts and presence changes in the selected guild/channel at
    // the given per-second rates until `stop_churn`; argument1 = channel.
    start_churn = 1,
    stop_churn = 2,
    // One new message in argument1's channel (mixed content).
    message_create = 3,
};

struct PerfAccountControl {
    PerfAccountCommand command;
    u32 typing_per_second;
    u32 presence_per_second;
    u32 messages_per_second;
    u64 channel_id;
    u64 guild_id;
};

struct PerfAccountCounters {
    u64 gateway_events;
    u64 gateway_payload_bytes;
    u64 rest_responses;
    u64 history_messages;
    u64 media_frames;
    u64 media_pixel_bytes;
    u64 stub_calls;
    u64 module_reserved_bytes;
    u64 module_committed_bytes;
    u64 module_base;
    u32 ready_events;
    u32 ready_json_bytes;
    u32 last_stub_slot;
    u32 member_list_syncs;
};

using PerfAccountControlFunction = bool(WINAPI*)(const PerfAccountControl*) noexcept;
using PerfAccountCountersFunction = bool(WINAPI*)(PerfAccountCounters*) noexcept;
inline constexpr char kPerfAccountControlExport[] = "maicord_perf_account_control";
inline constexpr char kPerfAccountCountersExport[] = "maicord_perf_account_counters";

} // namespace maicord::perf
