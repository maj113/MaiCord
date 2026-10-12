#include "maicord/ui/virtual_list.hpp"

namespace maicord {
bool ui_scroll_reveal_units(u64 first, u64 extent, u64 total, u64 scroll_units,
                            u64 viewport_units, u64* result) noexcept {
    if (!result || !viewport_units || !extent || first > total || extent > total - first) return false;
    const u64 last = first + extent;
    const u64 limit = total > viewport_units ? total - viewport_units : 0;
    u64 scroll = minimum(scroll_units, limit);
    const u64 end = scroll + minimum(viewport_units, total - scroll);
    if (!(first < scroll && last > end)) {
        if (first < scroll) scroll = extent <= viewport_units ? first : last - viewport_units;
        else if (last > end) scroll = extent <= viewport_units ? last - viewport_units : first;
    }
    *result = minimum(scroll, limit);
    return true;
}

bool ui_variable_list_reveal(const UiVariableList& list, u32 index, u64 scroll_units,
                             u64 viewport_units, u64* result) noexcept {
    if (!list.item_units || !list.fenwick || list.count > list.capacity || index >= list.count) return false;
    return ui_scroll_reveal_units(ui_variable_list_prefix_units(list, index), list.item_units[index],
        list.total_units, scroll_units, viewport_units, result);
}

namespace {

constexpr bool valid_item_units(u32 units) noexcept {
    return units != 0 && units <= kMaximumUiItemExtentUnits;
}

constexpr u16 next_generation(u16 generation) noexcept {
    ++generation;
    return generation == 0 ? 1 : generation;
}

void rebuild_fenwick(UiVariableList* list) noexcept {
    list->total_units = 0;
    for (u32 index = 0; index < list->count; ++index) {
        list->fenwick[index] = list->item_units[index];
        list->total_units += list->item_units[index];
    }
    for (u32 index = 0; index < list->count; ++index) {
        const u32 parent = index | (index + 1);
        if (parent < list->count) {
            list->fenwick[parent] += list->fenwick[index];
        }
    }
}

u32 lower_bound_item(const UiVariableList& list, u64 offset_units) noexcept {
    if (list.count == 0 || offset_units >= list.total_units) {
        return list.count;
    }
    u32 step = 1;
    while (step <= list.count / 2) {
        step <<= 1;
    }
    u32 index = 0;
    u64 prefix = 0;
    while (step != 0) {
        const u32 next = index + step;
        if (next <= list.count && prefix + list.fenwick[next - 1] <= offset_units) {
            index = next;
            prefix += list.fenwick[next - 1];
        }
        step >>= 1;
    }
    return index;
}

void update_anchor_insert(UiScrollAnchor* anchor, u16 old_generation,
                          u16 new_generation, u32 index, u32 item_count) noexcept {
    if (!anchor) {
        return;
    }
    if (!anchor->valid || anchor->generation != old_generation) {
        *anchor = UiScrollAnchor{};
        return;
    }
    if (anchor->index >= index) {
        anchor->index += item_count;
    }
    anchor->generation = new_generation;
}

void update_anchor_remove(UiScrollAnchor* anchor, u16 old_generation,
                          u16 new_generation, u32 index, u32 item_count) noexcept {
    if (!anchor) {
        return;
    }
    if (!anchor->valid || anchor->generation != old_generation) {
        *anchor = UiScrollAnchor{};
        return;
    }
    const u32 end = index + item_count;
    if (anchor->index >= index && anchor->index < end) {
        *anchor = UiScrollAnchor{};
        return;
    }
    if (anchor->index >= end) {
        anchor->index -= item_count;
    }
    anchor->generation = new_generation;
}

} // namespace

u32 ui_extent_units(float dips) noexcept {
    if (dips != dips || dips < 0.0f || dips > 1'048'576.0f) {
        return 0;
    }
    return static_cast<u32>(dips * static_cast<float>(kUiExtentUnitsPerDip) + 0.5f);
}

float ui_extent_dips(u64 units) noexcept {
    return static_cast<float>(units) / static_cast<float>(kUiExtentUnitsPerDip);
}

float ui_signed_extent_dips(i64 units) noexcept {
    return static_cast<float>(units) / static_cast<float>(kUiExtentUnitsPerDip);
}

bool ui_variable_list_initialize(UiVariableList* list, Slice<u32> item_units,
                                 Slice<u64> fenwick, u32 item_count,
                                 u32 default_item_units) noexcept {
    if (!list || !item_units.data || item_units.count == 0 || !fenwick.data ||
        fenwick.count != item_units.count || item_count > item_units.count ||
        item_units.count > kMaximumUiVariableItems ||
        !valid_item_units(default_item_units)) {
        return false;
    }
    *list = UiVariableList{};
    list->item_units = item_units.data;
    list->fenwick = fenwick.data;
    list->count = item_count;
    list->capacity = item_units.count;
    list->generation = 1;
    for (u32 index = 0; index < item_count; ++index) {
        list->item_units[index] = default_item_units;
    }
    rebuild_fenwick(list);
    return true;
}

u32 ui_variable_list_item_units(const UiVariableList& list, u32 index) noexcept {
    return list.item_units && index < list.count ? list.item_units[index] : 0;
}

u64 ui_variable_list_prefix_units(const UiVariableList& list,
                                  u32 exclusive_index) noexcept {
    if (!list.fenwick) {
        return 0;
    }
    u32 index = minimum(exclusive_index, list.count);
    u64 result = 0;
    while (index != 0) {
        result += list.fenwick[index - 1];
        index -= index & (~index + 1);
    }
    return result;
}

bool ui_variable_list_set_height(UiVariableList* list, u32 index,
                                 u32 item_units) noexcept {
    if (!list || !list->item_units || !list->fenwick || index >= list->count ||
        !valid_item_units(item_units)) {
        if (list) {
            ++list->rejected;
        }
        return false;
    }
    const u32 previous = list->item_units[index];
    if (previous == item_units) {
        return true;
    }
    list->item_units[index] = item_units;
    if (item_units > previous) {
        const u64 increase = static_cast<u64>(item_units - previous);
        list->total_units += increase;
        for (u32 position = index + 1; position <= list->count;
             position += position & (~position + 1)) {
            list->fenwick[position - 1] += increase;
        }
    } else {
        const u64 decrease = static_cast<u64>(previous - item_units);
        list->total_units -= decrease;
        for (u32 position = index + 1; position <= list->count;
             position += position & (~position + 1)) {
            list->fenwick[position - 1] -= decrease;
        }
    }
    ++list->height_updates;
    return true;
}

bool ui_variable_list_capture_anchor(const UiVariableList& list, u64 scroll_units,
                                     UiScrollAnchor* anchor) noexcept {
    if (!anchor || !list.item_units || !list.fenwick || list.count == 0 ||
        list.total_units == 0) {
        if (anchor) {
            *anchor = UiScrollAnchor{};
        }
        return false;
    }
    scroll_units = minimum(scroll_units, list.total_units - 1);
    const u32 index = lower_bound_item(list, scroll_units);
    const u64 prefix = ui_variable_list_prefix_units(list, index);
    *anchor = UiScrollAnchor{index, static_cast<u32>(scroll_units - prefix),
                             list.generation, true};
    return true;
}

bool ui_variable_list_restore_anchor(const UiVariableList& list,
                                     const UiScrollAnchor& anchor,
                                     u64* scroll_units) noexcept {
    if (!scroll_units || !anchor.valid || anchor.generation != list.generation ||
        anchor.index >= list.count || !list.item_units || !list.fenwick) {
        return false;
    }
    const u32 height = list.item_units[anchor.index];
    const u32 within = minimum(anchor.within_item_units, height - 1);
    *scroll_units = ui_variable_list_prefix_units(list, anchor.index) + within;
    return true;
}

UiVariableRange ui_variable_list_buffered_range(const UiVariableList& list, u64 scroll_units,
                                                u64 viewport_units, u64 margin_units) noexcept {
    if (!list.total_units || !viewport_units) return {};
    scroll_units = minimum(scroll_units, list.total_units - 1);
    const u64 start = scroll_units - minimum(scroll_units, margin_units);
    const u64 before = scroll_units - start;
    const u64 extent = minimum(viewport_units, list.total_units) + before +
                       minimum(margin_units, list.total_units);
    UiVariableRange range = ui_variable_list_range(list, start, extent, 0);
    range.first_offset_units -= static_cast<i64>(before);
    return range;
}

UiVariableRange ui_variable_list_range(const UiVariableList& list, u64 scroll_units,
                                       u64 viewport_units, u32 overscan) noexcept {
    if (!list.item_units || !list.fenwick || list.count == 0 || list.total_units == 0 ||
        viewport_units == 0) {
        return UiVariableRange{};
    }
    scroll_units = minimum(scroll_units, list.total_units - 1);
    const u64 remaining = list.total_units - 1 - scroll_units;
    const u64 end_offset = scroll_units + minimum(viewport_units - 1, remaining);
    const u32 first_visible = lower_bound_item(list, scroll_units);
    const u32 last_visible = lower_bound_item(list, end_offset);
    const u32 first = first_visible > overscan ? first_visible - overscan : 0;
    const u32 visible_end = last_visible + 1;
    const u32 end = visible_end > list.count - minimum(list.count, overscan)
                        ? list.count
                        : visible_end + overscan;
    const u64 first_prefix = ui_variable_list_prefix_units(list, first);
    return UiVariableRange{first, end - first,
                           static_cast<i64>(first_prefix) - static_cast<i64>(scroll_units)};
}

bool ui_variable_list_prepend(UiVariableList* list, u32 item_count,
                              u32 default_item_units, UiScrollAnchor* anchor) noexcept {
    return ui_variable_list_insert(list, 0, item_count, default_item_units, anchor);
}

bool ui_variable_list_remove_prefix(UiVariableList* list, u32 item_count,
                                    UiScrollAnchor* anchor) noexcept {
    return ui_variable_list_remove(list, 0, item_count, anchor);
}

bool ui_variable_list_insert(UiVariableList* list, u32 index, u32 item_count,
                             u32 default_item_units, UiScrollAnchor* anchor) noexcept {
    if (!list || !list->item_units || !list->fenwick || index > list->count ||
        item_count == 0 || item_count > list->capacity - list->count ||
        !valid_item_units(default_item_units)) {
        if (list) {
            ++list->rejected;
        }
        return false;
    }
    const u16 previous_generation = list->generation;
    for (u32 source = list->count; source > index; --source) {
        list->item_units[source - 1 + item_count] = list->item_units[source - 1];
    }
    for (u32 inserted = 0; inserted < item_count; ++inserted) {
        list->item_units[index + inserted] = default_item_units;
    }
    list->count += item_count;
    list->generation = next_generation(list->generation);
    update_anchor_insert(anchor, previous_generation, list->generation, index, item_count);
    rebuild_fenwick(list);
    ++list->structural_updates;
    return true;
}

bool ui_variable_list_remove(UiVariableList* list, u32 index, u32 item_count,
                             UiScrollAnchor* anchor) noexcept {
    if (!list || !list->item_units || !list->fenwick || index >= list->count ||
        item_count == 0 || item_count > list->count - index) {
        if (list) {
            ++list->rejected;
        }
        return false;
    }
    const u16 previous_generation = list->generation;
    const u32 end = index + item_count;
    for (u32 source = end; source < list->count; ++source) {
        list->item_units[source - item_count] = list->item_units[source];
    }
    list->count -= item_count;
    list->generation = next_generation(list->generation);
    update_anchor_remove(anchor, previous_generation, list->generation, index, item_count);
    rebuild_fenwick(list);
    ++list->structural_updates;
    return true;
}

} // namespace maicord
