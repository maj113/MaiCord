#pragma once

#include "../support/model_store_fixtures.hpp"

namespace maicord {
namespace showcase_probe_detail {

// 2026-10-18 13:00 UTC; every message is earlier the same day.
constexpr u64 kNowSeconds = 1'792'328'400ull;
constexpr u64 kSnowflakeEpochMs = 1'420'070'400'000ull;
constexpr u64 kGuild = 100, kChannel = 311, kSelf = 1, kBot = 30, kList = 1;
constexpr u64 kRoleMaintainers = 901, kRoleDesigners = 902, kRoleContributors = 903;
constexpr u32 kWidth = 1440, kHeight = 900;
constexpr u32 kMessages = 16, kUsers = 16, kMembers = 16;
constexpr u32 kGuilds = 8, kChannels = 24, kRows = 32;

constexpr u64 snowflake(u32 minutes_before_now, u32 serial = 0) noexcept {
    return ((kNowSeconds * 1000 - minutes_before_now * 60'000ull - kSnowflakeEpochMs) << 22) + serial;
}

void paint_landscape(u8* out, u32 out_w, u32 out_h) noexcept;

void paint_gallery(u8* out, u32 out_w, u32 out_h) noexcept;

void paint_avatar(u8* out, u32 s, u32 a, u32 b, u32 variant) noexcept;

void paint_logo(u8* out, u32 s) noexcept;

void paint_guild_icon(u8* out, u32 s, u32 a, u32 b, u32 shape) noexcept;

void paint_banner(u8* out, u32 w, u32 h) noexcept;

void paint_game_art(u8* out, u32 s) noexcept;

void paint_record(u8* out, u32 s) noexcept;

void paint_party(u8* out, u32 w, u32 h, u32 variant) noexcept;

} // namespace showcase_probe_detail

using namespace showcase_probe_detail;
} // namespace maicord
