#include "maicord/ui/list_navigation.hpp"
#include "maicord/ui/virtual_list.hpp"

namespace maicord {
namespace {
bool valid(UiListRowBounds row, u64 total) noexcept {
    return row.first < row.last && row.last <= total;
}
u32 seek(u32 count, u32 from, bool reverse, UiListEligible eligible, void* context) noexcept {
    if (eligible(context, from)) return from;
    const u32 found = ui_list_next(count, from, reverse, false, eligible, context);
    return found != kUiListNoSelection ? found : ui_list_next(count, from, !reverse, false, eligible, context);
}
} // namespace

UiListPageResult ui_list_page(u32 count, u32 current, u64 scroll_units, u64 viewport_units,
    u64 total_units, bool reverse, UiListBounds bounds, UiListEligible eligible, void* context) noexcept {
    if (!count || !viewport_units || !total_units || !bounds || !eligible) return {};
    const u64 limit = total_units > viewport_units ? total_units - viewport_units : 0;
    const u64 scroll = minimum(scroll_units, limit);
    const u64 end = scroll + minimum(viewport_units, total_units - scroll);
    u64 anchor = scroll;
    bool visible_focus = false;
    if (current < count) {
        const auto row = bounds(context, current);
        if (!valid(row, total_units)) return {};
        anchor = maximum(scroll, minimum(row.first, end - 1));
        visible_focus = row.last > scroll && row.first < end && eligible(context, current);
    }
    const u64 probe = reverse ? anchor - minimum(anchor, viewport_units) :
        anchor + minimum(viewport_units, total_units - 1 - anchor);
    u64 next_scroll = reverse ? scroll - minimum(scroll, viewport_units) :
        scroll + minimum(viewport_units, limit - scroll);
    u32 first = 0;
    u32 last = count;
    while (first < last) {
        const u32 middle = first + (last - first) / 2;
        const auto row = bounds(context, middle);
        if (!valid(row, total_units)) return {};
        if (row.last <= probe) first = middle + 1;
        else last = middle;
    }
    u32 candidate = minimum(first, count - 1);
    auto row = bounds(context, candidate);
    if (!valid(row, total_units)) return {};
    if (reverse && row.first > probe && candidate) --candidate;
    candidate = seek(count, candidate, reverse, eligible, context);
    if (candidate == kUiListNoSelection) return {};
    row = bounds(context, candidate);
    if (!valid(row, total_units)) return {};
    const u64 next_end = next_scroll + minimum(viewport_units, total_units - next_scroll);
    // A partially visible row is a valid paging destination. Revealing its
    // whole extent could move backwards after paging through oversized content.
    if ((row.last <= next_scroll || row.first >= next_end) &&
        !ui_scroll_reveal_units(row.first, row.last - row.first, total_units,
            next_scroll, viewport_units, &next_scroll)) return {};
    // At the last eligible row, trailing headers must not make PageDown move
    // backwards merely to fully reveal the already-visible focused row.
    if (visible_focus) next_scroll = reverse ? minimum(next_scroll, scroll) : maximum(next_scroll, scroll);
    return {next_scroll, candidate};
}
} // namespace maicord
