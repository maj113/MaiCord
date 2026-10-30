#include "maicord/core/time.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/widget_manager.hpp"

namespace maicord {
namespace {

bool widget_focusable(const UiWidgetManager& manager, const UiWidget& widget) noexcept {
    if (!(widget.flags & ui_widget_focusable))
        return false;
    return ui_widget_manager_input_enabled(
        manager, {static_cast<u16>(&widget - manager.storage.data), manager.generation});
}

UiWidgetResult add_widget(UiWidgetManager* manager, UiWidgetHandle parent, const UiWidgetDesc& description,
                          bool id_prevalidated) noexcept {
    if (!manager || !manager->storage.data || description.id == 0) {
        return UiWidgetResult{UiWidgetError::invalid_argument, ui_widget_invalid_handle()};
    }
    if ((manager->count >= manager->storage.count || manager->count >= manager->layout.storage.count) &&
        !ui_widget_manager_reserve(manager, manager->count + 1u)) {
        ++manager->rejected;
        return UiWidgetResult{UiWidgetError::capacity, ui_widget_invalid_handle()};
    }
    if (!id_prevalidated && ui_widget_manager_find(*manager, description.id).generation != 0) {
        ++manager->rejected;
        return UiWidgetResult{UiWidgetError::duplicate_id, ui_widget_invalid_handle()};
    }
    const bool root = manager->count == 0;
    const UiWidget* parent_widget = root ? nullptr : ui_widget_manager_resolve(*manager, parent);
    if ((root && parent.slot != kInvalidUiWidgetSlot) || (!root && !parent_widget)) {
        ++manager->rejected;
        return UiWidgetResult{UiWidgetError::invalid_parent, ui_widget_invalid_handle()};
    }
    UiLayoutNodeHandle layout_handle{};
    const UiLayoutResult layout_result =
        ui_layout_engine_add(&manager->layout,
                             root ? ui_layout_invalid_handle()
                                  : UiLayoutNodeHandle{parent.slot, manager->layout.generation},
                             description.layout, &layout_handle);
    // Scroll flags do not affect admission; set them on the retained copy.
    if (layout_result.ok() && description.kind == UiWidgetKind::scroll_view) {
        auto& flags = manager->layout.storage[layout_handle.slot].spec.style.flags;
        flags |= ui_layout_node_clip_children;
        if (!(flags & (ui_layout_node_scroll_vertical | ui_layout_node_scroll_horizontal)))
            flags |= ui_layout_node_scroll_vertical;
    }
    if (!layout_result.ok() || layout_handle.slot != manager->count) {
        ++manager->rejected;
        return UiWidgetResult{UiWidgetError::layout, ui_widget_invalid_handle()};
    }
    const u16 slot = static_cast<u16>(manager->count++);
    manager->storage[slot] = UiWidget{description.id, description.kind, description.flags};
    return UiWidgetResult{UiWidgetError::none, UiWidgetHandle{slot, manager->generation}};
}

} // namespace

bool ui_widget_manager_initialize(UiWidgetManager* manager, const Slice<UiWidget>& widget_view,
                                  const Slice<UiLayoutNode>& layout_view) noexcept {
    const Slice<UiWidget> widget_storage = widget_view; // *manager is initialized below
    const Slice<UiLayoutNode> layout_storage = layout_view;
    if (!manager || !widget_storage.data || widget_storage.count == 0 ||
        widget_storage.count > kInvalidUiWidgetSlot || widget_storage.count > layout_storage.count) {
        return false;
    }
    reset_value<UiWidgetManager>(manager);
    manager->storage = widget_storage;
    manager->generation = 1;
    manager->focused = ui_widget_invalid_handle();
    manager->hovered = ui_widget_invalid_handle();
    manager->captured = ui_widget_invalid_handle();
    return ui_layout_engine_initialize(&manager->layout, layout_storage);
}

void ui_widget_manager_reset(UiWidgetManager* manager) noexcept {
    if (!manager || !manager->storage.data) {
        return;
    }
    ++manager->generation;
    if (manager->generation == 0) {
        manager->generation = 1;
    }
    manager->count = 0;
    manager->focused = ui_widget_invalid_handle();
    manager->hovered = ui_widget_invalid_handle();
    manager->captured = ui_widget_invalid_handle();
    ui_layout_engine_reset(&manager->layout);
}

bool ui_widget_manager_bind_growable_storage(UiWidgetManager* manager, UiWidgetStorageGrowCallback callback,
                                             void* context) noexcept {
    if (!manager || !manager->storage.data || !manager->layout.storage.data || !callback || !context) {
        return false;
    }
    manager->grow_storage = callback;
    manager->grow_storage_context = context;
    return true;
}

bool ui_widget_manager_reserve(UiWidgetManager* manager, u32 required_count) noexcept {
    if (!manager || required_count > kInvalidUiWidgetSlot) {
        return false;
    }
    if (required_count <= manager->storage.count && required_count <= manager->layout.storage.count) {
        return true;
    }
    if (!manager->grow_storage) {
        return false;
    }
    Slice<UiWidget> widgets = manager->storage;
    Slice<UiLayoutNode> layout_nodes = manager->layout.storage;
    if (!manager->grow_storage(manager->grow_storage_context, required_count, &widgets, &layout_nodes) ||
        widgets.data != manager->storage.data || layout_nodes.data != manager->layout.storage.data ||
        widgets.count < required_count || layout_nodes.count < required_count ||
        widgets.count > kInvalidUiWidgetSlot) {
        return false;
    }
    manager->storage = widgets;
    manager->layout.storage = layout_nodes;
    return true;
}

bool ui_widget_manager_set_measure_callback(UiWidgetManager* manager, UiLayoutMeasureCallback callback,
                                            void* context) noexcept {
    return manager && ui_layout_engine_set_measure_callback(&manager->layout, callback, context);
}

UiWidgetResult ui_widget_manager_add(UiWidgetManager* manager, UiWidgetHandle parent,
                                     const UiWidgetDesc& description) noexcept {
    return add_widget(manager, parent, description, false);
}

UiWidgetResult ui_widget_manager_add_prevalidated(UiWidgetManager* manager, UiWidgetHandle parent,
                                                  const UiWidgetDesc& description) noexcept {
    return add_widget(manager, parent, description, true);
}

UiWidgetResult ui_widget_manager_add_keyed(UiWidgetManager* manager, UiWidgetHandle parent, u32 id,
                                           UiWidgetKind kind, u16 flags, const char* name, u32 name_bytes,
                                           const UiLayoutStyle& style) noexcept {
    return add_widget(manager, parent, {id, kind, flags, {{name, name_bytes}, style, {}, 0, id}}, true);
}

bool ui_widget_manager_initialize_fill_root(UiWidgetManager* manager, Slice<UiWidget> widget_storage,
                                            Slice<UiLayoutNode> layout_storage, Utf8View name, u16 flags,
                                            UiWidgetReference* owner) noexcept {
    if (!ui_widget_manager_initialize(manager, widget_storage, layout_storage))
        return false;
    const auto root = ui_widget_manager_add(
        manager, ui_widget_invalid_handle(),
        {1, UiWidgetKind::container, ui_widget_none, {name, ui_layout_fill_style(flags), {}, 0, 1}});
    if (!root.ok())
        return false;
    *owner = {manager, root.handle};
    return true;
}

UiWidgetResult ui_widget_manager_add_part(UiWidgetManager* manager, UiWidgetHandle parent,
                                          UiWidgetKind kind, Utf8View name) noexcept {
    auto style = ui_layout_style();
    style.flags |= ui_layout_node_clip_children | ui_layout_node_allow_overlap |
                   ui_layout_node_allow_zero_size;
    return ui_widget_manager_add_prevalidated(manager, parent,
                                              {manager->count + 1, kind, ui_widget_none, {name, style}});
}

UiWidgetResult ui_widget_manager_add_placed(UiWidgetManager* manager, UiWidgetHandle parent, UiWidgetKind kind,
                                            u16 flags, Utf8View name, const UiRect& rectangle,
                                            const UiRect& origin) noexcept {
    auto style = ui_layout_style();
    style.width = ui_layout_fixed(ui_rect_width(rectangle));
    style.height = ui_layout_fixed(ui_rect_height(rectangle));
    style.margin.left = rectangle.left - origin.left;
    style.margin.top = rectangle.top - origin.top;
    style.flags |= ui_layout_node_allow_overlap | ui_layout_node_clip_children;
    const u32 id = manager->count + 1;
    return ui_widget_manager_add_prevalidated(manager, parent, {id, kind, flags, {name, style, {}, 0, id}});
}

bool ui_widget_manager_place_part(UiWidgetManager* manager, UiWidgetHandle handle, UiRect rectangle,
                                  UiRect origin, bool visible, bool enabled,
                                  bool focusable) noexcept {
    auto* widget = ui_widget_manager_resolve(manager, handle);
    if (!widget)
        return false;
    auto& style = manager->layout.storage[handle.slot].spec.style;
    const float width = visible ? ui_rect_width(rectangle) : 0;
    const float height = visible ? ui_rect_height(rectangle) : 0;
    const float left = visible ? rectangle.left - origin.left : 0;
    const float top = visible ? rectangle.top - origin.top : 0;
    const u16 flags = static_cast<u16>(ui_layout_node_clip_children | ui_layout_node_allow_overlap |
                                       ui_layout_node_allow_zero_size |
                                       (visible ? ui_layout_node_visible : 0));
    const bool changed = style.width.mode != UiLayoutLengthMode::fixed || style.width.value != width ||
                         style.height.mode != UiLayoutLengthMode::fixed || style.height.value != height ||
                         style.margin.left != left || style.margin.top != top || style.flags != flags;
    style.width = ui_layout_fixed(width);
    style.height = ui_layout_fixed(height);
    style.margin = {left, top, 0, 0};
    style.flags = flags;
    widget->flags = static_cast<u16>(visible && enabled ? ui_widget_enabled | ui_widget_hit_test |
                                                              (focusable ? ui_widget_focusable : 0)
                                                        : 0);
    return changed;
}

UiLayoutResult ui_widget_manager_layout_rectangle(UiWidgetManager* manager, const UiRect& viewport) noexcept {
    const auto result = ui_layout_engine_layout_rectangle(&manager->layout, viewport);
    if (result.ok())
        ui_widget_manager_reconcile_input(manager);
    return result;
}

const UiWidget* ui_widget_manager_resolve(const UiWidgetManager& manager, UiWidgetHandle handle) noexcept {
    if (handle.generation == 0 || handle.generation != manager.generation || handle.slot >= manager.count) {
        return nullptr;
    }
    return &manager.storage[handle.slot];
}

UiWidget* ui_widget_manager_resolve(UiWidgetManager* manager, UiWidgetHandle handle) noexcept {
    if (!manager) {
        return nullptr;
    }
    return const_cast<UiWidget*>(ui_widget_manager_resolve(*manager, handle));
}

UiWidgetHandle ui_widget_manager_find(const UiWidgetManager& manager, u32 id) noexcept {
    for (u32 index = 0; index < manager.count; ++index) {
        const UiWidget& widget = manager.storage[index];
        if (widget.id == id) {
            return UiWidgetHandle{static_cast<u16>(index), manager.generation};
        }
    }
    return ui_widget_invalid_handle();
}

UiWidgetHandle ui_widget_manager_hit_test(const UiWidgetManager& manager, float x, float y) noexcept {
    if (!ui_coordinate_valid(x) || !ui_coordinate_valid(y)) {
        return ui_widget_invalid_handle();
    }
    for (u32 reverse = manager.count; reverse != 0; --reverse) {
        const u32 index = reverse - 1;
        const UiWidget& widget = manager.storage[index];
        const UiLayoutNode& layout = manager.layout.storage[index];
        if (!layout.visible || (widget.flags & ui_widget_enabled) == 0 ||
            (widget.flags & ui_widget_hit_test) == 0 || !ui_rect_contains(layout.rectangle, x, y) ||
            !ui_rect_contains(layout.clip_rectangle, x, y)) {
            continue;
        }
        const UiWidgetHandle handle{static_cast<u16>(index), manager.generation};
        if (ui_widget_manager_input_enabled(manager, handle) &&
            (!manager.hit_filter || manager.hit_filter(manager.hit_filter_context, manager, handle, x, y)))
            return handle;
    }
    return ui_widget_invalid_handle();
}

bool ui_widget_manager_focus(UiWidgetManager* manager, UiWidgetHandle handle) noexcept {
    if (!manager) {
        return false;
    }
    if (handle.generation == 0) {
        manager->focused = ui_widget_invalid_handle();
        return true;
    }
    const UiWidget* widget = ui_widget_manager_resolve(*manager, handle);
    if (!widget || !widget_focusable(*manager, *widget)) {
        return false;
    }
    manager->focused = handle;
    return true;
}

bool ui_widget_manager_context_anchor(const UiWidgetManager& manager, UiRect* anchor) noexcept {
    if (!anchor)
        return false;
    reset_value(anchor);
    const auto* widget = ui_widget_manager_resolve(manager, manager.focused);
    if (!widget || !widget_focusable(manager, *widget))
        return false;
    const auto* node = ui_widget_manager_layout_node(manager, manager.focused);
    if (!node || !ui_rect_valid(node->rectangle) || !ui_rect_valid(node->clip_rectangle))
        return false;
    const auto visible = ui_rect_intersection(node->rectangle, node->clip_rectangle);
    if (!ui_rect_valid(visible))
        return false;
    *anchor = visible;
    return true;
}

bool ui_widget_manager_hover(UiWidgetManager* manager, UiWidgetHandle handle) noexcept {
    if (!manager) {
        return false;
    }
    if (handle.generation == 0) {
        manager->hovered = ui_widget_invalid_handle();
        return true;
    }
    const UiWidget* widget = ui_widget_manager_resolve(*manager, handle);
    if (!widget || (widget->flags & ui_widget_hit_test) == 0 ||
        !ui_widget_manager_input_enabled(*manager, handle)) {
        return false;
    }
    if (manager->hovered.slot != handle.slot || manager->hovered.generation != handle.generation)
        manager->hover_started_ms = clock_monotonic_milliseconds();
    manager->hovered = handle;
    return true;
}

bool ui_widget_manager_hover_at(UiWidgetManager* manager, float x, float y, bool* changed) noexcept {
    const UiWidgetHandle handle = ui_widget_manager_hit_test(*manager, x, y);
    *changed = handle.slot != manager->hovered.slot || handle.generation != manager->hovered.generation;
    return ui_widget_manager_hover(manager, handle);
}

bool ui_widget_manager_input_reachable(const UiWidgetManager& manager, UiWidgetHandle handle) noexcept {
    if (!ui_widget_manager_resolve(manager, handle))
        return false;
    // A node's parent was added before it, so the walk only moves to lower slots.
    for (u16 slot = handle.slot;;) {
        const UiLayoutNode& layout = manager.layout.storage[slot];
        if ((manager.storage[slot].flags & ui_widget_block_input) || !layout.visible ||
            !(layout.spec.style.flags & ui_layout_node_visible))
            return false;
        if (layout.parent == kInvalidUiLayoutSlot)
            return true;
        slot = layout.parent;
    }
}

bool ui_widget_manager_input_enabled(const UiWidgetManager& manager, UiWidgetHandle handle) noexcept {
    const auto* target = ui_widget_manager_resolve(manager, handle);
    return target && (target->flags & ui_widget_enabled) && ui_widget_manager_input_reachable(manager, handle);
}

bool ui_widget_manager_reconcile_input(UiWidgetManager* manager) noexcept {
    if (!manager)
        return false;
    bool changed = false;
    const auto reconcile = [&](UiWidgetHandle* handle, u16 required) noexcept {
        if (!handle->generation)
            return;
        const auto* widget = ui_widget_manager_resolve(*manager, *handle);
        if (!widget || (widget->flags & required) != required ||
            !ui_widget_manager_input_enabled(*manager, *handle)) {
            *handle = ui_widget_invalid_handle();
            changed = true;
        }
    };
    reconcile(&manager->focused, ui_widget_focusable);
    reconcile(&manager->hovered, ui_widget_hit_test);
    reconcile(&manager->captured, ui_widget_hit_test);
    return changed;
}

bool ui_widget_manager_block_input(UiWidgetManager* manager, UiWidgetHandle handle, bool blocked) noexcept {
    auto* widget = ui_widget_manager_resolve(manager, handle);
    if (!widget)
        return false;
    if (((widget->flags & ui_widget_block_input) != 0) == blocked)
        return true;
    if (blocked)
        widget->flags |= ui_widget_block_input;
    else
        widget->flags &= static_cast<u16>(~ui_widget_block_input);
    ui_widget_manager_reconcile_input(manager);
    return true;
}

bool ui_widget_manager_capture(UiWidgetManager* manager, UiWidgetHandle handle) noexcept {
    if (!manager)
        return false;
    if (!handle.generation) {
        manager->captured = ui_widget_invalid_handle();
        return true;
    }
    const auto* widget = ui_widget_manager_resolve(*manager, handle);
    if (!widget || !(widget->flags & ui_widget_hit_test) ||
        !ui_widget_manager_input_enabled(*manager, handle))
        return false;
    manager->captured = handle;
    return true;
}

UiWidgetHandle ui_widget_manager_pointer_target(UiWidgetManager* manager, float x, float y) noexcept {
    if (!manager || !ui_coordinate_valid(x) || !ui_coordinate_valid(y))
        return ui_widget_invalid_handle();
    ui_widget_manager_reconcile_input(manager);
    return manager->captured.generation ? manager->captured : ui_widget_manager_hit_test(*manager, x, y);
}

UiWidgetHandle ui_widget_manager_move_focus(UiWidgetManager* manager, bool reverse) noexcept {
    if (!manager)
        return ui_widget_invalid_handle();
    manager->focused = ui_widget_manager_next_focus(*manager, manager->focused, reverse, true);
    return manager->focused;
}

UiWidgetHandle ui_widget_manager_next_focus(const UiWidgetManager& manager, UiWidgetHandle handle,
                                            bool reverse, bool wrap) noexcept {
    if (!manager.count || !manager.storage.data || manager.count > manager.storage.count ||
        manager.count > kInvalidUiWidgetSlot)
        return ui_widget_invalid_handle();
    u32 current = ui_widget_manager_resolve(manager, handle) ? handle.slot : manager.count;
    for (u32 step = 0; step < manager.count; ++step) {
        if (!wrap && current != manager.count && (reverse ? current == 0 : current + 1 == manager.count))
            return ui_widget_invalid_handle();
        const u32 index =
            reverse ? (current == manager.count || current == 0 ? manager.count - 1 : current - 1)
                    : (current == manager.count || current + 1 >= manager.count ? 0 : current + 1);
        current = index;
        const UiWidget& widget = manager.storage[index];
        if (widget_focusable(manager, widget))
            return UiWidgetHandle{static_cast<u16>(index), manager.generation};
    }
    return ui_widget_invalid_handle();
}

// A live widget's node shares its slot (add_widget admits only that) and was
// added with it, so the node needs no second handle check.
const UiLayoutNode* ui_widget_manager_layout_node(const UiWidgetManager& manager,
                                                  UiWidgetHandle handle) noexcept {
    return ui_widget_manager_resolve(manager, handle) ? &manager.layout.storage[handle.slot] : nullptr;
}

UiLayoutNode* ui_widget_manager_layout_node(UiWidgetManager* manager, UiWidgetHandle handle) noexcept {
    return ui_widget_manager_resolve(*manager, handle) ? &manager->layout.storage[handle.slot] : nullptr;
}

UiLayoutNodeHandle ui_widget_manager_layout_handle(const UiWidgetManager& manager, UiWidgetHandle handle) noexcept {
    return ui_widget_manager_resolve(manager, handle) ? UiLayoutNodeHandle{handle.slot, manager.layout.generation}
                                                      : ui_layout_invalid_handle();
}

bool ui_widget_manager_set_scroll(UiWidgetManager* manager, UiWidgetHandle handle, UiPoint offset) noexcept {
    return ui_layout_engine_set_scroll(&manager->layout, ui_widget_manager_layout_handle(*manager, handle), offset);
}

UiWidgetHandle ui_widget_manager_parent(const UiWidgetManager& manager, UiWidgetHandle handle) noexcept {
    const UiLayoutNode* node = ui_widget_manager_layout_node(manager, handle);
    return node && node->parent != kInvalidUiLayoutSlot ? UiWidgetHandle{node->parent, manager.generation}
                                                         : ui_widget_invalid_handle();
}

} // namespace maicord
