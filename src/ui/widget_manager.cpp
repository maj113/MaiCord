#include "maicord/core/time.hpp"
#include "maicord/ui/widget_manager.hpp"

namespace maicord {
namespace {

bool widget_focusable(const UiWidgetManager& manager, const UiWidget& widget) noexcept {
    if (!(widget.flags & ui_widget_focusable))
        return false;
    return ui_widget_manager_input_enabled(
        manager, {static_cast<u16>(&widget - manager.storage.data), widget.generation});
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
    UiLayoutNodeSpec layout_spec = description.layout;
    if (description.kind == UiWidgetKind::scroll_view) {
        layout_spec.style.flags |= ui_layout_node_clip_children;
        if (!(layout_spec.style.flags &
              (ui_layout_node_scroll_vertical | ui_layout_node_scroll_horizontal)))
            layout_spec.style.flags |= ui_layout_node_scroll_vertical;
    }
    const UiLayoutResult layout_result =
        ui_layout_engine_add(&manager->layout, root ? ui_layout_invalid_handle() : parent_widget->layout,
                             layout_spec, &layout_handle);
    if (!layout_result.ok() || layout_handle.slot != manager->count) {
        ++manager->rejected;
        return UiWidgetResult{UiWidgetError::layout, ui_widget_invalid_handle()};
    }
    const u16 slot = static_cast<u16>(manager->count++);
    UiWidget& widget = manager->storage[slot];
    widget = UiWidget{};
    widget.layout = layout_handle;
    widget.parent = root ? ui_widget_invalid_handle() : parent;
    widget.id = description.id;
    widget.kind = description.kind;
    widget.flags = description.flags;
    widget.generation = manager->generation;
    return UiWidgetResult{UiWidgetError::none, UiWidgetHandle{slot, widget.generation}};
}

} // namespace

bool ui_widget_manager_initialize(UiWidgetManager* manager, Slice<UiWidget> widget_storage,
                                  Slice<UiLayoutNode> layout_storage) noexcept {
    if (!manager || !widget_storage.data || widget_storage.count == 0 ||
        widget_storage.count > kInvalidUiWidgetSlot || widget_storage.count > layout_storage.count) {
        return false;
    }
    *manager = UiWidgetManager{};
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

bool ui_widget_manager_place_part(UiWidgetManager* manager, UiWidgetHandle handle, UiRect rectangle,
                                  UiRect origin, bool visible, bool enabled,
                                  bool focusable) noexcept {
    auto* widget = ui_widget_manager_resolve(manager, handle);
    auto* node = widget ? ui_layout_engine_resolve(&manager->layout, widget->layout) : nullptr;
    if (!node)
        return false;
    auto& style = node->spec.style;
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

UiLayoutResult ui_widget_manager_layout(UiWidgetManager* manager, UiSize viewport) noexcept {
    if (!manager) {
        return UiLayoutResult{UiLayoutError::invalid_argument, 0, 0};
    }
    const auto result = ui_layout_engine_layout(&manager->layout, viewport);
    if (result.ok())
        ui_widget_manager_reconcile_input(manager);
    return result;
}

UiLayoutResult ui_widget_manager_layout_rectangle(UiWidgetManager* manager, UiRect viewport) noexcept {
    if (!manager) {
        return UiLayoutResult{UiLayoutError::invalid_argument, 0, 0};
    }
    const auto result = ui_layout_engine_layout_rectangle(&manager->layout, viewport);
    if (result.ok())
        ui_widget_manager_reconcile_input(manager);
    return result;
}

const UiWidget* ui_widget_manager_resolve(const UiWidgetManager& manager, UiWidgetHandle handle) noexcept {
    if (handle.generation == 0 || handle.slot >= manager.count) {
        return nullptr;
    }
    const UiWidget& widget = manager.storage[handle.slot];
    return widget.generation == handle.generation ? &widget : nullptr;
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
        if (widget.id == id && widget.generation == manager.generation) {
            return UiWidgetHandle{static_cast<u16>(index), widget.generation};
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
        const UiLayoutNode* layout = ui_layout_engine_resolve(manager.layout, widget.layout);
        if (!layout || !layout->visible || (widget.flags & ui_widget_enabled) == 0 ||
            (widget.flags & ui_widget_hit_test) == 0 || !ui_rect_contains(layout->rectangle, x, y) ||
            !ui_rect_contains(layout->clip_rectangle, x, y)) {
            continue;
        }
        const UiWidgetHandle handle{static_cast<u16>(index), widget.generation};
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
    *anchor = {};
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

bool ui_widget_manager_input_enabled(const UiWidgetManager& manager, UiWidgetHandle handle) noexcept {
    const auto* target = ui_widget_manager_resolve(manager, handle);
    if (!target || !(target->flags & ui_widget_enabled))
        return false;
    for (u32 depth = 0; depth < kMaximumUiLayoutDepth; ++depth) {
        const auto* widget = ui_widget_manager_resolve(manager, handle);
        if (!widget || (widget->flags & ui_widget_block_input))
            return false;
        const auto* layout = ui_layout_engine_resolve(manager.layout, widget->layout);
        if (!layout || !layout->visible || !(layout->spec.style.flags & ui_layout_node_visible))
            return false;
        if (!widget->parent.generation)
            return handle.slot == 0 && widget->parent.slot == kInvalidUiWidgetSlot &&
                   layout->parent == kInvalidUiLayoutSlot;
        const auto* parent = ui_widget_manager_resolve(manager, widget->parent);
        if (!parent || widget->parent.slot >= handle.slot || layout->parent != parent->layout.slot)
            return false;
        handle = widget->parent;
    }
    return false;
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
            return UiWidgetHandle{static_cast<u16>(index), widget.generation};
    }
    return ui_widget_invalid_handle();
}

const UiLayoutNode* ui_widget_manager_layout_node(const UiWidgetManager& manager,
                                                  UiWidgetHandle handle) noexcept {
    const UiWidget* widget = ui_widget_manager_resolve(manager, handle);
    return widget ? ui_layout_engine_resolve(manager.layout, widget->layout) : nullptr;
}

u32 ui_widget_manager_format_path(const UiWidgetManager& manager, UiWidgetHandle handle,
                                  Slice<char> output) noexcept {
    if ((!output.data && output.count != 0) || output.count == 0) {
        return 0;
    }
    UiWidgetHandle chain[kMaximumUiLayoutDepth]{};
    u32 depth = 0;
    const UiWidget* widget = ui_widget_manager_resolve(manager, handle);
    while (widget && depth < kMaximumUiLayoutDepth) {
        chain[depth++] = handle;
        handle = widget->parent;
        widget = ui_widget_manager_resolve(manager, handle);
    }
    u32 written = 0;
    for (u32 reverse = depth; reverse != 0; --reverse) {
        const UiWidget* path_widget = ui_widget_manager_resolve(manager, chain[reverse - 1]);
        const UiLayoutNode* layout =
            path_widget ? ui_layout_engine_resolve(manager.layout, path_widget->layout) : nullptr;
        if (!layout) {
            break;
        }
        if (written != 0 && written < output.count) {
            output[written++] = '/';
        }
        for (u32 index = 0; index < layout->spec.debug_name.bytes && written < output.count; ++index) {
            output[written++] = layout->spec.debug_name.data[index];
        }
    }
    return written;
}

} // namespace maicord
