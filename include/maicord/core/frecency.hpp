#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// The account's frecency settings (settings-proto/2) keep one item per command, emoji or sticker: the
// FrecencyItem of the wire, with the newest ten uses.
inline constexpr u32 kFrecencyRecentUses = 10;
struct FrecencyItem {
    u64 recent_uses[kFrecencyRecentUses]; // unix ms, ascending, newest retained
    u32 total_uses;
    i32 frecency;
    i32 score;
    u8 recent_count;
};
// The sum of the client's weights for recent uses: each is 100 within 3 days, then 70, 50, 30 and 10 up to
// 15, 30, 45 and 80 days, else 1, by age in whole days.
__declspec(noinline) inline u32 frecency_recent_score(const u64* recent_uses, u32 count, u64 now_ms) noexcept {
    constexpr u64 day_ms = 24ull * 60ull * 60ull * 1'000ull;
    u32 score = 0;
    for (u32 use = 0; use < count; ++use) {
        const u64 days = now_ms > recent_uses[use] ? (now_ms - recent_uses[use]) / day_ms : 0;
        score += days <= 3 ? 100 : days <= 15 ? 70 : days <= 30 ? 50 : days <= 45 ? 30 : days <= 80 ? 10 : 1;
    }
    return score;
}
} // namespace maicord
