#include "maicord/ui/widget_scroll.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

float reveal_offset(float first, float last, float start, float end, float offset, float limit) noexcept {
    // An oversized target already spanning the viewport must not oscillate.
    float delta = 0;
    if (first < start && last > end) return minimum(offset, limit);
    const bool small = last - first <= end - start;
    if (first < start) delta = small ? first - start : last - end;
    else if (last > end) delta = small ? last - end : first - start;
    return minimum(maximum(offset + delta, 0.0f), limit);
}

bool contains(UiRect outer, UiRect inner) noexcept {
    return ui_rect_has_area(inner) && inner.left >= outer.left && inner.top >= outer.top &&
        inner.right <= outer.right && inner.bottom <= outer.bottom;
}

} // namespace

bool ui_scroll_reveal_offset(float first, float last, float viewport_first, float viewport_last,
    float offset, float maximum_offset, float* result) noexcept {
    if (!result || !ui_coordinate_valid(first) || !ui_coordinate_valid(last) || first > last ||
        !ui_coordinate_valid(viewport_first) || !ui_coordinate_valid(viewport_last) || viewport_first >= viewport_last ||
        !ui_extent_valid(offset) || !ui_extent_valid(maximum_offset)) return false;
    *result = reveal_offset(first, last, viewport_first, viewport_last, offset, maximum_offset);
    return true;
}

bool ui_widget_scroll_geometry(const UiWidgetManager& manager, UiWidgetHandle owner,
                               UiWidgetScrollGeometry* geometry) noexcept {
    if (!geometry) return false;
    reset_value(geometry);
    const auto* widget = ui_widget_manager_resolve(manager, owner);
    const auto* root = ui_widget_manager_layout_node(manager, owner);
    if (!widget || !root || !root->visible || !(root->spec.style.flags & ui_layout_node_visible) ||
        !ui_rect_valid_or_empty(root->content_rectangle)) return false;
    const u16 axes = root->spec.style.flags & (ui_layout_node_scroll_horizontal | ui_layout_node_scroll_vertical);
    if (!axes) return false;

    // Walk exposed descendants, clipping only at intermediate containers.
    // The owner's own clip and ancestor clips must not erase offscreen content
    // from its logical range. Fixed-size unclipped children may overflow.
    struct Frame { u16 parent; u16 next; };
    Frame stack[kMaximumUiLayoutDepth];
    u32 depth = 1;
    stack[0] = {widget->layout.slot, root->first_child};
    float right = root->content_rectangle.right;
    float bottom = root->content_rectangle.bottom;
    u32 visited = 0;
    while (depth) {
        auto& frame = stack[depth - 1];
        if (frame.next == kInvalidUiLayoutSlot) { --depth; continue; }
        const u16 slot = frame.next;
        if (++visited > manager.layout.count || slot >= manager.layout.count || slot <= frame.parent) return false;
        const auto& node = manager.layout.storage[slot];
        if (node.parent != frame.parent || node.generation != root->generation ||
            (node.next_sibling != kInvalidUiLayoutSlot && node.next_sibling <= slot)) return false;
        frame.next = node.next_sibling;
        if (!node.visible || !(node.spec.style.flags & ui_layout_node_visible)) continue;
        if (!ui_rect_valid_or_empty(node.rectangle) || !ui_rect_valid_or_empty(node.content_rectangle)) return false;
        right = maximum(right, node.rectangle.right + node.spec.style.margin.right + root->scroll_offset.x);
        bottom = maximum(bottom, node.rectangle.bottom + node.spec.style.margin.bottom + root->scroll_offset.y);
        // A clipping child's complete border/margin extent is already included;
        // its descendants cannot enlarge this owner's range.
        if (node.first_child != kInvalidUiLayoutSlot && !(node.spec.style.flags & ui_layout_node_clip_children)) {
            if (depth == kMaximumUiLayoutDepth) return false;
            stack[depth++] = {slot, node.first_child};
        }
    }
    const UiPoint limit{axes & ui_layout_node_scroll_horizontal ? maximum(0.0f, right - root->content_rectangle.right) : 0,
        axes & ui_layout_node_scroll_vertical ? maximum(0.0f, bottom - root->content_rectangle.bottom) : 0};
    if (!ui_extent_valid(limit.x) || !ui_extent_valid(limit.y)) return false;
    *geometry = {root->content_rectangle, root->scroll_offset, limit};
    return true;
}

UiWidgetRevealResult ui_widget_reveal(UiWidgetManager* manager, UiWidgetHandle target) noexcept {
    if (!manager || !ui_widget_manager_input_enabled(*manager, target))
        return {UiLayoutError::invalid_argument, 0, false};
    const auto* target_widget = ui_widget_manager_resolve(*manager, target);
    const auto* target_node = ui_widget_manager_layout_node(*manager, target);
    if (!target_node || !ui_rect_valid(target_node->rectangle))
        return {UiLayoutError::invalid_argument, 0, false};
    struct Move { UiLayoutNodeHandle node; UiPoint offset; };
    Move moves[kMaximumUiLayoutDepth];
    u32 count = 0;
    UiRect projected = target_node->rectangle;
    UiWidgetHandle ancestor = target_widget->parent;
    while (ancestor.generation) {
        const auto* widget = ui_widget_manager_resolve(*manager, ancestor);
        const auto* node = ui_widget_manager_layout_node(*manager, ancestor);
        if (!widget || !node) return {UiLayoutError::invalid_parent, 0, false};
        const auto flags = node->spec.style.flags;
        if (flags & (ui_layout_node_scroll_horizontal | ui_layout_node_scroll_vertical)) {
            UiPoint next = node->scroll_offset;
            if (flags & ui_layout_node_scroll_horizontal)
                next.x = reveal_offset(projected.left, projected.right, node->content_rectangle.left,
                    node->content_rectangle.right, next.x, kMaximumUiCoordinate);
            if (flags & ui_layout_node_scroll_vertical)
                next.y = reveal_offset(projected.top, projected.bottom, node->content_rectangle.top,
                    node->content_rectangle.bottom, next.y, kMaximumUiCoordinate);
            // Already-visible targets need only ancestor checks, never a
            // descendant range scan or a layout pass.
            if (next.x != node->scroll_offset.x || next.y != node->scroll_offset.y) {
                UiWidgetScrollGeometry geometry{};
                if (!ui_widget_scroll_geometry(*manager, ancestor, &geometry))
                    return {UiLayoutError::invalid_argument, 0, false};
                next.x = minimum(next.x, geometry.maximum.x);
                next.y = minimum(next.y, geometry.maximum.y);
                if (next.x != geometry.offset.x || next.y != geometry.offset.y) {
                    moves[count++] = {widget->layout, next};
                    const float dx = geometry.offset.x - next.x;
                    const float dy = geometry.offset.y - next.y;
                    projected = {projected.left + dx, projected.top + dy, projected.right + dx, projected.bottom + dy};
                }
            }
        }
        if (flags & ui_layout_node_clip_children) {
            projected = ui_rect_intersection(projected, node->content_rectangle);
            if (!ui_rect_has_area(projected)) break;
        }
        ancestor = widget->parent;
    }
    if (count) {
        const UiRect viewport = manager->layout.storage[0].rectangle;
        for (u32 i = 0; i < count; ++i) {
            if (!ui_layout_engine_set_scroll(&manager->layout, moves[i].node, moves[i].offset))
                return {UiLayoutError::invalid_argument, i, false};
        }
        const auto layout = ui_widget_manager_layout_rectangle(manager, viewport);
        if (!layout.ok()) return {layout.error, count, false};
    }
    target_node = ui_widget_manager_layout_node(*manager, target);
    return {UiLayoutError::none, count, target_node && contains(target_node->clip_rectangle, target_node->rectangle)};
}

} // namespace maicord
