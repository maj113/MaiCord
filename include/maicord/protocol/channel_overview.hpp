#pragma once

#include "maicord/core/json_reader.hpp"

namespace maicord {
inline constexpr u32 kChannelOverviewNameCodepoints = 100;
inline constexpr u32 kChannelOverviewTopicCodepoints = 1'024;
inline constexpr u32 kChannelOverviewNameBytes = kChannelOverviewNameCodepoints * 4;
inline constexpr u32 kChannelOverviewTopicBytes = kChannelOverviewTopicCodepoints * 4;
inline constexpr u32 kChannelOverviewMaximumBodyBytes =
    (kChannelOverviewNameCodepoints + kChannelOverviewTopicCodepoints) * 6 + 128;
inline constexpr u32 kChannelOverviewMaximumSlowmodeSeconds = 21'600;
inline constexpr u64 kChannelOverviewRouteId = 0x43484F5645525657ull;
enum ChannelOverviewField : u8 {
    channel_overview_name = 1,
    channel_overview_topic = 2,
    channel_overview_slowmode = 4,
    channel_overview_nsfw = 8,
    channel_overview_all = 15
};
enum class ChannelOverviewOperation : u8 {
    fetch,
    update
};
enum class ChannelOverviewError : u8 {
    none,
    input,
    json,
    duplicate,
    identity,
    type,
    field,
    capacity
};
struct ChannelOverviewResult {
    u32 bytes{};
    ChannelOverviewError error{};
    u8 fields{};
    constexpr bool ok() const noexcept {
        return error == ChannelOverviewError::none;
    }
};
// Fully owned UTF-8, no borrowed response strings or NUL requirement.
struct ChannelOverviewSnapshot {
    u64 channel_id{}, guild_id{};
    u32 name_bytes{}, topic_bytes{}, slowmode_seconds{};
    u8 known_fields{}, type{};
    bool nsfw{};
    char name[kChannelOverviewNameBytes]{}, topic[kChannelOverviewTopicBytes]{};
};
struct ChannelOverviewPatch {
    Utf8View name{}, topic{};
    u32 slowmode_seconds{};
    u8 fields{};
    bool nsfw{};
};
Utf8View channel_overview_name_view(const ChannelOverviewSnapshot&) noexcept;
Utf8View channel_overview_topic_view(const ChannelOverviewSnapshot&) noexcept;
bool channel_overview_patch_valid(const ChannelOverviewPatch&) noexcept;
// Requires id/guild_id/type/name; absent optional fields take their defaults.
// Success fields reports actual wire presence. Failures preserve output.
ChannelOverviewResult channel_overview_decode(Utf8View, ChannelOverviewSnapshot* output) noexcept;
ChannelOverviewResult channel_overview_write_target(u64 channel_id, Slice<u8>) noexcept;
// Only selected fields are serialized. Output remains untouched on failure.
ChannelOverviewResult channel_overview_write_patch(const ChannelOverviewPatch&, Slice<u8>) noexcept;
static_assert(sizeof(ChannelOverviewSnapshot) == 4'528);
static_assert(sizeof(ChannelOverviewPatch) == 40);
} // namespace maicord
