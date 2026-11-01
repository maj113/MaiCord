#pragma once

// Layout invariants for tests and benchmarks: structure, clipping, overflow and
// collisions of a laid-out UiLayoutEngine, and rendered content bounds against
// their owners. The shipping client never validates; it only lays out.
#include "maicord/ui/layout/layout_engine.hpp"

namespace maicord {

enum class UiLayoutDiagnosticKind : u8 {
    invalid_hierarchy,
    invalid_extent,
    unexpected_zero_size,
    child_overflow,
    sibling_collision,
    invalid_clip,
    text_overflow,
    invalid_baseline,
    invalid_content_owner,
    content_overflow,
    content_collision,
};

enum class UiLayoutContentKind : u8 {
    text,
    icon,
    media,
    control,
    decoration,
};

enum UiLayoutContentBoundsFlag : u16 {
    ui_layout_content_bounds_collision = 1u << 0,
    ui_layout_content_bounds_allow_overflow = 1u << 1,
    ui_layout_content_bounds_allow_overlap = 1u << 2,
};

struct UiLayoutContentBounds {
    UiLayoutNodeHandle owner;
    UiRect rectangle;
    UiLayoutContentKind kind;
    u16 flags;
};

struct UiLayoutOverflow {
    float left;
    float top;
    float right;
    float bottom;
};

struct UiLayoutDiagnostic {
    UiLayoutDiagnosticKind kind;
    UiLayoutNodeHandle node;
    UiLayoutNodeHandle related;
    UiRect expected;
    UiRect actual;
    UiLayoutOverflow overflow;
};

struct UiLayoutValidationResult {
    u32 count;
    u32 dropped;

    constexpr bool clean() const noexcept {
        return count == 0 && dropped == 0;
    }
};

namespace layout_validation_detail {

inline constexpr float kEpsilon = 0.01f;
inline constexpr float kRenderedContentTolerance = 2.5f;

inline bool has_flag(const UiLayoutStyle& style, UiLayoutNodeFlag flag) noexcept {
    return (style.flags & static_cast<u16>(flag)) != 0;
}

inline UiLayoutOverflow calculate_overflow(const UiRect& expected, const UiRect& actual) noexcept {
    return UiLayoutOverflow{
        maximum(0.0f, expected.left - actual.left),
        maximum(0.0f, expected.top - actual.top),
        maximum(0.0f, actual.right - expected.right),
        maximum(0.0f, actual.bottom - expected.bottom),
    };
}

inline bool has_overflow(const UiLayoutOverflow& overflow) noexcept {
    return overflow.left > kEpsilon || overflow.top > kEpsilon || overflow.right > kEpsilon ||
           overflow.bottom > kEpsilon;
}

inline UiLayoutNodeHandle handle_for(const UiLayoutNode& node, u32 slot) noexcept {
    return UiLayoutNodeHandle{static_cast<u16>(slot), node.generation};
}

inline void push_diagnostic(UiLayoutValidationResult* result, Slice<UiLayoutDiagnostic> output,
                            const UiLayoutDiagnostic& diagnostic) noexcept {
    if (result->count < output.count && output.data) {
        output[result->count] = diagnostic;
        ++result->count;
    } else {
        ++result->dropped;
    }
}

inline bool rectangles_intersect(const UiRect& left, const UiRect& right) noexcept {
    return left.left < right.right - kEpsilon && left.right > right.left + kEpsilon &&
           left.top < right.bottom - kEpsilon && left.bottom > right.top + kEpsilon;
}

inline bool is_ancestor(const UiLayoutEngine& engine, u16 ancestor, u16 node) noexcept {
    if (ancestor >= engine.count || node >= engine.count || ancestor == node) {
        return false;
    }
    for (u16 cursor = engine.storage[node].parent; cursor != kInvalidUiLayoutSlot;
         cursor = engine.storage[cursor].parent) {
        if (cursor == ancestor) {
            return true;
        }
        if (cursor >= engine.count) {
            return false;
        }
    }
    return false;
}

} // namespace layout_validation_detail

inline UiLayoutValidationResult ui_layout_engine_validate(const UiLayoutEngine& engine,
                                                          Slice<UiLayoutDiagnostic> output) noexcept {
    using namespace layout_validation_detail;
    UiLayoutValidationResult result{};
    if (!engine.storage.data || engine.count == 0 || (!output.data && output.count != 0)) {
        result.dropped = 1;
        return result;
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
        }
        if (has_flag(node.spec.style, ui_layout_node_text_content)) {
            const UiRect& content = node.content_rectangle;
            const UiRect intrinsic{content.left, content.top,
                                   content.left + node.spec.intrinsic_size.width,
                                   content.top + node.spec.intrinsic_size.height};
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
                        absolute_value(expected - (peer.rectangle.top + peer.measured_baseline)) > kEpsilon) {
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

inline UiLayoutValidationResult ui_layout_engine_validate_content_bounds(
    const UiLayoutEngine& engine, ConstSlice<UiLayoutContentBounds> content,
    Slice<UiLayoutDiagnostic> output) noexcept {
    using namespace layout_validation_detail;
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
                is_ancestor(engine, left.owner.slot, right.owner.slot) ||
                is_ancestor(engine, right.owner.slot, left.owner.slot)) {
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
