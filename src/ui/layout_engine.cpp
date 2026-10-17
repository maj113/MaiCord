#include "maicord/ui/layout_engine.hpp"

namespace maicord {
namespace {

constexpr float kLayoutEpsilon = 0.01f;
constexpr float kRenderedContentTolerance = 2.5f;

float absolute(float value) noexcept {
    return value < 0.0f ? -value : value;
}

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
           ui_extent_valid(specification.intrinsic_baseline) &&
           specification.intrinsic_baseline <= specification.intrinsic_size.height &&
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
    return absolute(left.width - right.width) <= kLayoutEpsilon &&
           absolute(left.height - right.height) <= kLayoutEpsilon;
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
            node.measured_intrinsic_size = node.spec.intrinsic_size;
            node.measured_intrinsic_baseline = node.spec.intrinsic_baseline;
            continue;
        }
        UiSize measured{};
        float baseline = 0.0f;
        const UiLayoutMeasureRequest request{UiLayoutNodeHandle{static_cast<u16>(index), node.generation},
                                             node.spec.key, node.measure_constraint};
        if (!engine->measure_callback(engine->measure_context, request, &measured, &baseline) ||
            !ui_size_valid_or_empty(measured) || !ui_extent_valid(baseline) || baseline > measured.height) {
            engine->unstable_node = request.node;
            return UiLayoutError::measurement_failed;
        }
        node.measured_intrinsic_size = measured;
        node.measured_intrinsic_baseline = baseline;
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
    const float content_width = maximum(node.measured_intrinsic_size.width, children_width) +
                                style.padding.left + style.padding.right;
    const float content_height = maximum(node.measured_intrinsic_size.height, children_height) +
                                 style.padding.top + style.padding.bottom;
    node.measured_size = UiSize{
        resolved_measure(style.width, content_width, style.minimum_size.width, style.maximum_size.width),
        resolved_measure(style.height, content_height, style.minimum_size.height,
                         style.maximum_size.height)};
    node.measured_baseline =
        minimum(node.measured_size.height, style.padding.top + node.measured_intrinsic_baseline);
}

void set_geometry(UiLayoutNode* node, UiRect rectangle, UiRect inherited_clip) noexcept {
    node->rectangle = rectangle;
    node->content_rectangle = ui_rect_inset(rectangle, node->spec.style.padding);
    node->clip_rectangle = inherited_clip;
    node->visible = has_flag(node->spec.style, ui_layout_node_visible);
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

float justify_offset(UiLayoutJustification justification, float free_space, u32 child_count,
                     float* extra_gap) noexcept {
    *extra_gap = 0.0f;
    if (free_space <= 0.0f) {
        return 0.0f;
    }
    switch (justification) {
    case UiLayoutJustification::center:
        return free_space * 0.5f;
    case UiLayoutJustification::end:
        return free_space;
    case UiLayoutJustification::space_between:
        if (child_count > 1) {
            *extra_gap = free_space / static_cast<float>(child_count - 1);
        }
        return 0.0f;
    case UiLayoutJustification::start:
        return 0.0f;
    }
    return 0.0f;
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
    case UiLayoutAlignment::baseline:
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
    const UiRect available = parent.content_rectangle;
    UiRect inherited_clip = parent.clip_rectangle;
    if (has_flag(parent.spec.style, ui_layout_node_clip_children)) {
        inherited_clip = ui_rect_intersection(inherited_clip, available);
    }
    for (u16 child_slot = parent.first_child; child_slot != kInvalidUiLayoutSlot;
         child_slot = engine->storage[child_slot].next_sibling) {
        UiLayoutNode& child = engine->storage[child_slot];
        if (!has_flag(child.spec.style, ui_layout_node_visible)) {
            child.visible = false;
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
    float maximum_baseline = 0.0f;
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
        if (horizontal && child.spec.style.vertical_alignment == UiLayoutAlignment::baseline) {
            maximum_baseline =
                maximum(maximum_baseline, child.spec.style.margin.top + child.measured_baseline);
        }
        ++child_count;
    }
    if (child_count == 0) {
        return;
    }
    const float configured_gaps = parent.spec.style.gap * static_cast<float>(child_count - 1);
    occupied += configured_gaps;
    const float free_space = maximum(0.0f, available_main - occupied);
    float extra_gap = 0.0f;
    float cursor = (horizontal ? available.left : available.top) +
                   (fill_weight == 0.0f ? justify_offset(parent.spec.style.justification, free_space,
                                                         child_count, &extra_gap)
                                        : 0.0f);
    UiRect inherited_clip = parent.clip_rectangle;
    if (has_flag(parent.spec.style, ui_layout_node_clip_children)) {
        inherited_clip = ui_rect_intersection(inherited_clip, available);
    }
    const float scroll_main = horizontal ? parent.scroll_offset.x : parent.scroll_offset.y;
    const float scroll_cross = horizontal ? parent.scroll_offset.y : parent.scroll_offset.x;
    u32 arranged_index = 0;
    for (u16 child_slot = parent.first_child; child_slot != kInvalidUiLayoutSlot;
         child_slot = engine->storage[child_slot].next_sibling) {
        UiLayoutNode& child = engine->storage[child_slot];
        if (!has_flag(child.spec.style, ui_layout_node_visible)) {
            child.visible = false;
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
        float cross_position = 0.0f;
        if (horizontal && style.vertical_alignment == UiLayoutAlignment::baseline) {
            cross_position = available.top + maximum_baseline - child.measured_baseline;
        } else {
            const UiLayoutAlignment alignment =
                horizontal ? style.vertical_alignment : style.horizontal_alignment;
            const float margin_start = horizontal ? style.margin.top : style.margin.left;
            const float margin_end = horizontal ? style.margin.bottom : style.margin.right;
            cross_position = aligned_position(horizontal ? available.top : available.left, available_cross,
                                              cross_extent, margin_start, margin_end, alignment);
        }
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
            cursor += parent.spec.style.gap + extra_gap;
        }
    }
}

struct WrapRowMetrics {
    u16 next;
    u32 count;
    float width;
    float height;
    float maximum_baseline;
};

WrapRowMetrics measure_wrap_row(const UiLayoutEngine& engine, u16 first, float available_width,
                                float gap) noexcept {
    WrapRowMetrics row{first, 0, 0.0f, 0.0f, 0.0f};
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
        if (child.spec.style.vertical_alignment == UiLayoutAlignment::baseline) {
            row.maximum_baseline =
                maximum(row.maximum_baseline, child.spec.style.margin.top + child.measured_baseline);
        }
        ++row.count;
        slot = next;
        row.next = slot;
    }
    return row;
}

void arrange_wrap_children(UiLayoutEngine* engine, UiLayoutNode& parent) noexcept {
    const UiRect available = parent.content_rectangle;
    const float available_width = ui_rect_width(available);
    UiRect inherited_clip = parent.clip_rectangle;
    if (has_flag(parent.spec.style, ui_layout_node_clip_children)) {
        inherited_clip = ui_rect_intersection(inherited_clip, available);
    }
    for (u16 slot = parent.first_child; slot != kInvalidUiLayoutSlot;
         slot = engine->storage[slot].next_sibling) {
        if (!has_flag(engine->storage[slot].spec.style, ui_layout_node_visible)) {
            engine->storage[slot].visible = false;
        }
    }

    float row_top = available.top;
    u16 row_first = parent.first_child;
    while (row_first != kInvalidUiLayoutSlot) {
        const WrapRowMetrics row =
            measure_wrap_row(*engine, row_first, available_width, parent.spec.style.gap);
        if (row.count == 0) {
            break;
        }
        const float free_space = maximum(0.0f, available_width - row.width);
        float extra_gap = 0.0f;
        float cursor = available.left +
                       justify_offset(parent.spec.style.justification, free_space, row.count, &extra_gap);
        u32 arranged_index = 0;
        for (u16 slot = row_first; slot != row.next; slot = engine->storage[slot].next_sibling) {
            UiLayoutNode& child = engine->storage[slot];
            if (!has_flag(child.spec.style, ui_layout_node_visible)) {
                continue;
            }
            const UiLayoutStyle& style = child.spec.style;
            const float width =
                clamp_extent(child.measured_size.width, style.minimum_size.width, style.maximum_size.width);
            const float height = arranged_cross_extent(child, true, row.height);
            cursor += style.margin.left;
            float top = 0.0f;
            if (style.vertical_alignment == UiLayoutAlignment::baseline) {
                top = row_top + row.maximum_baseline - child.measured_baseline;
            } else {
                top = aligned_position(row_top, row.height, height, style.margin.top, style.margin.bottom,
                                       style.vertical_alignment);
            }
            const UiRect rectangle{cursor - parent.scroll_offset.x, top - parent.scroll_offset.y,
                                   cursor - parent.scroll_offset.x + width,
                                   top - parent.scroll_offset.y + height};
            set_geometry(&child, rectangle, inherited_clip);
            cursor += width + style.margin.right;
            if (++arranged_index < row.count) {
                cursor += parent.spec.style.gap + extra_gap;
            }
        }
        row_top += row.height;
        row_first = row.next;
        if (row_first != kInvalidUiLayoutSlot) {
            row_top += parent.spec.style.gap;
        }
    }
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

bool update_measure_constraints(UiLayoutEngine* engine, UiLayoutNodeHandle* changed_node) noexcept {
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
            if (!changed && changed_node) {
                *changed_node = UiLayoutNodeHandle{static_cast<u16>(index), node.generation};
            }
            changed = true;
        }
    }
    return changed;
}

UiLayoutOverflow calculate_overflow(const UiRect& expected, const UiRect& actual) noexcept {
    return UiLayoutOverflow{
        maximum(0.0f, expected.left - actual.left),
        maximum(0.0f, expected.top - actual.top),
        maximum(0.0f, actual.right - expected.right),
        maximum(0.0f, actual.bottom - expected.bottom),
    };
}

bool has_overflow(const UiLayoutOverflow& overflow) noexcept {
    return overflow.left > kLayoutEpsilon || overflow.top > kLayoutEpsilon ||
           overflow.right > kLayoutEpsilon || overflow.bottom > kLayoutEpsilon;
}

UiLayoutNodeHandle handle_for(const UiLayoutNode& node, u32 slot) noexcept {
    return UiLayoutNodeHandle{static_cast<u16>(slot), node.generation};
}

void push_diagnostic(UiLayoutValidationResult* result, Slice<UiLayoutDiagnostic> output,
                     const UiLayoutDiagnostic& diagnostic) noexcept {
    if (result->count < output.count && output.data) {
        output[result->count] = diagnostic;
        ++result->count;
    } else {
        ++result->dropped;
    }
}

bool rectangles_intersect(const UiRect& left, const UiRect& right) noexcept {
    return left.left < right.right - kLayoutEpsilon && left.right > right.left + kLayoutEpsilon &&
           left.top < right.bottom - kLayoutEpsilon && left.bottom > right.top + kLayoutEpsilon;
}

bool rectangles_match(const UiRect& left, const UiRect& right) noexcept {
    return absolute(left.left - right.left) <= kLayoutEpsilon &&
           absolute(left.top - right.top) <= kLayoutEpsilon &&
           absolute(left.right - right.right) <= kLayoutEpsilon &&
           absolute(left.bottom - right.bottom) <= kLayoutEpsilon;
}

UiRect expected_wrap_rectangle(const UiLayoutEngine& engine, const UiLayoutNode& parent,
                               const UiLayoutNode& target) noexcept {
    const UiRect available = parent.content_rectangle;
    const float available_width = ui_rect_width(available);
    float row_top = available.top;
    u16 row_first = parent.first_child;
    while (row_first != kInvalidUiLayoutSlot) {
        const WrapRowMetrics row =
            measure_wrap_row(engine, row_first, available_width, parent.spec.style.gap);
        if (row.count == 0) {
            break;
        }
        const float free_space = maximum(0.0f, available_width - row.width);
        float extra_gap = 0.0f;
        float cursor = available.left +
                       justify_offset(parent.spec.style.justification, free_space, row.count, &extra_gap);
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
            float top = 0.0f;
            if (style.vertical_alignment == UiLayoutAlignment::baseline) {
                top = row_top + row.maximum_baseline - child.measured_baseline;
            } else {
                top = aligned_position(row_top, row.height, height, style.margin.top, style.margin.bottom,
                                       style.vertical_alignment);
            }
            if (&child == &target) {
                return UiRect{cursor - parent.scroll_offset.x, top - parent.scroll_offset.y,
                              cursor - parent.scroll_offset.x + width,
                              top - parent.scroll_offset.y + height};
            }
            cursor += width + style.margin.right;
            if (++arranged_index < row.count) {
                cursor += parent.spec.style.gap + extra_gap;
            }
        }
        row_top += row.height;
        row_first = row.next;
        if (row_first != kInvalidUiLayoutSlot) {
            row_top += parent.spec.style.gap;
        }
    }
    return target.rectangle;
}

UiRect expected_alignment_rectangle(const UiLayoutEngine& engine, const UiLayoutNode& parent,
                                    const UiLayoutNode& child) noexcept {
    const UiRect available = parent.content_rectangle;
    const UiLayoutStyle& style = child.spec.style;
    if (parent.spec.style.flow == UiLayoutFlow::horizontal_wrap) {
        return expected_wrap_rectangle(engine, parent, child);
    }
    if (parent.spec.style.flow == UiLayoutFlow::overlay) {
        return overlay_child_rectangle(parent, child);
    }

    const bool horizontal = parent.spec.style.flow == UiLayoutFlow::horizontal;
    const float available_cross = horizontal ? ui_rect_height(available) : ui_rect_width(available);
    const float extent = arranged_cross_extent(child, horizontal, available_cross);
    const float margin_start = horizontal ? style.margin.top : style.margin.left;
    const float margin_end = horizontal ? style.margin.bottom : style.margin.right;
    const UiLayoutAlignment alignment = horizontal ? style.vertical_alignment : style.horizontal_alignment;
    const float scroll_cross = horizontal ? parent.scroll_offset.y : parent.scroll_offset.x;
    float position = aligned_position(horizontal ? available.top : available.left, available_cross, extent,
                                      margin_start, margin_end, alignment) -
                     scroll_cross;
    if (horizontal && alignment == UiLayoutAlignment::baseline) {
        float maximum_baseline = 0.0f;
        for (u16 slot = parent.first_child; slot != kInvalidUiLayoutSlot;
             slot = engine.storage[slot].next_sibling) {
            const UiLayoutNode& peer = engine.storage[slot];
            if (peer.visible && peer.spec.style.vertical_alignment == UiLayoutAlignment::baseline) {
                maximum_baseline =
                    maximum(maximum_baseline, peer.spec.style.margin.top + peer.measured_baseline);
            }
        }
        position = available.top + maximum_baseline - child.measured_baseline - scroll_cross;
    }
    UiRect expected = child.rectangle;
    if (horizontal) {
        expected.top = position;
        expected.bottom = position + extent;
    } else {
        expected.left = position;
        expected.right = position + extent;
    }
    return expected;
}

bool layout_node_is_ancestor(const UiLayoutEngine& engine, u16 ancestor, u16 node) noexcept {
    if (ancestor >= engine.count || node >= engine.count || ancestor == node) {
        return false;
    }
    u16 cursor = engine.storage[node].parent;
    while (cursor != kInvalidUiLayoutSlot) {
        if (cursor == ancestor) {
            return true;
        }
        if (cursor >= engine.count) {
            return false;
        }
        cursor = engine.storage[cursor].parent;
    }
    return false;
}

} // namespace

bool ui_layout_engine_initialize(UiLayoutEngine* engine, Slice<UiLayoutNode> storage) noexcept {
    if (!engine || !storage.data || storage.count == 0 || storage.count > kInvalidUiLayoutSlot) {
        return false;
    }
    *engine = UiLayoutEngine{};
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
    engine->unstable_node = ui_layout_invalid_handle();
    for (u32 index = 0; index < engine->count; ++index) {
        engine->storage[index].measure_constraint = UiSize{};
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
    const u16 depth = root ? 0 : static_cast<u16>(parent_node->depth + 1);
    if (depth >= kMaximumUiLayoutDepth) {
        ++engine->rejected;
        return UiLayoutResult{UiLayoutError::depth, engine->count, engine->revision};
    }
    const u16 slot = static_cast<u16>(engine->count++);
    UiLayoutNode& node = engine->storage[slot];
    node = UiLayoutNode{};
    node.spec = specification;
    node.parent = root ? kInvalidUiLayoutSlot : parent.slot;
    node.first_child = kInvalidUiLayoutSlot;
    node.last_child = kInvalidUiLayoutSlot;
    node.next_sibling = kInvalidUiLayoutSlot;
    node.generation = engine->generation;
    node.depth = depth;
    node.measured_intrinsic_size = specification.intrinsic_size;
    node.measured_intrinsic_baseline = specification.intrinsic_baseline;
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

UiLayoutResult ui_layout_engine_layout(UiLayoutEngine* engine, UiSize viewport) noexcept {
    return ui_layout_engine_layout_rectangle(engine, UiRect{0.0f, 0.0f, viewport.width, viewport.height});
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
    engine->unstable_node = ui_layout_invalid_handle();
    for (u32 pass = 0; pass < kMaximumUiLayoutMeasurePasses; ++pass) {
        const UiLayoutError measured = measure_intrinsic_content(engine, viewport);
        if (measured != UiLayoutError::none) {
            ++engine->rejected;
            return UiLayoutResult{measured, engine->count, engine->revision};
        }
        for (u32 index = 0; index < engine->count; ++index) {
            engine->storage[index].visible = false;
            engine->storage[index].rectangle = UiRect{};
            engine->storage[index].content_rectangle = UiRect{};
            engine->storage[index].clip_rectangle = UiRect{};
        }
        for (u32 reverse = engine->count; reverse != 0; --reverse) {
            measure_node(engine, reverse - 1);
        }
        set_geometry(&root, viewport_rectangle, viewport_rectangle);
        for (u32 index = 0; index < engine->count; ++index) {
            UiLayoutNode& node = engine->storage[index];
            if (node.visible) {
                arrange_children(engine, node);
            }
        }
        ++engine->measure_passes;
        UiLayoutNodeHandle changed = ui_layout_invalid_handle();
        if (!update_measure_constraints(engine, &changed)) {
            engine->unstable_node = ui_layout_invalid_handle();
            ++engine->revision;
            return UiLayoutResult{UiLayoutError::none, engine->count, engine->revision};
        }
        engine->unstable_node = changed;
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

UiLayoutValidationResult ui_layout_engine_validate(const UiLayoutEngine& engine,
                                                   Slice<UiLayoutDiagnostic> output) noexcept {
    UiLayoutValidationResult result{};
    if (!engine.storage.data || engine.count == 0 || (!output.data && output.count != 0)) {
        result.dropped = 1;
        return result;
    }
    if (engine.unstable_node.generation != 0 && engine.unstable_node.slot < engine.count) {
        const UiLayoutNode& unstable = engine.storage[engine.unstable_node.slot];
        push_diagnostic(&result, output,
                        UiLayoutDiagnostic{UiLayoutDiagnosticKind::unstable_measurement,
                                           engine.unstable_node, ui_layout_invalid_handle(),
                                           UiRect{0.0f, 0.0f, unstable.measure_constraint.width,
                                                  unstable.measure_constraint.height},
                                           UiRect{0.0f, 0.0f, unstable.measured_intrinsic_size.width,
                                                  unstable.measured_intrinsic_size.height},
                                           UiLayoutOverflow{}});
    }
    for (u32 index = 0; index < engine.count; ++index) {
        const UiLayoutNode& node = engine.storage[index];
        if (node.generation != engine.generation || (index == 0 && node.parent != kInvalidUiLayoutSlot) ||
            (index != 0 && (node.parent >= index || node.depth != engine.storage[node.parent].depth + 1))) {
            push_diagnostic(&result, output,
                            UiLayoutDiagnostic{UiLayoutDiagnosticKind::invalid_hierarchy,
                                               handle_for(node, index), ui_layout_invalid_handle(),
                                               UiRect{}, node.rectangle, UiLayoutOverflow{}});
            continue;
        }
        if (!node.visible) {
            continue;
        }
        if (!ui_rect_valid_or_empty(node.rectangle)) {
            push_diagnostic(&result, output,
                            UiLayoutDiagnostic{UiLayoutDiagnosticKind::invalid_extent,
                                               handle_for(node, index), ui_layout_invalid_handle(),
                                               UiRect{}, node.rectangle, UiLayoutOverflow{}});
            continue;
        }
        if (!ui_rect_has_area(node.rectangle) &&
            !has_flag(node.spec.style, ui_layout_node_allow_zero_size)) {
            push_diagnostic(&result, output,
                            UiLayoutDiagnostic{UiLayoutDiagnosticKind::unexpected_zero_size,
                                               handle_for(node, index), ui_layout_invalid_handle(),
                                               UiRect{}, node.rectangle, UiLayoutOverflow{}});
        }
        if (!ui_coordinate_valid(node.clip_rectangle.left) ||
            !ui_coordinate_valid(node.clip_rectangle.top) ||
            !ui_coordinate_valid(node.clip_rectangle.right) ||
            !ui_coordinate_valid(node.clip_rectangle.bottom) ||
            node.clip_rectangle.right < node.clip_rectangle.left ||
            node.clip_rectangle.bottom < node.clip_rectangle.top) {
            push_diagnostic(&result, output,
                            UiLayoutDiagnostic{UiLayoutDiagnosticKind::invalid_clip,
                                               handle_for(node, index), ui_layout_invalid_handle(),
                                               UiRect{}, node.clip_rectangle, UiLayoutOverflow{}});
        }
        if (index != 0) {
            const UiLayoutNode& parent = engine.storage[node.parent];
            const bool scrolling = has_flag(parent.spec.style, ui_layout_node_scroll_horizontal) ||
                                   has_flag(parent.spec.style, ui_layout_node_scroll_vertical);
            const UiLayoutOverflow overflow = calculate_overflow(parent.content_rectangle, node.rectangle);
            if (has_overflow(overflow) && !scrolling &&
                !has_flag(node.spec.style, ui_layout_node_allow_overflow)) {
                push_diagnostic(&result, output,
                                UiLayoutDiagnostic{UiLayoutDiagnosticKind::child_overflow,
                                                   handle_for(node, index), handle_for(parent, node.parent),
                                                   parent.content_rectangle, node.rectangle, overflow});
            }
            if (scrolling && !has_flag(parent.spec.style, ui_layout_node_clip_children)) {
                push_diagnostic(&result, output,
                                UiLayoutDiagnostic{UiLayoutDiagnosticKind::invalid_clip,
                                                   handle_for(parent, node.parent), handle_for(node, index),
                                                   parent.content_rectangle, node.clip_rectangle,
                                                   UiLayoutOverflow{}});
            }
            const UiRect expected = expected_alignment_rectangle(engine, parent, node);
            if (!rectangles_match(expected, node.rectangle)) {
                push_diagnostic(&result, output,
                                UiLayoutDiagnostic{UiLayoutDiagnosticKind::invalid_alignment,
                                                   handle_for(node, index), handle_for(parent, node.parent),
                                                   expected, node.rectangle, UiLayoutOverflow{}});
            }
        }
        if (has_flag(node.spec.style, ui_layout_node_text_content)) {
            const UiRect& content = node.content_rectangle;
            const UiRect intrinsic{content.left, content.top,
                                   content.left + node.measured_intrinsic_size.width,
                                   content.top + node.measured_intrinsic_size.height};
            const UiLayoutOverflow overflow = calculate_overflow(content, intrinsic);
            if (has_overflow(overflow)) {
                push_diagnostic(&result, output,
                                UiLayoutDiagnostic{UiLayoutDiagnosticKind::text_overflow,
                                                   handle_for(node, index), ui_layout_invalid_handle(),
                                                   content, intrinsic, overflow});
            }
        }
        if ((has_flag(node.spec.style, ui_layout_node_scroll_horizontal) ||
             has_flag(node.spec.style, ui_layout_node_scroll_vertical)) &&
            !has_flag(node.spec.style, ui_layout_node_clip_children)) {
            push_diagnostic(&result, output,
                            UiLayoutDiagnostic{UiLayoutDiagnosticKind::invalid_clip,
                                               handle_for(node, index), ui_layout_invalid_handle(),
                                               node.content_rectangle, node.clip_rectangle,
                                               UiLayoutOverflow{}});
        }
    }
    for (u32 parent_index = 0; parent_index < engine.count; ++parent_index) {
        const UiLayoutNode& parent = engine.storage[parent_index];
        if (!parent.visible || parent.spec.style.flow == UiLayoutFlow::overlay ||
            has_flag(parent.spec.style, ui_layout_node_allow_overlap)) {
            continue;
        }
        for (u16 left_slot = parent.first_child; left_slot != kInvalidUiLayoutSlot;
             left_slot = engine.storage[left_slot].next_sibling) {
            const UiLayoutNode& left = engine.storage[left_slot];
            if (!left.visible || has_flag(left.spec.style, ui_layout_node_allow_overlap)) {
                continue;
            }
            if (parent.spec.style.flow == UiLayoutFlow::horizontal &&
                left.spec.style.vertical_alignment == UiLayoutAlignment::baseline) {
                const float expected = left.rectangle.top + left.measured_baseline;
                for (u16 peer_slot = left.next_sibling; peer_slot != kInvalidUiLayoutSlot;
                     peer_slot = engine.storage[peer_slot].next_sibling) {
                    const UiLayoutNode& peer = engine.storage[peer_slot];
                    if (peer.visible && peer.spec.style.vertical_alignment == UiLayoutAlignment::baseline &&
                        absolute(expected - (peer.rectangle.top + peer.measured_baseline)) >
                            kLayoutEpsilon) {
                        push_diagnostic(&result, output,
                                        UiLayoutDiagnostic{UiLayoutDiagnosticKind::invalid_baseline,
                                                           handle_for(left, left_slot),
                                                           handle_for(peer, peer_slot), left.rectangle,
                                                           peer.rectangle, UiLayoutOverflow{}});
                    }
                }
            }
            for (u16 right_slot = left.next_sibling; right_slot != kInvalidUiLayoutSlot;
                 right_slot = engine.storage[right_slot].next_sibling) {
                const UiLayoutNode& right = engine.storage[right_slot];
                if (right.visible && !has_flag(right.spec.style, ui_layout_node_allow_overlap) &&
                    rectangles_intersect(left.rectangle, right.rectangle)) {
                    const UiRect collision = ui_rect_intersection(left.rectangle, right.rectangle);
                    push_diagnostic(&result, output,
                                    UiLayoutDiagnostic{UiLayoutDiagnosticKind::sibling_collision,
                                                       handle_for(left, left_slot),
                                                       handle_for(right, right_slot), UiRect{}, collision,
                                                       UiLayoutOverflow{}});
                }
            }
        }
    }
    return result;
}

UiLayoutValidationResult ui_layout_engine_validate_content_bounds(
    const UiLayoutEngine& engine, ConstSlice<UiLayoutContentBounds> content,
    Slice<UiLayoutDiagnostic> output) noexcept {
    UiLayoutValidationResult result{};
    if (!engine.storage.data || engine.count == 0 || (!content.data && content.count != 0) ||
        (!output.data && output.count != 0)) {
        result.dropped = 1;
        return result;
    }
    for (u32 index = 0; index < content.count; ++index) {
        const UiLayoutContentBounds& bounds = content[index];
        const UiLayoutNode* owner = ui_layout_engine_resolve(engine, bounds.owner);
        if (!owner) {
            push_diagnostic(&result, output,
                            UiLayoutDiagnostic{UiLayoutDiagnosticKind::invalid_content_owner, bounds.owner,
                                               ui_layout_invalid_handle(), UiRect{}, bounds.rectangle,
                                               UiLayoutOverflow{}});
            continue;
        }
        if (!ui_rect_valid(bounds.rectangle)) {
            push_diagnostic(&result, output,
                            UiLayoutDiagnostic{UiLayoutDiagnosticKind::invalid_extent, bounds.owner,
                                               ui_layout_invalid_handle(), UiRect{}, bounds.rectangle,
                                               UiLayoutOverflow{}});
            continue;
        }
        const UiRect expected =
            bounds.kind == UiLayoutContentKind::control ? owner->rectangle : owner->content_rectangle;
        const UiRect tolerated_content{
            expected.left - kRenderedContentTolerance, expected.top - kRenderedContentTolerance,
            expected.right + kRenderedContentTolerance, expected.bottom + kRenderedContentTolerance};
        const UiLayoutOverflow overflow = calculate_overflow(tolerated_content, bounds.rectangle);
        if (has_overflow(overflow) && (bounds.flags & ui_layout_content_bounds_allow_overflow) == 0 &&
            !has_flag(owner->spec.style, ui_layout_node_allow_overflow)) {
            push_diagnostic(&result, output,
                            UiLayoutDiagnostic{bounds.kind == UiLayoutContentKind::text
                                                   ? UiLayoutDiagnosticKind::text_overflow
                                                   : UiLayoutDiagnosticKind::content_overflow,
                                               bounds.owner, ui_layout_invalid_handle(), tolerated_content,
                                               bounds.rectangle, overflow});
        }
    }
    for (u32 left_index = 0; left_index < content.count; ++left_index) {
        const UiLayoutContentBounds& left = content[left_index];
        if ((left.flags & ui_layout_content_bounds_collision) == 0 ||
            (left.flags & ui_layout_content_bounds_allow_overlap) != 0) {
            continue;
        }
        const UiLayoutNode* left_owner = ui_layout_engine_resolve(engine, left.owner);
        if (!left_owner || !ui_rect_valid_or_empty(left.rectangle)) {
            continue;
        }
        for (u32 right_index = left_index + 1; right_index < content.count; ++right_index) {
            const UiLayoutContentBounds& right = content[right_index];
            if ((right.flags & ui_layout_content_bounds_collision) == 0 ||
                (right.flags & ui_layout_content_bounds_allow_overlap) != 0) {
                continue;
            }
            const UiLayoutNode* right_owner = ui_layout_engine_resolve(engine, right.owner);
            if (!right_owner || !ui_rect_valid_or_empty(right.rectangle) ||
                has_flag(left_owner->spec.style, ui_layout_node_allow_overlap) ||
                has_flag(right_owner->spec.style, ui_layout_node_allow_overlap) ||
                layout_node_is_ancestor(engine, left.owner.slot, right.owner.slot) ||
                layout_node_is_ancestor(engine, right.owner.slot, left.owner.slot)) {
                continue;
            }
            const UiRect visible_left = ui_rect_intersection(left.rectangle, left_owner->clip_rectangle);
            const UiRect visible_right = ui_rect_intersection(right.rectangle, right_owner->clip_rectangle);
            if (!rectangles_intersect(visible_left, visible_right)) {
                continue;
            }
            const UiRect collision = ui_rect_intersection(visible_left, visible_right);
            push_diagnostic(&result, output,
                            UiLayoutDiagnostic{UiLayoutDiagnosticKind::content_collision, left.owner,
                                               right.owner, UiRect{}, collision, UiLayoutOverflow{}});
        }
    }
    return result;
}

} // namespace maicord
