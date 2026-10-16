#include "maicord/ui/scroll_host.hpp"
#include "maicord/ui/widget_scroll.hpp"

namespace maicord {
namespace {
bool finite(double value) noexcept {
    return value == value && value > -1e15 && value < 1e15;
}
bool valid(const UiScrollRange& range) noexcept {
    return ui_rect_has_area(range.viewport) && finite(range.offset) && finite(range.maximum) &&
           range.maximum >= 0 && range.offset >= 0 && finite(range.line) && range.line > 0 &&
           finite(range.units_per_dip) && range.units_per_dip > 0 && finite(range.origin);
}
void rebase_entry(UiScrollEntry* entry, const UiScrollRange& range) noexcept {
    const double shift = range.origin - entry->origin;
    entry->position += shift;
    entry->reported_position += shift;
    entry->origin = range.origin;
}
UiScrollEntry* acquire_entry(UiScrollHost* host, UiScrollBinding binding,
                             const UiScrollRange& range) noexcept {
    UiScrollEntry* entry = nullptr;
    UiScrollEntry* free = nullptr;
    for (auto& candidate : host->entries) {
        if (candidate.binding.read && candidate.binding.context == binding.context &&
            candidate.binding.key == binding.key && candidate.binding.read == binding.read &&
            candidate.binding.write == binding.write)
            entry = &candidate;
        if ((!candidate.animation.active || host->immediate) &&
            (!free || (!candidate.binding.read && free->binding.read)))
            free = &candidate;
    }
    if (!entry) {
        if (!free)
            return nullptr;
        entry = free;
        *entry = UiScrollEntry{};
        entry->binding = binding;
        entry->position = entry->reported_position = range.offset;
        entry->origin = range.origin;
        entry->generation = range.generation;
    } else {
        rebase_entry(entry, range);
        if (range.generation != entry->generation || range.offset != entry->reported_position) {
            ui_smooth_scroll_cancel(&entry->animation);
            entry->position = entry->reported_position = range.offset;
            entry->generation = range.generation;
        }
    }
    return entry;
}
UiScrollDispatch write_immediate(UiScrollEntry* entry, const UiScrollRange& range,
                                 double target) noexcept {
    ui_smooth_scroll_cancel(&entry->animation);
    target = minimum(maximum(target, 0.0), range.maximum);
    bool changed = false;
    if (!entry->binding.write(entry->binding.context, entry->binding.key, target, &changed))
        return UiScrollDispatch::failed;
    entry->position = target;
    UiScrollRange reported{};
    if (!entry->binding.read(entry->binding.context, entry->binding.key, &reported)) {
        *entry = {};
        return UiScrollDispatch::handled;
    }
    if (!valid(reported)) return UiScrollDispatch::failed;
    rebase_entry(entry, reported);
    entry->reported_position = reported.offset;
    entry->generation = reported.generation;
    return UiScrollDispatch::handled;
}
UiWidgetHandle widget_handle(u64 key) noexcept {
    return UiWidgetHandle{static_cast<u16>(key), static_cast<u16>(key >> 16)};
}
bool widget_scroll_input_allowed(const UiWidgetManager& manager, UiWidgetHandle handle) noexcept {
    // Scroll owners need not be buttons with ui_widget_enabled. Explicit modal
    // blocks and stale/hidden ancestor chains still exclude their whole subtree.
    for (u32 depth = 0; depth < kMaximumUiLayoutDepth; ++depth) {
        const auto* widget = ui_widget_manager_resolve(manager, handle);
        const auto* node = ui_widget_manager_layout_node(manager, handle);
        if (!widget || !node || !node->visible || !(node->spec.style.flags & ui_layout_node_visible) ||
            (widget->flags & ui_widget_block_input)) return false;
        if (!widget->parent.generation)
            return handle.slot == 0 && widget->parent.slot == kInvalidUiWidgetSlot &&
                   node->parent == kInvalidUiLayoutSlot;
        const auto* parent = ui_widget_manager_resolve(manager, widget->parent);
        if (!parent || widget->parent.slot >= handle.slot || node->parent != parent->layout.slot) return false;
        handle = widget->parent;
    }
    return false;
}
bool widget_read(void* context, u64 key, UiScrollRange* range) noexcept {
    auto& manager = *static_cast<UiWidgetManager*>(context);
    const UiWidget* widget = ui_widget_manager_resolve(manager, widget_handle(key));
    const UiLayoutNode* node =
        widget ? ui_widget_manager_layout_node(manager, widget_handle(key)) : nullptr;
    if (!node || !node->visible || widget->kind != UiWidgetKind::scroll_view ||
        !widget_scroll_input_allowed(manager, widget_handle(key)) ||
        !(node->spec.style.flags & ui_layout_node_scroll_vertical))
        return false;
    UiWidgetScrollGeometry geometry{};
    if (!ui_widget_scroll_geometry(manager, widget_handle(key), &geometry))
        return false;
    *range = UiScrollRange{ui_rect_intersection(node->rectangle, node->clip_rectangle), geometry.offset.y,
                           geometry.maximum.y, kUiScrollLineDips, 1};
    return true;
}
bool widget_write(void* context, u64 key, double offset, bool* changed) noexcept {
    auto& manager = *static_cast<UiWidgetManager*>(context);
    UiWidget* widget = ui_widget_manager_resolve(&manager, widget_handle(key));
    const UiLayoutNode* node = ui_widget_manager_layout_node(manager, widget_handle(key));
    if (!widget || !node || manager.layout.count == 0)
        return false;
    const float next = static_cast<float>(offset);
    *changed = next != node->scroll_offset.y;
    if (!*changed)
        return true;
    const UiRect viewport = manager.layout.storage[0].rectangle;
    return ui_layout_engine_set_scroll(&manager.layout, widget->layout,
                                       UiPoint{node->scroll_offset.x, next}) &&
           ui_widget_manager_layout_rectangle(&manager, viewport).ok();
}
} // namespace

UiScrollDispatch ui_scroll_host_wheel(UiScrollHost* host, UiScrollBinding binding, UiPoint point,
                                      i32 wheel_delta, u32 lines, bool page, u64 now_us,
                                      double dips_per_pixel) noexcept {
    if (!host || !binding.read || !binding.write || !finite(dips_per_pixel) || dips_per_pixel <= 0)
        return UiScrollDispatch::failed;
    UiScrollRange range{};
    if (!binding.read(binding.context, binding.key, &range) ||
        !ui_rect_contains(range.viewport, point.x, point.y))
        return UiScrollDispatch::unhandled;
    if (!valid(range))
        return UiScrollDispatch::failed;
    const double distance =
        page ? ui_rect_height(range.viewport) * range.units_per_dip : lines * range.line;
    if (host->immediate) {
        auto* entry = acquire_entry(host, binding, range);
        if (!entry) return UiScrollDispatch::failed;
        return write_immediate(entry, range, entry->position - wheel_delta * distance / 120.0);
    }
    if (range.maximum == 0 || wheel_delta == 0 || (!page && lines == 0))
        return UiScrollDispatch::handled;
    UiScrollEntry* entry = acquire_entry(host, binding, range);
    if (!entry)
        return UiScrollDispatch::failed;
    return ui_smooth_scroll_queue(&entry->animation, -static_cast<double>(wheel_delta) * distance / 120.0, now_us,
                                  range.units_per_dip * dips_per_pixel)
               ? UiScrollDispatch::handled
               : UiScrollDispatch::failed;
}

UiScrollDispatch ui_scroll_host_reveal(UiScrollHost* host, UiScrollBinding binding, double first,
                                       double last, u64 now_us, double dips_per_pixel) noexcept {
    if (!host || !binding.read || !binding.write || !finite(first) || !finite(last) || first < 0 ||
        last < first || !finite(dips_per_pixel) || dips_per_pixel <= 0)
        return UiScrollDispatch::failed;
    UiScrollRange range{};
    if (!binding.read(binding.context, binding.key, &range))
        return UiScrollDispatch::unhandled;
    if (!valid(range))
        return UiScrollDispatch::failed;
    UiScrollEntry* entry = acquire_entry(host, binding, range);
    if (!entry)
        return UiScrollDispatch::failed;
    const double pending = entry->position + (host->immediate ? 0.0 : entry->animation.remaining);
    double target = minimum(maximum(pending, 0.0), range.maximum);
    const double height = ui_rect_height(range.viewport) * range.units_per_dip;
    if (first < target || last - first > height)
        target = first;
    else if (last > target + height)
        target = last - height;
    target = minimum(maximum(target, 0.0), range.maximum);
    if (host->immediate) return write_immediate(entry, range, target);
    // Retarget the existing curve. Repeated reveals must not accumulate the
    // same distance, restart an unchanged curve, or allocate another timer.
    return ui_smooth_scroll_queue(&entry->animation, target - pending, now_us,
                                  range.units_per_dip * dips_per_pixel)
               ? UiScrollDispatch::handled
               : UiScrollDispatch::failed;
}

UiScrollDispatch ui_scroll_host_scroll_to(UiScrollHost* host, UiScrollBinding binding, double target,
                                          u64 now_us, double dips_per_pixel) noexcept {
    if (!host || !binding.read || !binding.write || !finite(target) || !finite(dips_per_pixel) ||
        dips_per_pixel <= 0)
        return UiScrollDispatch::failed;
    UiScrollRange range{};
    if (!binding.read(binding.context, binding.key, &range))
        return UiScrollDispatch::unhandled;
    if (!valid(range))
        return UiScrollDispatch::failed;
    UiScrollEntry* entry = acquire_entry(host, binding, range);
    if (!entry)
        return UiScrollDispatch::failed;
    target = minimum(maximum(target, 0.0), range.maximum);
    if (host->immediate) return write_immediate(entry, range, target);
    const double pending = entry->position + entry->animation.remaining;
    return ui_smooth_scroll_queue(&entry->animation, target - pending, now_us,
                                  range.units_per_dip * dips_per_pixel)
               ? UiScrollDispatch::handled
               : UiScrollDispatch::failed;
}

UiScrollDispatch ui_scroll_host_wheel_widgets(UiScrollHost* host, UiWidgetManager* widgets, UiPoint point,
                                              i32 wheel_delta, u32 lines, bool page, u64 now_us,
                                              double dips_per_pixel,
                                              UiWidgetHandle virtual_owner) noexcept {
    if (!widgets)
        return UiScrollDispatch::unhandled;
    // Later descendants paint over earlier ones. Resolve the containing
    // scroll owner, not merely a hit-testable button inside that owner.
    for (u32 i = widgets->count; i != 0; --i) {
        const UiWidget& widget = widgets->storage[i - 1];
        if (virtual_owner.slot == i - 1 && virtual_owner.generation == widget.generation)
            continue;
        if (widget.kind != UiWidgetKind::scroll_view)
            continue;
        const u64 key = (static_cast<u64>(widget.generation) << 16) | (i - 1);
        const auto result = ui_scroll_host_wheel(host, {widgets, key, widget_read, widget_write}, point,
                                                 wheel_delta, lines, page, now_us, dips_per_pixel);
        if (result != UiScrollDispatch::unhandled)
            return result;
    }
    return UiScrollDispatch::unhandled;
}

bool ui_scroll_host_tick(UiScrollHost* host, u64 now_us, bool* changed) noexcept {
    if (!host || !changed)
        return false;
    *changed = false;
    if (host->immediate) {
        for (auto& entry : host->entries) ui_smooth_scroll_cancel(&entry.animation);
        return true;
    }
    for (auto& entry : host->entries) {
        if (!entry.animation.active)
            continue;
        UiScrollRange range{};
        if (!entry.binding.read(entry.binding.context, entry.binding.key, &range)) {
            ui_smooth_scroll_cancel(&entry.animation);
            continue;
        }
        if (!valid(range))
            return false;
        if (range.generation != entry.generation) {
            ui_smooth_scroll_cancel(&entry.animation);
            continue;
        }
        rebase_entry(&entry, range);
        if (range.offset > range.maximum) {
            bool clamped = false;
            if (!entry.binding.write(entry.binding.context, entry.binding.key, range.maximum, &clamped))
                return false;
            *changed = *changed || clamped;
            ui_smooth_scroll_cancel(&entry.animation);
            continue;
        }
        if (range.offset != entry.reported_position) {
            // An intentional move, not a layout-anchor coordinate translation.
            ui_smooth_scroll_cancel(&entry.animation);
            continue;
        }
        double delta = 0;
        if (!ui_smooth_scroll_step(&entry.animation, now_us, &delta))
            return false;
        entry.position = minimum(maximum(entry.position + delta, 0.0), range.maximum);
        if (delta != 0) {
            bool updated = false;
            if (!entry.binding.write(entry.binding.context, entry.binding.key, entry.position, &updated))
                return false;
            *changed = *changed || updated;
            if (!entry.binding.read(entry.binding.context, entry.binding.key, &range)) {
                ui_smooth_scroll_cancel(&entry.animation);
                continue;
            }
            if (!valid(range))
                return false;
            rebase_entry(&entry, range);
            entry.reported_position = range.offset;
        }
        // Preserve subpixel movement in double precision. An unchanged
        // float layout sample must never discard the rest of a wheel event.
        if ((entry.position <= 0 && entry.animation.remaining < 0) ||
            (entry.position >= range.maximum && entry.animation.remaining > 0))
            ui_smooth_scroll_cancel(&entry.animation);
    }
    return true;
}
bool ui_scroll_host_active(const UiScrollHost& host) noexcept {
    if (host.immediate) return false;
    for (const auto& entry : host.entries)
        if (entry.animation.active)
            return true;
    return false;
}
void ui_scroll_host_cancel(UiScrollHost* host) noexcept {
    if (host)
        for (auto& entry : host->entries)
            entry = {};
}
} // namespace maicord
