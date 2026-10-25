#include "maicord/ui/widget_storage.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

static_assert(sizeof(UiWidget) == 16);
static_assert(sizeof(UiLayoutNode) == 216);

bool append_storage_bytes(usize* cursor, usize bytes, usize alignment) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return false;
    }
    const usize mask = alignment - 1;
    if (*cursor > ~usize{0} - mask) {
        return false;
    }
    const usize aligned = (*cursor + mask) & ~mask;
    if (bytes > ~usize{0} - aligned) {
        return false;
    }
    *cursor = aligned + bytes;
    return true;
}

} // namespace

bool ui_widget_storage_initialize(UiWidgetStorageOwner* storage, u32 reserved_capacity,
                                  u32 initial_capacity) noexcept {
    if (!storage || reserved_capacity == 0 || initial_capacity == 0 ||
        initial_capacity > reserved_capacity || reserved_capacity > kInvalidUiWidgetSlot) {
        return false;
    }
    reset_value<UiWidgetStorageOwner>(storage);
    if (!stable_storage_initialize(&storage->widgets, reserved_capacity, 0, sizeof(UiWidget),
                                   alignof(UiWidget)) ||
        !stable_storage_initialize(&storage->layout_nodes, reserved_capacity, 0, sizeof(UiLayoutNode),
                                   alignof(UiLayoutNode))) {
        ui_widget_storage_release(storage);
        return false;
    }
    storage->reserved_capacity = reserved_capacity;
    if (!ui_widget_storage_reserve(storage, initial_capacity)) {
        ui_widget_storage_release(storage);
        return false;
    }
    return true;
}

bool ui_widget_storage_reserve(UiWidgetStorageOwner* storage, u32 required_capacity) noexcept {
    if (!storage || !storage->widgets.arena.base || !storage->layout_nodes.arena.base ||
        required_capacity == 0 || required_capacity > storage->reserved_capacity) {
        return false;
    }
    if (required_capacity <= storage->capacity) {
        return true;
    }
    const ArenaMark widget_mark = arena_mark(&storage->widgets.arena);
    const ArenaMark layout_mark = arena_mark(&storage->layout_nodes.arena);
    if (!stable_storage_reserve(&storage->widgets, required_capacity) ||
        !stable_storage_reserve(&storage->layout_nodes, required_capacity)) {
        arena_rewind(&storage->widgets.arena, widget_mark);
        arena_rewind(&storage->layout_nodes.arena, layout_mark);
        storage->widgets.capacity = storage->capacity;
        storage->layout_nodes.capacity = storage->capacity;
        return false;
    }
    storage->capacity = required_capacity;
    return true;
}

bool ui_widget_storage_grow(void* context, u32 required_count, Slice<UiWidget>* widgets,
                            Slice<UiLayoutNode>* layout_nodes) noexcept {
    auto* storage = static_cast<UiWidgetStorageOwner*>(context);
    if (!widgets || !layout_nodes || !ui_widget_storage_reserve(storage, required_count)) {
        return false;
    }
    *widgets = ui_widget_storage_widgets(*storage);
    *layout_nodes = ui_widget_storage_layout_nodes(*storage);
    return true;
}

void ui_widget_storage_release(UiWidgetStorageOwner* storage) noexcept {
    if (!storage) {
        return;
    }
    stable_storage_release(&storage->widgets);
    stable_storage_release(&storage->layout_nodes);
    reset_value<UiWidgetStorageOwner>(storage);
}

bool ui_virtual_row_storage_initialize(UiVirtualRowStorageOwner* storage, u32 reserved_capacity,
                                       u32 initial_capacity, u32 row_bytes, u32 row_alignment) noexcept {
    if (!storage || storage->rows.arena.base || initial_capacity == 0) {
        return false;
    }
    return stable_storage_initialize(&storage->rows, reserved_capacity, initial_capacity, row_bytes,
                                     row_alignment);
}

bool ui_virtual_row_storage_grow(UiVirtualRowStorageOwner* storage, u32 required_capacity) noexcept {
    return storage && stable_storage_grow(&storage->rows, required_capacity);
}

void ui_virtual_row_storage_release(UiVirtualRowStorageOwner* storage) noexcept {
    if (!storage) {
        return;
    }
    stable_storage_release(&storage->rows);
    reset_value<UiVirtualRowStorageOwner>(storage);
}

bool ui_fixed_widget_storage_initialize(UiFixedWidgetStorageOwner* storage, u32 capacity, u32 sidecar_bytes,
                                        u32 sidecar_alignment) noexcept {
    if (!storage || storage->arena.base || capacity == 0 ||
        static_cast<usize>(capacity) > ~usize{0} / sizeof(UiWidget) ||
        static_cast<usize>(capacity) > ~usize{0} / sizeof(UiLayoutNode)) {
        return false;
    }
    usize reserve_bytes = 0;
    if (!append_storage_bytes(&reserve_bytes, static_cast<usize>(capacity) * sizeof(UiWidget),
                              alignof(UiWidget)) ||
        !append_storage_bytes(&reserve_bytes, static_cast<usize>(capacity) * sizeof(UiLayoutNode),
                              alignof(UiLayoutNode)) ||
        (sidecar_bytes != 0 && !append_storage_bytes(&reserve_bytes, sidecar_bytes, sidecar_alignment))) {
        return false;
    }
    reset_value<UiFixedWidgetStorageOwner>(storage);
    if (!arena_initialize(&storage->arena, reserve_bytes, 0, 4 * 1024)) {
        return false;
    }
    UiWidget* widgets = arena_allocate_zero<UiWidget>(&storage->arena, capacity);
    UiLayoutNode* layout_nodes = arena_allocate_zero<UiLayoutNode>(&storage->arena, capacity);
    void* sidecar = sidecar_bytes != 0
                        ? arena_allocate_zero(&storage->arena, sidecar_bytes, sidecar_alignment)
                        : nullptr;
    if (!widgets || !layout_nodes || (sidecar_bytes != 0 && !sidecar)) {
        ui_fixed_widget_storage_release(storage);
        return false;
    }
    storage->widgets = Slice<UiWidget>{widgets, capacity};
    storage->layout_nodes = Slice<UiLayoutNode>{layout_nodes, capacity};
    storage->sidecar = sidecar;
    storage->sidecar_bytes = sidecar_bytes;
    return true;
}

void ui_fixed_widget_storage_release(UiFixedWidgetStorageOwner* storage) noexcept {
    if (!storage) {
        return;
    }
    arena_release(&storage->arena);
    reset_value<UiFixedWidgetStorageOwner>(storage);
}

} // namespace maicord
