#pragma once

#include "maicord/ui/layout_engine.hpp"

namespace maicord {

inline constexpr u16 kInvalidUiWidgetSlot = 0xFFFFu;

enum class UiWidgetKind : u8 {
    container,
    text,
    button,
    text_field,
    image,
    scroll_view,
    custom,
};

enum UiWidgetFlag : u16 {
    ui_widget_none = 0,
    ui_widget_enabled = 1u << 0,
    ui_widget_hit_test = 1u << 1,
    ui_widget_focusable = 1u << 2,
    // Blocks this subtree. A noninteractive container with flags
    // ui_widget_none still permits interactive descendants.
    ui_widget_block_input = 1u << 3,
};

struct UiWidgetHandle {
    u16 slot;
    u16 generation;
};

struct UiWidgetDesc {
    u32 id;
    UiWidgetKind kind;
    u16 flags;
    UiLayoutNodeSpec layout;
};

// All active slots share the manager generation; reset invalidates the whole tree.
struct UiWidget {
    UiLayoutNodeHandle layout;
    UiWidgetHandle parent;
    u32 id;
    UiWidgetKind kind;
    u16 flags;
};

enum class UiWidgetError : u8 {
    none,
    invalid_argument,
    capacity,
    duplicate_id,
    invalid_parent,
    layout,
};

struct UiWidgetResult {
    UiWidgetError error;
    UiWidgetHandle handle;

    constexpr bool ok() const noexcept {
        return error == UiWidgetError::none;
    }
};

using UiWidgetStorageGrowCallback = bool (*)(void* context, u32 required_count, Slice<UiWidget>* widgets,
                                             Slice<UiLayoutNode>* layout_nodes) noexcept;
struct UiWidgetManager;
// Narrows a hit-testable widget's pointer region to its interactive part (a
// settings row answers only on its button or switch). Rejected points fall
// through to the widgets below. Keyboard focus still covers the whole widget.
using UiWidgetHitFilter = bool (*)(const void* context, const UiWidgetManager& manager, UiWidgetHandle handle,
                                   float x, float y) noexcept;

struct UiWidgetManager {
    Slice<UiWidget> storage;
    UiLayoutEngine layout;
    UiWidgetStorageGrowCallback grow_storage;
    void* grow_storage_context;
    UiWidgetHitFilter hit_filter;
    const void* hit_filter_context;
    UiWidgetHandle focused;
    UiWidgetHandle hovered;
    u64 hover_started_ms;
    UiWidgetHandle captured;
    u32 count;
    u32 rejected;
    u16 generation;
};

constexpr UiWidgetHandle ui_widget_invalid_handle() noexcept {
    return UiWidgetHandle{kInvalidUiWidgetSlot, 0};
}

constexpr bool ui_widget_handle_equal(UiWidgetHandle left, UiWidgetHandle right) noexcept {
    return left.slot == right.slot && left.generation == right.generation;
}

// Equal and issued: a zero-generation handle matches nothing.
constexpr bool ui_widget_handle_matches(UiWidgetHandle handle, UiWidgetHandle target) noexcept {
    return handle.generation && handle.slot == target.slot && handle.generation == target.generation;
}

struct UiWidgetReference {
    const UiWidgetManager* manager;
    UiWidgetHandle handle;
};

bool ui_widget_manager_initialize(UiWidgetManager* manager, const Slice<UiWidget>& widget_storage,
                                  const Slice<UiLayoutNode>& layout_storage) noexcept;
void ui_widget_manager_reset(UiWidgetManager* manager) noexcept;
bool ui_widget_manager_bind_growable_storage(UiWidgetManager* manager, UiWidgetStorageGrowCallback callback,
                                             void* context) noexcept;
bool ui_widget_manager_reserve(UiWidgetManager* manager, u32 required_count) noexcept;
bool ui_widget_manager_set_measure_callback(UiWidgetManager* manager, UiLayoutMeasureCallback callback,
                                            void* context) noexcept;
UiWidgetResult ui_widget_manager_add(UiWidgetManager* manager, UiWidgetHandle parent,
                                     const UiWidgetDesc& description) noexcept;
// For builders that have already checked the complete ID batch for collisions.
UiWidgetResult ui_widget_manager_add_prevalidated(UiWidgetManager* manager, UiWidgetHandle parent,
                                                  const UiWidgetDesc& description) noexcept;
// The same for a node whose layout key is its widget ID; the style is copied once, inside.
UiWidgetResult ui_widget_manager_add_keyed(UiWidgetManager* manager, UiWidgetHandle parent, u32 id,
                                           UiWidgetKind kind, u16 flags, const char* name, u32 name_bytes,
                                           const UiLayoutStyle& style) noexcept;
template <usize Count>
__forceinline UiWidgetResult ui_widget_manager_add_keyed(UiWidgetManager* manager, UiWidgetHandle parent,
                                                         u32 id, UiWidgetKind kind, u16 flags,
                                                         const char (&name)[Count],
                                                         const UiLayoutStyle& style) noexcept {
    return ui_widget_manager_add_keyed(manager, parent, id, kind, flags, name, Count - 1, style);
}
// Initializes the manager with one container that fills the viewport and points `owner` at it.
bool ui_widget_manager_initialize_fill_root(UiWidgetManager* manager, Slice<UiWidget> widget_storage,
                                            Slice<UiLayoutNode> layout_storage, Utf8View name, u16 flags,
                                            UiWidgetReference* owner) noexcept;
// A part of a modal scene: clipped, free to overlap and allowed to collapse to zero size. Its ID is the
// next free one, so the scene's builder owns the ID space.
UiWidgetResult ui_widget_manager_add_part(UiWidgetManager* manager, UiWidgetHandle parent,
                                          UiWidgetKind kind, Utf8View name) noexcept;
// A part sized to `rectangle` and offset from `origin` by margins, clipped and free to overlap; its ID is the
// next free one. For scenes that register their controls while building a retained tree.
UiWidgetResult ui_widget_manager_add_placed(UiWidgetManager* manager, UiWidgetHandle parent, UiWidgetKind kind,
                                            u16 flags, Utf8View name, const UiRect& rectangle,
                                            const UiRect& origin) noexcept;
// Pins a part to `rectangle`, offset from `origin`. A part that is not visible collapses; one that is not
// visible and enabled takes no input. True when its layout changed.
bool ui_widget_manager_place_part(UiWidgetManager* manager, UiWidgetHandle handle, UiRect rectangle,
                                  UiRect origin, bool visible, bool enabled,
                                  bool focusable = true) noexcept;
UiLayoutResult ui_widget_manager_layout(UiWidgetManager* manager, UiSize viewport) noexcept;
UiLayoutResult ui_widget_manager_layout_rectangle(UiWidgetManager* manager, const UiRect& viewport) noexcept;
const UiWidget* ui_widget_manager_resolve(const UiWidgetManager& manager, UiWidgetHandle handle) noexcept;
UiWidget* ui_widget_manager_resolve(UiWidgetManager* manager, UiWidgetHandle handle) noexcept;
UiWidgetHandle ui_widget_manager_find(const UiWidgetManager& manager, u32 id) noexcept;
UiWidgetHandle ui_widget_manager_hit_test(const UiWidgetManager& manager, float x, float y) noexcept;
bool ui_widget_manager_focus(UiWidgetManager* manager, UiWidgetHandle handle) noexcept;
// Visible portion of the focused widget, in layout DIPs. Rejects stale,
// disabled, hidden, blocked or completely clipped targets; never uses a pointer.
bool ui_widget_manager_context_anchor(const UiWidgetManager& manager, UiRect* anchor) noexcept;
bool ui_widget_manager_hover(UiWidgetManager* manager, UiWidgetHandle handle) noexcept;
// The widget's chain to the root is visible, unblocked and well formed; the widget itself need not be enabled.
bool ui_widget_manager_input_reachable(const UiWidgetManager& manager, UiWidgetHandle handle) noexcept;
bool ui_widget_manager_input_enabled(const UiWidgetManager& manager, UiWidgetHandle handle) noexcept;
bool ui_widget_manager_block_input(UiWidgetManager* manager, UiWidgetHandle handle, bool blocked) noexcept;
// Logical capture is owned by the manager. The platform separately owns OS
// capture and must release it on cancellation. Invalid handle releases capture.
bool ui_widget_manager_capture(UiWidgetManager* manager, UiWidgetHandle handle) noexcept;
UiWidgetHandle ui_widget_manager_pointer_target(UiWidgetManager* manager, float x, float y) noexcept;
// Clears stale, hidden, disabled or blocked focus/hover/capture. Returns change.
bool ui_widget_manager_reconcile_input(UiWidgetManager* manager) noexcept;
UiWidgetHandle ui_widget_manager_move_focus(UiWidgetManager* manager, bool reverse) noexcept;
// Read-only traversal, optionally stopping at a scope boundary. Stale current
// handles start at the first/last eligible widget; no candidate changes focus.
UiWidgetHandle ui_widget_manager_next_focus(const UiWidgetManager& manager, UiWidgetHandle current,
                                            bool reverse, bool wrap) noexcept;
const UiLayoutNode* ui_widget_manager_layout_node(const UiWidgetManager& manager,
                                                  UiWidgetHandle handle) noexcept;

} // namespace maicord
