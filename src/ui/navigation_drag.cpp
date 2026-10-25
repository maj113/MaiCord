#include "maicord/ui/navigation_drag.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using Kind = NavigationDragRowKind;
using Drop = NavigationDropKind;

bool guild_source(Kind kind) noexcept {
    return kind == Kind::guild || kind == Kind::nested_guild;
}
bool channel_source(Kind kind) noexcept {
    return kind == Kind::text || kind == Kind::voice;
}
// The row a pointer is over, splitting the gaps between rows evenly; the
// first and last rows extend to the list ends.
u32 row_at(ConstSlice<NavigationDragRow> rows, float y, float gap) noexcept {
    if (!rows.data || !rows.count)
        return ~0u;
    for (u32 i = 0; i < rows.count; ++i) {
        const float bottom = i + 1 < rows.count ? (rows[i].rect.bottom + rows[i + 1].rect.top) * .5f
                                                : rows[i].rect.bottom + gap * .5f;
        if (y < bottom)
            return i;
    }
    return rows.count - 1;
}
UiRect line(const NavigationDragRow& row, Drop drop, float gap) noexcept {
    const float center = drop == Drop::before ? row.rect.top - gap * .5f : row.rect.bottom + gap * .5f;
    return {row.rect.left, center - kNavigationDragLine * .5f, row.rect.right, center + kNavigationDragLine * .5f};
}
NavigationDragTarget place(const NavigationDragRow& row, Drop drop, float gap) noexcept {
    return {row.id, row.kind, drop, drop == Drop::combine || drop == Drop::into ? row.rect : line(row, drop, gap)};
}
float fraction(const NavigationDragRow& row, float y) noexcept {
    const float height = ui_rect_height(row.rect);
    return height > 0 ? (y - row.rect.top) / height : .5f;
}
} // namespace

void navigation_drag_press(NavigationDrag* drag, NavigationDragList list, u64 source_id, NavigationDragRowKind kind,
                           UiPoint point) noexcept {
    if (!drag)
        return;
    reset_value(drag);
    const bool draggable = source_id && list != NavigationDragList::none &&
                           (list == NavigationDragList::guilds ? guild_source(kind) || kind == Kind::folder
                                                               : channel_source(kind) || kind == Kind::category);
    if (!draggable)
        return;
    drag->phase = NavigationDragPhase::armed;
    drag->list = list;
    drag->source_id = source_id;
    drag->source_kind = kind;
    drag->origin = drag->pointer = point;
}

bool navigation_drag_move(NavigationDrag* drag, UiPoint point) noexcept {
    if (!drag || drag->phase == NavigationDragPhase::idle)
        return false;
    drag->pointer = point;
    if (drag->phase != NavigationDragPhase::armed)
        return false;
    const float dx = point.x - drag->origin.x, dy = point.y - drag->origin.y;
    if (dx * dx + dy * dy < kNavigationDragThreshold * kNavigationDragThreshold)
        return false;
    drag->phase = NavigationDragPhase::dragging;
    reset_value(&drag->target);
    return true;
}

void navigation_drag_cancel(NavigationDrag* drag) noexcept {
    if (drag)
        reset_value(drag);
}

bool navigation_drag_release(NavigationDrag* drag, NavigationDragTarget* drop) noexcept {
    if (!drag)
        return false;
    const bool committed = drag->phase == NavigationDragPhase::dragging && drag->target.drop != Drop::none &&
                           drag->target.id;
    if (committed && drop)
        *drop = drag->target;
    reset_value(drag);
    return committed;
}

NavigationDragTarget navigation_drag_guild_target(const NavigationDrag& drag, ConstSlice<NavigationDragRow> rows,
                                                  float y, float gap) noexcept {
    if (drag.phase != NavigationDragPhase::dragging || drag.list != NavigationDragList::guilds)
        return {};
    const u32 index = row_at(rows, y, gap);
    if (index == ~0u)
        return {};
    const auto& row = rows[index];
    const bool source_folder = drag.source_kind == Kind::folder;
    if (row.kind == Kind::none || (row.id == drag.source_id && (row.kind == Kind::folder) == source_folder))
        return {};
    const float at = fraction(row, y);
    if (source_folder) {
        // Folders stay top level: nested guilds are not neighbours for them.
        if (row.kind == Kind::nested_guild)
            return {};
        return place(row, at < .5f ? Drop::before : Drop::after, gap);
    }
    const bool combinable = row.kind == Kind::guild || row.kind == Kind::folder;
    if (combinable && at >= .25f && at < .75f)
        return place(row, Drop::combine, gap);
    return place(row, at < .5f ? Drop::before : Drop::after, gap);
}

NavigationDragTarget navigation_drag_channel_target(const NavigationDrag& drag, ConstSlice<NavigationDragRow> rows,
                                                    float y) noexcept {
    if (drag.phase != NavigationDragPhase::dragging || drag.list != NavigationDragList::channels)
        return {};
    const u32 index = row_at(rows, y, 0);
    if (index == ~0u)
        return {};
    const auto& row = rows[index];
    if (row.id == drag.source_id || row.kind == Kind::none)
        return {};
    const float at = fraction(row, y);
    if (drag.source_kind == Kind::category) {
        if (row.kind != Kind::category)
            return {};
        return place(row, at < .5f ? Drop::before : Drop::after, 0);
    }
    if (row.kind == Kind::category)
        return place(row, Drop::into, 0);
    if (row.kind != drag.source_kind)
        return {};
    return place(row, at < .5f ? Drop::before : Drop::after, 0);
}

NavigationDragVisual navigation_drag_update_target(NavigationDrag* drag, const NavigationDragTarget& target) noexcept {
    if (!drag || drag->phase != NavigationDragPhase::dragging)
        return {};
    drag->target = target;
    const bool ring = target.drop == Drop::combine || target.drop == Drop::into;
    return {drag->source_id, target.drop != Drop::none ? target.indicator : UiRect{}, true, ring};
}

float navigation_drag_autoscroll(UiRect viewport, float y, u64 elapsed_ms) noexcept {
    const float height = ui_rect_height(viewport);
    if (height <= 0 || !elapsed_ms)
        return 0;
    const float band = height < kNavigationDragEdge * 3 ? height / 3 : kNavigationDragEdge;
    const float seconds = static_cast<float>(elapsed_ms > 100 ? 100 : elapsed_ms) / 1000.f;
    if (y < viewport.top + band) {
        const float depth = (viewport.top + band - y) / band;
        return -kNavigationDragScrollSpeed * (depth > 1 ? 1 : depth) * seconds;
    }
    if (y > viewport.bottom - band) {
        const float depth = (y - (viewport.bottom - band)) / band;
        return kNavigationDragScrollSpeed * (depth > 1 ? 1 : depth) * seconds;
    }
    return 0;
}

} // namespace maicord
