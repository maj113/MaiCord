#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/ui/core/geometry.hpp"

namespace maicord {

inline constexpr u16 kInvalidUiLayoutSlot = 0xFFFFu;
inline constexpr u16 kMaximumUiLayoutDepth = 64;
inline constexpr u32 kMaximumUiLayoutMeasurePasses = 4;

enum class UiLayoutFlow : u8 {
    overlay,
    horizontal,
    horizontal_wrap,
    vertical,
};

enum class UiLayoutLengthMode : u8 {
    content,
    fixed,
    fill,
};

enum class UiLayoutAlignment : u8 {
    start,
    center,
    end,
    stretch,
};

enum class UiLayoutJustification : u8 {
    start,
    end,
};

enum UiLayoutNodeFlag : u16 {
    ui_layout_node_none = 0,
    ui_layout_node_visible = 1u << 0,
    ui_layout_node_clip_children = 1u << 1,
    ui_layout_node_allow_overlap = 1u << 2,
    ui_layout_node_allow_zero_size = 1u << 4,
    ui_layout_node_text_content = 1u << 5,
    ui_layout_node_scroll_horizontal = 1u << 6,
    ui_layout_node_scroll_vertical = 1u << 7,
};

struct UiLayoutLength {
    UiLayoutLengthMode mode;
    float value;
};

struct UiLayoutStyle {
    UiLayoutLength width;
    UiLayoutLength height;
    UiSize minimum_size;
    UiSize maximum_size;
    UiInsets margin;
    UiInsets padding;
    float gap;
    UiLayoutFlow flow;
    UiLayoutAlignment horizontal_alignment;
    UiLayoutAlignment vertical_alignment;
    UiLayoutJustification justification;
    u16 flags;
};

struct UiLayoutNodeSpec {
    Utf8View debug_name;
    UiLayoutStyle style;
    // The declared content extent; a text node measured through the engine's
    // callback holds its latest measurement here instead.
    UiSize intrinsic_size;
    u32 key;
};

struct UiLayoutNodeHandle {
    u16 slot;
    u16 generation;
};

struct UiLayoutMeasureRequest {
    UiLayoutNodeHandle node;
    u32 key;
    UiSize constraint;
};

using UiLayoutMeasureCallback = bool (*)(void* context, const UiLayoutMeasureRequest& request, UiSize* size) noexcept;

struct UiLayoutNode {
    UiLayoutNodeSpec spec;
    UiRect rectangle;
    UiRect content_rectangle;
    UiRect clip_rectangle;
    UiSize measured_size;
    UiSize measure_constraint;
    UiPoint scroll_offset;
    u16 parent;
    u16 first_child;
    u16 last_child;
    u16 next_sibling;
    u16 generation;
    u8 depth;
    bool visible;
};
static_assert(sizeof(UiLayoutNode) == 192 && kMaximumUiLayoutDepth <= 0xFF);

enum class UiLayoutError : u8 {
    none,
    invalid_argument,
    capacity,
    invalid_parent,
    invalid_style,
    depth,
    missing_root,
    measurement_failed,
    unstable_layout,
};

struct UiLayoutResult {
    UiLayoutError error;
    u32 nodes;
    u32 revision;

    constexpr bool ok() const noexcept {
        return error == UiLayoutError::none;
    }
};

struct UiLayoutEngine {
    Slice<UiLayoutNode> storage;
    u32 count;
    u32 revision;
    u32 wrap_nodes;
    u16 generation;
    UiLayoutMeasureCallback measure_callback;
    void* measure_context;
};

constexpr UiLayoutLength ui_layout_content() noexcept {
    return UiLayoutLength{UiLayoutLengthMode::content, 0.0f};
}

constexpr UiLayoutLength ui_layout_fixed(float extent) noexcept {
    return UiLayoutLength{UiLayoutLengthMode::fixed, extent};
}

constexpr UiLayoutLength ui_layout_fill() noexcept {
    return UiLayoutLength{UiLayoutLengthMode::fill, 1.0f};
}

UiLayoutStyle ui_layout_style() noexcept;

// A node that stacks its children and fills its parent.
UiLayoutStyle ui_layout_fill_style(u16 flags) noexcept;

// The viewport of a virtualized list: fills its parent, stretches its rows and
// clips and scrolls them vertically.
UiLayoutStyle ui_layout_scroll_root_style() noexcept;

// A full-width filler of a fixed (possibly zero) height, standing in for the
// unrealized rows above a virtualized list's first realized row.
UiLayoutStyle ui_layout_spacer_style(float height) noexcept;

constexpr UiLayoutNodeHandle ui_layout_invalid_handle() noexcept {
    return UiLayoutNodeHandle{kInvalidUiLayoutSlot, 0};
}

bool ui_layout_engine_initialize(UiLayoutEngine* engine, Slice<UiLayoutNode> storage) noexcept;
void ui_layout_engine_reset(UiLayoutEngine* engine) noexcept;
bool ui_layout_engine_set_measure_callback(UiLayoutEngine* engine, UiLayoutMeasureCallback callback,
                                           void* context) noexcept;
UiLayoutResult ui_layout_engine_add(UiLayoutEngine* engine, UiLayoutNodeHandle parent,
                                    const UiLayoutNodeSpec& specification,
                                    UiLayoutNodeHandle* handle) noexcept;
UiLayoutResult ui_layout_engine_layout_rectangle(UiLayoutEngine* engine, UiRect viewport) noexcept;
bool ui_layout_engine_set_scroll(UiLayoutEngine* engine, UiLayoutNodeHandle handle,
                                 UiPoint offset) noexcept;
const UiLayoutNode* ui_layout_engine_resolve(const UiLayoutEngine& engine,
                                             UiLayoutNodeHandle handle) noexcept;
UiLayoutNode* ui_layout_engine_resolve(UiLayoutEngine* engine, UiLayoutNodeHandle handle) noexcept;

} // namespace maicord
