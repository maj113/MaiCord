#pragma once

#include "maicord/core/stable_storage.hpp"
#include "maicord/ui/widget_manager.hpp"

namespace maicord {

// Stable-address, page-committed backing for large virtualized widget trees.
// Handles remain slot based and existing scene data never moves while capacity
// grows, so a resize cannot invalidate widget or layout references.
struct UiWidgetStorageOwner {
    StableStorage widgets;
    StableStorage layout_nodes;
    u32 capacity;
    u32 reserved_capacity;
};

// Stable-address backing for the materialized rows of a virtualized scene.
// The source collection remains external; only viewport rows are committed.
struct UiVirtualRowStorageOwner {
    StableStorage rows;
};

// Compact one-allocation backing for fixed-size retained scenes. An optional
// typed sidecar lets a scene keep its projected records beside widgets and
// layout nodes without embedding maximum-capacity arrays in the scene object.
struct UiFixedWidgetStorageOwner {
    Arena arena;
    Slice<UiWidget> widgets;
    Slice<UiLayoutNode> layout_nodes;
    void* sidecar;
    u32 sidecar_bytes;
};

bool ui_widget_storage_initialize(UiWidgetStorageOwner* storage, u32 reserved_capacity,
                                  u32 initial_capacity) noexcept;
bool ui_widget_storage_reserve(UiWidgetStorageOwner* storage, u32 required_capacity) noexcept;
bool ui_widget_storage_grow(void* context, u32 required_count, Slice<UiWidget>* widgets,
                            Slice<UiLayoutNode>* layout_nodes) noexcept;
void ui_widget_storage_release(UiWidgetStorageOwner* storage) noexcept;

bool ui_virtual_row_storage_initialize(UiVirtualRowStorageOwner* storage, u32 reserved_capacity,
                                       u32 initial_capacity, u32 row_bytes, u32 row_alignment) noexcept;
bool ui_virtual_row_storage_grow(UiVirtualRowStorageOwner* storage, u32 required_capacity) noexcept;
void ui_virtual_row_storage_release(UiVirtualRowStorageOwner* storage) noexcept;

bool ui_fixed_widget_storage_initialize(UiFixedWidgetStorageOwner* storage, u32 capacity,
                                        u32 sidecar_bytes = 0, u32 sidecar_alignment = 1) noexcept;
void ui_fixed_widget_storage_release(UiFixedWidgetStorageOwner* storage) noexcept;

template <typename T> T* ui_fixed_widget_storage_sidecar(UiFixedWidgetStorageOwner& storage) noexcept {
    return storage.sidecar_bytes != 0 && storage.sidecar_bytes % sizeof(T) == 0
               ? static_cast<T*>(storage.sidecar)
               : nullptr;
}

inline Slice<UiWidget> ui_widget_storage_widgets(UiWidgetStorageOwner& storage) noexcept {
    return Slice<UiWidget>{stable_storage_data<UiWidget>(storage.widgets), storage.capacity};
}

inline Slice<UiLayoutNode> ui_widget_storage_layout_nodes(UiWidgetStorageOwner& storage) noexcept {
    return Slice<UiLayoutNode>{stable_storage_data<UiLayoutNode>(storage.layout_nodes), storage.capacity};
}

template <typename T> Slice<T> ui_virtual_row_storage_rows(UiVirtualRowStorageOwner& storage) noexcept {
    return Slice<T>{stable_storage_data<T>(storage.rows), storage.rows.capacity};
}

} // namespace maicord
