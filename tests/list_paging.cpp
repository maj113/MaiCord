#include "maicord/ui/list_navigation.hpp"

namespace maicord {
namespace {
struct Rows {
    UiListRowBounds rows[100];
    bool enabled[100];
    u32 queries;
};
UiListRowBounds bounds(void* context, u32 index) noexcept {
    auto& rows = *static_cast<Rows*>(context);
    ++rows.queries;
    return rows.rows[index];
}
bool eligible(void* context, u32 index) noexcept { return static_cast<Rows*>(context)->enabled[index]; }
} // namespace

void test_list_paging(void (*check)(bool, const char*) noexcept) noexcept {
    Rows source{};
    for (u32 i = 0; i < 100; ++i) {
        source.rows[i] = {i * 44ull, (i + 1) * 44ull};
        source.enabled[i] = true;
    }
    auto target = ui_list_page(100, 1, 0, 200, 4400, false, bounds, eligible, &source);
    check(target.ok() && target.index == 5 && target.scroll_units == 200 && source.queries < 12,
          "page uses viewport extent and binary logical geometry lookup");
    target = ui_list_page(100, target.index, target.scroll_units, 200, 4400, true, bounds, eligible, &source);
    check(target.ok() && target.index == 0 && target.scroll_units == 0, "reverse page restores beginning without wrapping");
    target = ui_list_page(100, 98, 4000, 200, 4400, false, bounds, eligible, &source);
    check(target.ok() && target.index == 99 && target.scroll_units == 4200,
          "page reaches final row even when remaining scroll is shorter than a page");
    target = ui_list_page(100, 99, 4200, 200, 4400, false, bounds, eligible, &source);
    check(target.ok() && target.index == 99 && target.scroll_units == 4200, "repeated bottom page remains stable");
    for (u32 i = 1; i < 100; ++i) source.enabled[i] = false;
    target = ui_list_page(100, 0, 20, 200, 4400, false, bounds, eligible, &source);
    check(target.ok() && target.index == 0 && target.scroll_units == 20,
          "trailing headers do not reverse page direction at last eligible partially visible row");
    source.enabled[0] = false;
    check(!ui_list_page(100, 0, 0, 200, 4400, false, bounds, eligible, &source).ok(),
          "all-ineligible pages have no target");
    source.rows[0] = {0, 1000};
    source.enabled[0] = true;
    for (u32 i = 1; i < 4; ++i) {
        source.rows[i] = {900 + i * 100ull, 1000 + i * 100ull};
        source.enabled[i] = true;
    }
    for (u32 page = 1; page <= 3; ++page) {
        target = ui_list_page(4, 0, (page - 1) * 300, 300, 1300, false, bounds, eligible, &source);
        check(target.ok() && target.index == 0 && target.scroll_units == page * 300,
              "oversized row keeps focus while successive pages expose its content");
    }
    target = ui_list_page(4, 0, 900, 300, 1300, false, bounds, eligible, &source);
    check(target.ok() && target.index == 3 && target.scroll_units == 1000, "page leaves oversized row after exposing its trailing content");
    target = ui_list_page(4, 3, 1000, 300, 1300, true, bounds, eligible, &source);
    check(target.ok() && target.index == 0 && target.scroll_units == 700, "reverse page enters oversized row without snapping to its start");

    const u64 distant = (1ull << 50) + 3;
    for (u32 i = 0; i < 100; ++i) {
        source.rows[i] = {distant + i * 44ull, distant + (i + 1) * 44ull};
        source.enabled[i] = true;
    }
    target = ui_list_page(100, 2, distant + 100, 200, distant + 4400, false, bounds, eligible, &source);
    check(target.ok() && target.index == 6 && target.scroll_units == distant + 300,
          "page preserves exact integer units in very long lists");
    source.rows[50] = {100, 99};
    check(!ui_list_page(100, 2, distant, 200, distant + 4400, false, bounds, eligible, &source).ok() &&
          !ui_list_page(100, 2, distant, 0, distant + 4400, false, bounds, eligible, &source).ok() &&
          !ui_list_page(0, 0, 0, 200, 0, false, bounds, eligible, &source).ok(),
          "page rejects malformed queried geometry and empty inputs");
    bool paging_invariants = true;
    for (u32 seed = 0; seed < 32; seed += 4) {
        u64 total = 0;
        for (u32 i = 0; i < 16; ++i) {
            const u64 extent = 1 + (seed * 7 + i * 53) % 400;
            source.rows[i] = {total, total + extent};
            source.enabled[i] = (i + seed) % 3 != 0;
            total += extent;
        }
        const u64 viewport = 1 + seed * 19;
        const u64 limit = total > viewport ? total - viewport : 0;
        for (u64 offset = 0; offset <= limit; offset += 137) {
            u32 current = kUiListNoSelection;
            for (u32 i = 0; i < 16; ++i)
                if (source.enabled[i] && source.rows[i].last > offset && source.rows[i].first < offset + viewport) { current = i; break; }
            if (current == kUiListNoSelection) continue;
            for (u32 direction = 0; direction < 2; ++direction) {
                const bool reverse = direction != 0;
                const auto page = ui_list_page(16, current, offset, viewport, total, reverse, bounds, eligible, &source);
                paging_invariants = paging_invariants && page.ok();
                if (!page.ok()) continue;
                const auto row = source.rows[page.index];
                paging_invariants = paging_invariants && source.enabled[page.index] && page.scroll_units <= limit &&
                    row.last > page.scroll_units && row.first < page.scroll_units + viewport &&
                    (reverse ? page.index <= current && page.scroll_units <= offset : page.index >= current && page.scroll_units >= offset);
            }
        }
    }
    check(paging_invariants, "mixed-height filtered pages preserve visibility direction and bounded scroll across viewport sizes");
}
} // namespace maicord
