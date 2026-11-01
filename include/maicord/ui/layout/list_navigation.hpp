#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

inline constexpr u32 kUiListNoSelection = ~0u;
using UiListEligible = bool (*)(void*, u32) noexcept;
enum class UiListMove : u8 { restore, previous, next, first, last };

struct UiListRowBounds { u64 first; u64 last; };
using UiListBounds = UiListRowBounds (*)(void*, u32) noexcept;
struct UiListPageResult {
    u64 scroll_units = 0;
    u32 index = kUiListNoSelection;
    constexpr bool ok() const noexcept { return index != kUiListNoSelection; }
};

// Ordered, non-overlapping logical bounds in caller-defined integer units.
// Plans one viewport of movement, preserving visible focus position where
// possible. Oversized rows can keep focus while their content pages. Callbacks
// must not mutate the logical source; they may memoize query data in context.
// The source must stay stable for the duration of a call.
UiListPageResult ui_list_page(u32 count, u32 current, u64 scroll_units, u64 viewport_units,
    u64 total_units, bool reverse, UiListBounds bounds, UiListEligible eligible, void* context) noexcept;

// Non-wrapping row movement. Restore keeps an eligible current row, otherwise
// finds the next eligible row from that position, then searches from the start.
u32 ui_list_move(u32 count, u32 current, UiListMove move,
                 UiListEligible eligible, void* context) noexcept;

// Traverses logical rows, independently of widget materialization. A stale
// current index starts at the first/last row; scans at most count candidates.
u32 ui_list_next(u32 count, u32 current, bool reverse, bool wrap,
                 UiListEligible eligible, void* context) noexcept;

} // namespace maicord
