#include "maicord/ui/layout/layout_engine.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

constexpr float kLayoutEpsilon = 0.01f;

bool has_flag(const UiLayoutStyle& style, UiLayoutNodeFlag flag) noexcept {
    return (style.flags & static_cast<u16>(flag)) != 0;
}

float clamp_extent(float value, float minimum_value, float maximum_value) noexcept {
    return minimum(maximum(value, minimum_value), maximum_value);
}

bool valid_length(const UiLayoutLength& length) noexcept {
    if (!ui_extent_valid(length.value)) {
        return false;
    }
    return length.mode != UiLayoutLengthMode::fill || length.value > 0.0f;
}

bool valid_style(const UiLayoutNodeSpec& specification) noexcept {
    const UiLayoutStyle& style = specification.style;
    return valid_length(style.width) && valid_length(style.height) &&
           ui_size_valid_or_empty(style.minimum_size) && ui_size_valid_or_empty(style.maximum_size) &&
           style.minimum_size.width <= style.maximum_size.width &&
           style.minimum_size.height <= style.maximum_size.height && ui_insets_valid(style.margin) &&
           ui_insets_valid(style.padding) && ui_extent_valid(style.gap) &&
           ui_size_valid_or_empty(specification.intrinsic_size) &&
           (specification.debug_name.data || specification.debug_name.bytes == 0);
}

float outer_width(const UiLayoutNode& node) noexcept {
    return node.measured_size.width + node.spec.style.margin.left + node.spec.style.margin.right;
}

float outer_height(const UiLayoutNode& node) noexcept {
    return node.measured_size.height + node.spec.style.margin.top + node.spec.style.margin.bottom;
}

float resolved_measure(UiLayoutLength length, float content, float minimum_value,
                       float maximum_value) noexcept {
    const float desired = length.mode == UiLayoutLengthMode::fixed ? length.value : content;
    return clamp_extent(desired, minimum_value, maximum_value);
}

bool extents_match(UiSize left, UiSize right) noexcept {
    return absolute_value(left.width - right.width) <= kLayoutEpsilon &&
           absolute_value(left.height - right.height) <= kLayoutEpsilon;
}

UiSize initial_measure_constraint(const UiLayoutNode& node, UiSize viewport) noexcept {
    const UiLayoutStyle& style = node.spec.style;
    float width = maximum(0.0f, viewport.width - style.margin.left - style.margin.right -
                                    style.padding.left - style.padding.right);
    float height = kMaximumUiCoordinate;
    if (style.width.mode == UiLayoutLengthMode::fixed) {
        width = maximum(0.0f, style.width.value - style.padding.left - style.padding.right);
    }
    if (style.height.mode == UiLayoutLengthMode::fixed) {
        height = maximum(0.0f, style.height.value - style.padding.top - style.padding.bottom);
    }
    return UiSize{width, height};
}

UiLayoutError measure_intrinsic_content(UiLayoutEngine* engine, UiSize viewport) noexcept {
    if (!engine->measure_callback && engine->wrap_nodes == 0) {
        return UiLayoutError::none;
    }
    for (u32 index = 0; index < engine->count; ++index) {
        UiLayoutNode& node = engine->storage[index];
        const bool wrap = node.spec.style.flow == UiLayoutFlow::horizontal_wrap;
        const bool text = has_flag(node.spec.style, ui_layout_node_text_content) &&
                          engine->measure_callback && has_flag(node.spec.style, ui_layout_node_visible);
        if ((text || wrap) &&
            (!ui_extent_valid(node.measure_constraint.width) ||
             !ui_extent_valid(node.measure_constraint.height) || node.measure_constraint.width <= 0.0f)) {
            node.measure_constraint = initial_measure_constraint(node, viewport);
        }
        if (!text) {
            continue;
        }
        UiSize measured{};
        const UiLayoutMeasureRequest request{UiLayoutNodeHandle{static_cast<u16>(index), node.generation},
                                             node.spec.key, node.measure_constraint};
        if (!engine->measure_callback(engine->measure_context, request, &measured) ||
            !ui_size_valid_or_empty(measured)) {
            return UiLayoutError::measurement_failed;
        }
        node.spec.intrinsic_size = measured;
    }
    return UiLayoutError::none;
}

void measure_node(UiLayoutEngine* engine, u32 index) noexcept {
    UiLayoutNode& node = engine->storage[index];
    const UiLayoutStyle& style = node.spec.style;
    float children_width = 0.0f;
    float children_height = 0.0f;
    u32 child_count = 0;
    if (style.flow == UiLayoutFlow::horizontal_wrap) {
        float available = node.measure_constraint.width;
        if (!ui_extent_valid(available) || available <= 0.0f) {
            available = kMaximumUiCoordinate;
        }
        float row_width = 0.0f;
        float row_height = 0.0f;
        float committed_height = 0.0f;
        u32 row_count = 0;
        for (u16 child_slot = node.first_child; child_slot != kInvalidUiLayoutSlot;
             child_slot = engine->storage[child_slot].next_sibling) {
            const UiLayoutNode& child = engine->storage[child_slot];
            if (!has_flag(child.spec.style, ui_layout_node_visible)) {
                continue;
            }
            const float child_width = outer_width(child);
            const float child_height = outer_height(child);
            const float candidate = row_count == 0 ? child_width : row_width + style.gap + child_width;
            if (row_count != 0 && candidate > available + kLayoutEpsilon) {
                children_width = maximum(children_width, row_width);
                if (committed_height != 0.0f) {
                    committed_height += style.gap;
                }
                committed_height += row_height;
                row_width = child_width;
                row_height = child_height;
                row_count = 1;
            } else {
                row_width = candidate;
                row_height = maximum(row_height, child_height);
                ++row_count;
            }
            ++child_count;
        }
        if (row_count != 0) {
            children_width = maximum(children_width, row_width);
            if (committed_height != 0.0f) {
                committed_height += style.gap;
            }
            children_height = committed_height + row_height;
        }
    } else {
        for (u16 child_slot = node.first_child; child_slot != kInvalidUiLayoutSlot;
             child_slot = engine->storage[child_slot].next_sibling) {
            const UiLayoutNode& child = engine->storage[child_slot];
            if (!has_flag(child.spec.style, ui_layout_node_visible)) {
                continue;
            }
            const float child_width = outer_width(child);
            const float child_height = outer_height(child);
            if (style.flow == UiLayoutFlow::horizontal) {
                children_width += child_width;
                children_height = maximum(children_height, child_height);
            } else if (style.flow == UiLayoutFlow::vertical) {
                children_width = maximum(children_width, child_width);
                children_height += child_height;
            } else {
                children_width = maximum(children_width, child_width);
                children_height = maximum(children_height, child_height);
            }
            ++child_count;
        }
        if (child_count > 1) {
            const float gaps = style.gap * static_cast<float>(child_count - 1);
            if (style.flow == UiLayoutFlow::horizontal) {
                children_width += gaps;
            } else if (style.flow == UiLayoutFlow::vertical) {
                children_height += gaps;
            }
        }
    }
    const float content_width = maximum(node.spec.intrinsic_size.width, children_width) +
                                style.padding.left + style.padding.right;
    const float content_height = maximum(node.spec.intrinsic_size.height, children_height) +
                                 style.padding.top + style.padding.bottom;
    node.measured_size = UiSize{
        resolved_measure(style.width, content_width, style.minimum_size.width, style.maximum_size.width),
        resolved_measure(style.height, content_height, style.minimum_size.height,
                         style.maximum_size.height)};
}

void set_geometry(UiLayoutNode* node, const UiRect& rectangle, const UiRect& inherited_clip) noexcept {
    node->rectangle = rectangle;
    node->content_rectangle = ui_rect_inset(rectangle, node->spec.style.padding);
    node->clip_rectangle = inherited_clip;
    node->visible = has_flag(node->spec.style, ui_layout_node_visible);
}

UiRect children_clip(const UiLayoutNode& parent) noexcept {
    return has_flag(parent.spec.style, ui_layout_node_clip_children)
               ? ui_rect_intersection(parent.clip_rectangle, parent.content_rectangle)
               : parent.clip_rectangle;
}

float child_main_minimum(const UiLayoutNode& child, bool horizontal) noexcept {
    return horizontal ? child.spec.style.minimum_size.width : child.spec.style.minimum_size.height;
}

float child_main_measured(const UiLayoutNode& child, bool horizontal) noexcept {
    return horizontal ? child.measured_size.width : child.measured_size.height;
}

UiLayoutLength child_main_length(const UiLayoutNode& child, bool horizontal) noexcept {
    return horizontal ? child.spec.style.width : child.spec.style.height;
}

float child_main_margin(const UiLayoutNode& child, bool horizontal) noexcept {
    return horizontal ? child.spec.style.margin.left + child.spec.style.margin.right
                      : child.spec.style.margin.top + child.spec.style.margin.bottom;
}

float child_fill_weight(const UiLayoutNode& child, bool horizontal) noexcept {
    const UiLayoutLength length = child_main_length(child, horizontal);
    return length.mode == UiLayoutLengthMode::fill ? length.value : 0.0f;
}

float aligned_position(float start, float available, float extent, float margin_start, float margin_end,
                       UiLayoutAlignment alignment) noexcept {
    const float usable = maximum(0.0f, available - margin_start - margin_end);
    switch (alignment) {
    case UiLayoutAlignment::center:
        return start + margin_start + maximum(0.0f, (usable - extent) * 0.5f);
    case UiLayoutAlignment::end:
        return start + available - margin_end - extent;
    case UiLayoutAlignment::start:
    case UiLayoutAlignment::stretch:
        return start + margin_start;
    }
    return start + margin_start;
}

inline float arranged_cross_extent(const UiLayoutNode& child, bool horizontal_parent, float available) noexcept {
    const UiLayoutStyle& style = child.spec.style;
    const UiLayoutLength length = horizontal_parent ? style.height : style.width;
    const UiLayoutAlignment alignment =
        horizontal_parent ? style.vertical_alignment : style.horizontal_alignment;
    const float margins =
        horizontal_parent ? style.margin.top + style.margin.bottom : style.margin.left + style.margin.right;
    const float measured = horizontal_parent ? child.measured_size.height : child.measured_size.width;
    const float minimum_value = horizontal_parent ? style.minimum_size.height : style.minimum_size.width;
    const float maximum_value = horizontal_parent ? style.maximum_size.height : style.maximum_size.width;
    const bool stretches =
        length.mode == UiLayoutLengthMode::fill || alignment == UiLayoutAlignment::stretch;
    return clamp_extent(stretches ? maximum(0.0f, available - margins) : measured, minimum_value,
                        maximum_value);
}

inline UiRect overlay_child_rectangle(const UiLayoutNode& parent, const UiLayoutNode& child) noexcept {
    const auto& style = child.spec.style;
    const UiRect available = parent.content_rectangle;
    const float available_width = ui_rect_width(available);
    const float available_height = ui_rect_height(available);
    const float width = arranged_cross_extent(child, false, available_width);
    const float height = arranged_cross_extent(child, true, available_height);
    const float left = aligned_position(available.left, available_width, width, style.margin.left,
                                        style.margin.right, style.horizontal_alignment) -
                       parent.scroll_offset.x;
    const float top = aligned_position(available.top, available_height, height, style.margin.top,
                                       style.margin.bottom, style.vertical_alignment) -
                      parent.scroll_offset.y;
    return {left, top, left + width, top + height};
}

void arrange_overlay_children(UiLayoutEngine* engine, UiLayoutNode& parent) noexcept {
    const UiRect inherited_clip = children_clip(parent);
    for (u16 child_slot = parent.first_child; child_slot != kInvalidUiLayoutSlot;
         child_slot = engine->storage[child_slot].next_sibling) {
        UiLayoutNode& child = engine->storage[child_slot];
        if (!has_flag(child.spec.style, ui_layout_node_visible)) {
            continue;
        }
        set_geometry(&child, overlay_child_rectangle(parent, child), inherited_clip);
    }
}

void arrange_stack_children(UiLayoutEngine* engine, UiLayoutNode& parent, bool horizontal) noexcept {
    const UiRect available = parent.content_rectangle;
    const float available_main = horizontal ? ui_rect_width(available) : ui_rect_height(available);
    const float available_cross = horizontal ? ui_rect_height(available) : ui_rect_width(available);
    u32 child_count = 0;
    float occupied = 0.0f;
    float fill_weight = 0.0f;
    for (u16 child_slot = parent.first_child; child_slot != kInvalidUiLayoutSlot;
         child_slot = engine->storage[child_slot].next_sibling) {
        const UiLayoutNode& child = engine->storage[child_slot];
        if (!has_flag(child.spec.style, ui_layout_node_visible)) {
            continue;
        }
        const float weight = child_fill_weight(child, horizontal);
        occupied += (weight > 0.0f ? child_main_minimum(child, horizontal)
                                   : child_main_measured(child, horizontal)) +
                    child_main_margin(child, horizontal);
        fill_weight += weight;
        ++child_count;
    }
    if (child_count == 0) {
        return;
    }
    const float configured_gaps = parent.spec.style.gap * static_cast<float>(child_count - 1);
    occupied += configured_gaps;
    const float free_space = maximum(0.0f, available_main - occupied);
    const bool at_end = fill_weight == 0.0f && parent.spec.style.justification == UiLayoutJustification::end;
    float cursor = (horizontal ? available.left : available.top) + (at_end ? free_space : 0.0f);
    const UiRect inherited_clip = children_clip(parent);
    const float scroll_main = horizontal ? parent.scroll_offset.x : parent.scroll_offset.y;
    const float scroll_cross = horizontal ? parent.scroll_offset.y : parent.scroll_offset.x;
    u32 arranged_index = 0;
    for (u16 child_slot = parent.first_child; child_slot != kInvalidUiLayoutSlot;
         child_slot = engine->storage[child_slot].next_sibling) {
        UiLayoutNode& child = engine->storage[child_slot];
        if (!has_flag(child.spec.style, ui_layout_node_visible)) {
            continue;
        }
        const UiLayoutStyle& style = child.spec.style;
        const float weight = child_fill_weight(child, horizontal);
        float main_extent = weight > 0.0f
                                ? child_main_minimum(child, horizontal) +
                                      (fill_weight > 0.0f ? free_space * weight / fill_weight : 0.0f)
                                : child_main_measured(child, horizontal);
        main_extent = horizontal
                          ? clamp_extent(main_extent, style.minimum_size.width, style.maximum_size.width)
                          : clamp_extent(main_extent, style.minimum_size.height, style.maximum_size.height);
        const float cross_extent = arranged_cross_extent(child, horizontal, available_cross);
        const float main_margin_start = horizontal ? style.margin.left : style.margin.top;
        const float main_margin_end = horizontal ? style.margin.right : style.margin.bottom;
        cursor += main_margin_start;
        const UiLayoutAlignment alignment = horizontal ? style.vertical_alignment : style.horizontal_alignment;
        const float margin_start = horizontal ? style.margin.top : style.margin.left;
        const float margin_end = horizontal ? style.margin.bottom : style.margin.right;
        const float cross_position = aligned_position(horizontal ? available.top : available.left, available_cross,
                                                      cross_extent, margin_start, margin_end, alignment);
        UiRect rectangle{};
        if (horizontal) {
            rectangle =
                UiRect{cursor - scroll_main, cross_position - scroll_cross,
                       cursor - scroll_main + main_extent, cross_position - scroll_cross + cross_extent};
        } else {
            rectangle =
                UiRect{cross_position - scroll_cross, cursor - scroll_main,
                       cross_position - scroll_cross + cross_extent, cursor - scroll_main + main_extent};
        }
        set_geometry(&child, rectangle, inherited_clip);
        cursor += main_extent + main_margin_end;
        if (++arranged_index < child_count) {
            cursor += parent.spec.style.gap;
        }
    }
}

struct WrapRowMetrics {
    u16 next;
    u32 count;
    float width;
    float height;
};

WrapRowMetrics measure_wrap_row(const UiLayoutEngine& engine, u16 first, float available_width,
                                float gap) noexcept {
    WrapRowMetrics row{first, 0, 0.0f, 0.0f};
    u16 slot = first;
    while (slot != kInvalidUiLayoutSlot) {
        const UiLayoutNode& child = engine.storage[slot];
        const u16 next = child.next_sibling;
        if (!has_flag(child.spec.style, ui_layout_node_visible)) {
            slot = next;
            row.next = slot;
            continue;
        }
        const float width = outer_width(child);
        const float candidate = row.count == 0 ? width : row.width + gap + width;
        if (row.count != 0 && candidate > available_width + kLayoutEpsilon) {
            row.next = slot;
            break;
        }
        row.width = candidate;
        row.height = maximum(row.height, outer_height(child));
        ++row.count;
        slot = next;
        row.next = slot;
    }
    return row;
}

// Rectangles of a wrapping parent's visible children, row by row. place(slot,
// rectangle) returns true to stop.
template <typename Place>
void place_wrap_children(const UiLayoutEngine& engine, const UiLayoutNode& parent, Place place) noexcept {
    const UiRect available = parent.content_rectangle;
    const float available_width = ui_rect_width(available);
    float row_top = available.top;
    u16 row_first = parent.first_child;
    while (row_first != kInvalidUiLayoutSlot) {
        const WrapRowMetrics row = measure_wrap_row(engine, row_first, available_width, parent.spec.style.gap);
        if (row.count == 0) {
            break;
        }
        const float free_space = maximum(0.0f, available_width - row.width);
        float cursor =
            available.left + (parent.spec.style.justification == UiLayoutJustification::end ? free_space : 0.0f);
        u32 arranged_index = 0;
        for (u16 slot = row_first; slot != row.next; slot = engine.storage[slot].next_sibling) {
            const UiLayoutNode& child = engine.storage[slot];
            if (!has_flag(child.spec.style, ui_layout_node_visible)) {
                continue;
            }
            const UiLayoutStyle& style = child.spec.style;
            const float width =
                clamp_extent(child.measured_size.width, style.minimum_size.width, style.maximum_size.width);
            const float height = arranged_cross_extent(child, true, row.height);
            cursor += style.margin.left;
            const float top = aligned_position(row_top, row.height, height, style.margin.top, style.margin.bottom,
                                               style.vertical_alignment);
            if (place(slot, UiRect{cursor - parent.scroll_offset.x, top - parent.scroll_offset.y,
                                   cursor - parent.scroll_offset.x + width,
                                   top - parent.scroll_offset.y + height})) {
                return;
            }
            cursor += width + style.margin.right;
            if (++arranged_index < row.count) {
                cursor += parent.spec.style.gap;
            }
        }
        row_top += row.height;
        row_first = row.next;
        if (row_first != kInvalidUiLayoutSlot) {
            row_top += parent.spec.style.gap;
        }
    }
}

void arrange_wrap_children(UiLayoutEngine* engine, UiLayoutNode& parent) noexcept {
    const UiRect inherited_clip = children_clip(parent);
    place_wrap_children(*engine, parent, [&](u16 slot, const UiRect& rectangle) noexcept {
        set_geometry(&engine->storage[slot], rectangle, inherited_clip);
        return false;
    });
}

void arrange_children(UiLayoutEngine* engine, UiLayoutNode& parent) noexcept {
    switch (parent.spec.style.flow) {
    case UiLayoutFlow::overlay:
        arrange_overlay_children(engine, parent);
        break;
    case UiLayoutFlow::horizontal:
        arrange_stack_children(engine, parent, true);
        break;
    case UiLayoutFlow::horizontal_wrap:
        arrange_wrap_children(engine, parent);
        break;
    case UiLayoutFlow::vertical:
        arrange_stack_children(engine, parent, false);
        break;
    }
}

bool update_measure_constraints(UiLayoutEngine* engine) noexcept {
    if (!engine->measure_callback && engine->wrap_nodes == 0) {
        return false;
    }
    bool changed = false;
    for (u32 index = 0; index < engine->count; ++index) {
        UiLayoutNode& node = engine->storage[index];
        const bool text =
            engine->measure_callback && has_flag(node.spec.style, ui_layout_node_text_content);
        const bool wrap = node.spec.style.flow == UiLayoutFlow::horizontal_wrap;
        if (!node.visible || (!text && !wrap)) {
            continue;
        }
        UiSize constraint{maximum(0.0f, ui_rect_width(node.content_rectangle)),
                          node.spec.style.height.mode == UiLayoutLengthMode::content
                              ? kMaximumUiCoordinate
                              : maximum(0.0f, ui_rect_height(node.content_rectangle))};
        if (constraint.width <= 0.0f) {
            constraint.width = node.measure_constraint.width;
        }
        if (!extents_match(constraint, node.measure_constraint)) {
            node.measure_constraint = constraint;
            changed = true;
        }
    }
    return changed;
}

} // namespace

__declspec(noinline) UiLayoutStyle ui_layout_style() noexcept {
    return UiLayoutStyle{
        ui_layout_content(),
        ui_layout_content(),
        UiSize{0.0f, 0.0f},
        UiSize{kMaximumUiCoordinate, kMaximumUiCoordinate},
        UiInsets{},
        UiInsets{},
        0.0f,
        UiLayoutFlow::overlay,
        UiLayoutAlignment::start,
        UiLayoutAlignment::start,
        UiLayoutJustification::start,
        ui_layout_node_visible,
    };
}

UiLayoutStyle ui_layout_fill_style(u16 flags) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.flags = flags;
    return style;
}

UiLayoutStyle ui_layout_scroll_root_style() noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.flow = UiLayoutFlow::vertical;
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.vertical_alignment = UiLayoutAlignment::stretch;
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children |
                                   ui_layout_node_scroll_vertical);
    return style;
}

UiLayoutStyle ui_layout_spacer_style(float height) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fixed(height);
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_allow_zero_size);
    return style;
}

bool ui_layout_engine_initialize(UiLayoutEngine* engine, Slice<UiLayoutNode> storage) noexcept {
    if (!engine || !storage.data || storage.count == 0 || storage.count > kInvalidUiLayoutSlot) {
        return false;
    }
    reset_value<UiLayoutEngine>(engine);
    engine->storage = storage;
    engine->generation = 1;
    return true;
}

void ui_layout_engine_reset(UiLayoutEngine* engine) noexcept {
    if (!engine || !engine->storage.data || engine->storage.count == 0) {
        return;
    }
    ++engine->generation;
    if (engine->generation == 0) {
        engine->generation = 1;
    }
    engine->count = 0;
    engine->wrap_nodes = 0;
}

bool ui_layout_engine_set_measure_callback(UiLayoutEngine* engine, UiLayoutMeasureCallback callback,
                                           void* context) noexcept {
    if (!engine || !engine->storage.data || (!callback && context)) {
        return false;
    }
    engine->measure_callback = callback;
    engine->measure_context = context;
    for (u32 index = 0; index < engine->count; ++index) {
        reset_value<UiSize>(&engine->storage[index].measure_constraint);
    }
    return true;
}

UiLayoutResult ui_layout_engine_add(UiLayoutEngine* engine, UiLayoutNodeHandle parent,
                                    const UiLayoutNodeSpec& specification,
                                    UiLayoutNodeHandle* handle) noexcept {
    if (handle) {
        *handle = ui_layout_invalid_handle();
    }
    if (!engine || !handle || !engine->storage.data) {
        return UiLayoutResult{UiLayoutError::invalid_argument, 0, 0};
    }
    if (engine->count >= engine->storage.count || engine->count >= kInvalidUiLayoutSlot) {
        ++engine->rejected;
        return UiLayoutResult{UiLayoutError::capacity, engine->count, engine->revision};
    }
    if (!valid_style(specification)) {
        ++engine->rejected;
        return UiLayoutResult{UiLayoutError::invalid_style, engine->count, engine->revision};
    }
    const bool root = engine->count == 0;
    const UiLayoutNode* parent_node = root ? nullptr : ui_layout_engine_resolve(*engine, parent);
    if ((root && parent.slot != kInvalidUiLayoutSlot) || (!root && !parent_node)) {
        ++engine->rejected;
        return UiLayoutResult{UiLayoutError::invalid_parent, engine->count, engine->revision};
    }
    const u32 depth = root ? 0 : parent_node->depth + 1u;
    if (depth >= kMaximumUiLayoutDepth) {
        ++engine->rejected;
        return UiLayoutResult{UiLayoutError::depth, engine->count, engine->revision};
    }
    const u16 slot = static_cast<u16>(engine->count++);
    UiLayoutNode& node = engine->storage[slot];
    // Skip the specification prefix, which is overwritten below.
    clear_bytes(reinterpret_cast<u8*>(&node) + sizeof(node.spec), sizeof(node) - sizeof(node.spec));
    node.spec = specification;
    node.parent = root ? kInvalidUiLayoutSlot : parent.slot;
    node.first_child = kInvalidUiLayoutSlot;
    node.last_child = kInvalidUiLayoutSlot;
    node.next_sibling = kInvalidUiLayoutSlot;
    node.generation = engine->generation;
    node.depth = static_cast<u8>(depth);
    if (specification.style.flow == UiLayoutFlow::horizontal_wrap) {
        ++engine->wrap_nodes;
    }
    if (!root) {
        UiLayoutNode& mutable_parent = engine->storage[parent.slot];
        if (mutable_parent.last_child == kInvalidUiLayoutSlot) {
            mutable_parent.first_child = slot;
        } else {
            engine->storage[mutable_parent.last_child].next_sibling = slot;
        }
        mutable_parent.last_child = slot;
    }
    *handle = UiLayoutNodeHandle{slot, node.generation};
    return UiLayoutResult{UiLayoutError::none, engine->count, engine->revision};
}

UiLayoutResult ui_layout_engine_layout_rectangle(UiLayoutEngine* engine,
                                                 UiRect viewport_rectangle) noexcept {
    if (!engine || !engine->storage.data || engine->count == 0) {
        return UiLayoutResult{UiLayoutError::missing_root, 0, 0};
    }
    if (!ui_coordinate_valid(viewport_rectangle.left) || !ui_coordinate_valid(viewport_rectangle.top) ||
        !ui_coordinate_valid(viewport_rectangle.right) || !ui_coordinate_valid(viewport_rectangle.bottom) ||
        !ui_extent_valid(ui_rect_width(viewport_rectangle)) ||
        !ui_extent_valid(ui_rect_height(viewport_rectangle)) || !ui_rect_has_area(viewport_rectangle)) {
        ++engine->rejected;
        return UiLayoutResult{UiLayoutError::invalid_argument, engine->count, engine->revision};
    }
    UiLayoutNode& root = engine->storage[0];
    if (!has_flag(root.spec.style, ui_layout_node_visible)) {
        ++engine->rejected;
        return UiLayoutResult{UiLayoutError::missing_root, engine->count, engine->revision};
    }
    const UiSize viewport{ui_rect_width(viewport_rectangle), ui_rect_height(viewport_rectangle)};
    for (u32 pass = 0; pass < kMaximumUiLayoutMeasurePasses; ++pass) {
        const UiLayoutError measured = measure_intrinsic_content(engine, viewport);
        if (measured != UiLayoutError::none) {
            ++engine->rejected;
            return UiLayoutResult{measured, engine->count, engine->revision};
        }
        // Measure bottom-up; the arrange pass below places only visible
        // subtrees, so every node starts the pass hidden and empty.
        for (u32 index = engine->count; index-- != 0;) {
            UiLayoutNode& node = engine->storage[index];
            node.visible = false;
            reset_value<UiRect>(&node.rectangle);
            reset_value<UiRect>(&node.content_rectangle);
            reset_value<UiRect>(&node.clip_rectangle);
            measure_node(engine, index);
        }
        set_geometry(&root, viewport_rectangle, viewport_rectangle);
        for (u32 index = 0; index < engine->count; ++index) {
            UiLayoutNode& node = engine->storage[index];
            if (node.visible) {
                arrange_children(engine, node);
            }
        }
        ++engine->measure_passes;
        if (!update_measure_constraints(engine)) {
            ++engine->revision;
            return UiLayoutResult{UiLayoutError::none, engine->count, engine->revision};
        }
    }
    ++engine->unstable_measurements;
    ++engine->rejected;
    return UiLayoutResult{UiLayoutError::unstable_layout, engine->count, engine->revision};
}

bool ui_layout_engine_set_scroll(UiLayoutEngine* engine, UiLayoutNodeHandle handle,
                                 UiPoint offset) noexcept {
    UiLayoutNode* node = ui_layout_engine_resolve(engine, handle);
    if (!node || !ui_extent_valid(offset.x) || !ui_extent_valid(offset.y) ||
        (offset.x != 0.0f && !has_flag(node->spec.style, ui_layout_node_scroll_horizontal)) ||
        (offset.y != 0.0f && !has_flag(node->spec.style, ui_layout_node_scroll_vertical))) {
        return false;
    }
    node->scroll_offset = offset;
    return true;
}

const UiLayoutNode* ui_layout_engine_resolve(const UiLayoutEngine& engine,
                                             UiLayoutNodeHandle handle) noexcept {
    if (handle.generation == 0 || handle.slot >= engine.count) {
        return nullptr;
    }
    const UiLayoutNode& node = engine.storage[handle.slot];
    return node.generation == handle.generation ? &node : nullptr;
}

UiLayoutNode* ui_layout_engine_resolve(UiLayoutEngine* engine, UiLayoutNodeHandle handle) noexcept {
    if (!engine) {
        return nullptr;
    }
    return const_cast<UiLayoutNode*>(ui_layout_engine_resolve(*engine, handle));
}

} // namespace maicord
