#include "maicord/ui/list_navigation.hpp"

namespace maicord {

u32 ui_list_move(u32 count, u32 current, UiListMove move,
                 UiListEligible eligible, void* context) noexcept {
    if (!count || !eligible || move > UiListMove::last) return kUiListNoSelection;
    if (move == UiListMove::restore) {
        // Start one step before current so the shared bounded traversal tests
        // it first, then searches the remaining candidates exactly once.
        const u32 before = current < count ? (current ? current - 1 : count - 1) : kUiListNoSelection;
        return ui_list_next(count, before, false, true, eligible, context);
    }
    const bool reverse = move == UiListMove::previous || move == UiListMove::last;
    if (move == UiListMove::first || move == UiListMove::last) current = kUiListNoSelection;
    return ui_list_next(count, current, reverse, false, eligible, context);
}

u32 ui_list_next(u32 count, u32 current, bool reverse, bool wrap,
                 UiListEligible eligible, void* context) noexcept {
    if (!count || !eligible) return kUiListNoSelection;
    u32 candidate = current;
    for (u32 visited = 0; visited < count; ++visited) {
        if (candidate >= count) candidate = reverse ? count - 1 : 0;
        else if (reverse) {
            if (!candidate) { if (!wrap) return kUiListNoSelection; candidate = count - 1; }
            else --candidate;
        } else {
            if (candidate == count - 1) { if (!wrap) return kUiListNoSelection; candidate = 0; }
            else ++candidate;
        }
        if (eligible(context, candidate)) return candidate;
    }
    return kUiListNoSelection;
}

} // namespace maicord
