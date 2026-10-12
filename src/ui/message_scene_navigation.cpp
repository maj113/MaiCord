#include "maicord/ui/message_scene.hpp"

namespace maicord {
namespace {
bool eligible(void* context, u32 index) noexcept {
    const auto& store = *static_cast<const MessageTimeline*>(context)->store;
    return message_store_resolve(store, message_store_ordered_handle(store, index)) != nullptr;
}
UiListRowBounds bounds(void* context, u32 index) noexcept {
    const auto& list = *static_cast<const MessageTimeline*>(context)->list;
    const u64 first = ui_variable_list_prefix_units(list, index);
    return {first, first + list.item_units[index]};
}
} // namespace

bool message_scene_reveal_message(MessageScene* scene, u64 message_id) noexcept {
    if (!scene || !scene->active || !scene->timeline || !scene->timeline->store ||
        !scene->timeline->list || !message_id) return false;
    auto& timeline = *scene->timeline;
    if (timeline.list->count != timeline.store->count) return false;
    const auto found = message_store_find(*timeline.store, message_id);
    u64 scroll = 0;
    if (!found.ok() || !ui_variable_list_reveal(*timeline.list, found.order_index,
        timeline.scroll_units, timeline.viewport_units, &scroll)) return false;
    // Explicit focus must stay on this row when newer messages arrive.
    return message_timeline_set_scroll(&timeline, scroll, false) &&
        message_scene_update(scene, scene->viewport).ok() && message_scene_focus_message(scene, message_id);
}

MessageSceneHit message_scene_navigate(MessageScene* scene, UiListMove move) noexcept {
    if (!scene || !scene->active || !scene->timeline || !scene->timeline->store ||
        !scene->timeline->list || !scene->timeline->store->count ||
        scene->timeline->list->count != scene->timeline->store->count) return {};
    auto& timeline = *scene->timeline;
    const auto found = message_store_find(*timeline.store, scene->focused_message_id);
    u32 current = found.ok() ? found.order_index : kUiListNoSelection;
    if (current == kUiListNoSelection && move == UiListMove::restore)
        current = ui_variable_list_range(*timeline.list, timeline.scroll_units, timeline.viewport_units, 0).first;
    const u32 index = ui_list_move(timeline.store->count, current, move, eligible, &timeline);
    if (index == kUiListNoSelection) return {};
    const auto* record = message_store_resolve(*timeline.store, message_store_ordered_handle(*timeline.store, index));
    const u64 id = record ? record->id : 0;
    if (!id || !message_scene_reveal_message(scene, id)) return {};
    return {id, scene->widgets.focused, index};
}

MessageSceneHit message_scene_page(MessageScene* scene, bool reverse) noexcept {
    if (!scene || !scene->active || !scene->timeline || !scene->timeline->store ||
        !scene->timeline->list || !scene->timeline->list->item_units || !scene->timeline->list->fenwick ||
        scene->timeline->list->count > scene->timeline->list->capacity ||
        scene->timeline->list->count != scene->timeline->store->count) return {};
    auto& timeline = *scene->timeline;
    const auto found = message_store_find(*timeline.store, scene->focused_message_id);
    const auto target = ui_list_page(timeline.store->count, found.ok() ? found.order_index : kUiListNoSelection,
        timeline.scroll_units, timeline.viewport_units, timeline.list->total_units, reverse, bounds, eligible, &timeline);
    if (!target.ok()) return {};
    const auto* record = message_store_resolve(*timeline.store, message_store_ordered_handle(*timeline.store, target.index));
    const u64 id = record ? record->id : 0;
    if (!id || !message_timeline_set_scroll(&timeline, target.scroll_units, false) ||
        !message_scene_update(scene, scene->viewport).ok() || !message_scene_focus_message(scene, id)) return {};
    return {id, scene->widgets.focused, target.index};
}
} // namespace maicord
