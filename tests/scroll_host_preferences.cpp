#include "maicord/ui/scroll_host.hpp"

namespace maicord {
namespace {
struct PreferenceScrollModel {
    double offset = 0;
    double maximum = 1000;
    double line = 10;
    double origin = 0;
    u64 generation = 0;
    u32 writes = 0;
    bool visible = true;
    bool integer_storage = false;
    bool reject_write = false;
};
UiScrollBinding model_binding(PreferenceScrollModel* model, u64 key = 1) noexcept {
    return {model, key,
        [](void* context, u64, UiScrollRange* range) noexcept {
            auto& value = *static_cast<PreferenceScrollModel*>(context);
            *range = {{0, 0, 100, 100}, value.offset, value.maximum, value.line, 1,
                      value.origin, value.generation};
            return value.visible;
        },
        [](void* context, u64, double offset, bool* changed) noexcept {
            auto& value = *static_cast<PreferenceScrollModel*>(context);
            if (value.reject_write) return false;
            const double next = value.integer_storage ? static_cast<double>(static_cast<i64>(offset)) : offset;
            *changed = value.offset != next;
            value.offset = next;
            ++value.writes;
            return true;
        }};
}
}

void test_scroll_host_preferences(void (*check)(bool, const char*) noexcept) noexcept {
    UiScrollHost host{};
    host.immediate = true;
    PreferenceScrollModel model{};
    const auto binding = model_binding(&model);
    auto wheel = [&](i32 delta, u32 lines = 3, bool page = false) noexcept {
        return ui_scroll_host_wheel(&host, binding, {20, 20}, delta, lines, page, 1000);
    };
    check(wheel(-120) == UiScrollDispatch::handled && model.offset == 30 && model.writes == 1 &&
          !ui_scroll_host_active(host), "scroll preference: immediate wheel synchronously changes offset without animation");
    bool changed = true;
    check(ui_scroll_host_tick(&host, 1'000'000, &changed) && !changed && model.offset == 30 && model.writes == 1,
          "scroll preference: immediate input creates no timer follow-up writes");
    check(wheel(-120, 1, true) == UiScrollDispatch::handled && model.offset == 130,
          "scroll preference: immediate page scrolling uses current viewport height");
    check(wheel(2147483647) == UiScrollDispatch::handled && model.offset == 0 &&
          wheel(-2147483647 - 1) == UiScrollDispatch::handled && model.offset == 1000,
          "scroll preference: extreme signed wheel input clamps at both content boundaries");
    model.maximum = 0;
    check(wheel(-120) == UiScrollDispatch::handled && model.offset == 0,
          "scroll preference: content shrink clamps existing position synchronously");
    model.maximum = 1000;
    check(ui_scroll_host_reveal(&host, binding, 350, 400, 2000) == UiScrollDispatch::handled && model.offset == 300 &&
          !ui_scroll_host_active(host), "scroll preference: reveal jumps to exact required visible interval");
    const u32 writes = model.writes;
    check(ui_scroll_host_reveal(&host, binding, 350, 400, 3000) == UiScrollDispatch::handled && model.offset == 300 &&
          model.writes == writes + 1 && !ui_scroll_host_active(host), "scroll preference: repeated reveal stays stationary");
    check(ui_scroll_host_reveal(&host, binding, 500, 700, 4000) == UiScrollDispatch::handled && model.offset == 500,
          "scroll preference: oversized reveal aligns its leading edge immediately");
    model.visible = false;
    check(wheel(-120) == UiScrollDispatch::unhandled && model.offset == 500,
          "scroll preference: hidden or retired owner is never written");
    model.visible = true;
    model.reject_write = true;
    check(wheel(-120) == UiScrollDispatch::failed && model.offset == 500 && !ui_scroll_host_active(host),
          "scroll preference: synchronous write errors return failure without scheduling animation");
    model.reject_write = false;

    ui_scroll_host_cancel(&host);
    model = {};
    model.line = 15;
    model.integer_storage = true;
    bool fractional = true;
    for (u32 i = 0; i < 8; ++i) fractional = fractional && wheel(-1, 1) == UiScrollDispatch::handled;
    check(fractional && model.offset == 1 && !ui_scroll_host_active(host),
          "scroll preference: eight fractional high resolution events survive quantized owner storage");
    model.offset = 20; // Intentional external navigation invalidates cached residue.
    check(wheel(-8, 1) == UiScrollDispatch::handled && model.offset == 21,
          "scroll preference: external offset change resets fractional position");
    wheel(-4, 1); // Retain a half-unit that quantized storage cannot represent.
    ++model.generation;
    check(wheel(-4, 1) == UiScrollDispatch::handled && model.offset == 21,
          "scroll preference: content generation change discards old fractional residue");
    ui_scroll_host_cancel(&host);
    model.offset = 0; model.origin = 0;
    wheel(-4, 1);
    model.offset = 10; model.origin = 10;
    check(wheel(-4, 1) == UiScrollDispatch::handled && model.offset == 11,
          "scroll preference: layout anchor translation preserves current fractional residue");
    ui_scroll_host_cancel(&host);
    model = {};
    host.immediate = false;
    check(wheel(-120) == UiScrollDispatch::handled && ui_scroll_host_active(host) && model.offset == 0,
          "scroll preference: existing smooth mode still queues its animated distance");
    host.immediate = true;
    check(wheel(-120) == UiScrollDispatch::handled && model.offset == 30 && !host.entries[0].animation.active &&
          !ui_scroll_host_active(host), "scroll preference: immediate gesture cancels affected pending curve");

    PreferenceScrollModel owners[kUiScrollHostCapacity + 2]{};
    ui_scroll_host_cancel(&host);
    bool bounded = true;
    for (u32 i = 0; i < kUiScrollHostCapacity + 2; ++i) {
        bounded = bounded && ui_scroll_host_wheel(&host, model_binding(&owners[i], i + 1), {20, 20},
                                                   -120, 1, false, 1000) == UiScrollDispatch::handled &&
                             owners[i].offset == 10;
    }
    check(bounded && !ui_scroll_host_active(host), "scroll preference: idle immediate cache evicts within bounded capacity");

    UiWidget widgets[5]{};
    UiLayoutNode nodes[5]{};
    UiWidgetManager manager{};
    check(ui_widget_manager_initialize(&manager, {widgets, 5}, {nodes, 5}),
          "scroll preference: retained widget fixture initializes");
    auto style = ui_layout_style();
    style.width = style.height = ui_layout_fill();
    style.flow = UiLayoutFlow::vertical;
    style.flags |= ui_layout_node_scroll_vertical | ui_layout_node_clip_children;
    const auto outer = ui_widget_manager_add(&manager, ui_widget_invalid_handle(),
        {1, UiWidgetKind::scroll_view, ui_widget_none, {{}, style}});
    style.width = style.height = ui_layout_fixed(50);
    const auto inner = ui_widget_manager_add(&manager, outer.handle,
        {2, UiWidgetKind::scroll_view, ui_widget_none, {{}, style}});
    style = ui_layout_style();
    style.width = ui_layout_fixed(50); style.height = ui_layout_fixed(300);
    const auto content = ui_widget_manager_add(&manager, inner.handle,
        {3, UiWidgetKind::container, ui_widget_none, {{}, style}});
    const auto below = ui_widget_manager_add(&manager, outer.handle,
        {4, UiWidgetKind::container, ui_widget_none, {{}, style}});
    const bool built = outer.ok() && inner.ok() && content.ok() && below.ok() &&
                       ui_widget_manager_layout(&manager, {100, 100}).ok();
    check(built, "scroll preference: nested retained scroll owners arrange");
    if (!built) return;
    ui_scroll_host_cancel(&host);
    check(ui_scroll_host_wheel_widgets(&host, &manager, {20, 20}, -120, 1, false, 1000) == UiScrollDispatch::handled &&
          nodes[inner.handle.slot].scroll_offset.y == 32 && nodes[outer.handle.slot].scroll_offset.y == 0,
          "scroll preference: deepest eligible retained owner gets immediate input");
    check(ui_widget_manager_block_input(&manager, outer.handle, true) &&
          ui_scroll_host_wheel_widgets(&host, &manager, {20, 20}, -120, 1, false, 2000) == UiScrollDispatch::unhandled &&
          nodes[inner.handle.slot].scroll_offset.y == 32 && nodes[outer.handle.slot].scroll_offset.y == 0,
          "scroll preference: modal background subtree block prevents inner and outer scrolling");
    ui_widget_manager_block_input(&manager, outer.handle, false);
    check(ui_scroll_host_wheel_widgets(&host, &manager, {20, 20}, -120, 1, false, 3000, 1, inner.handle) ==
          UiScrollDispatch::handled && nodes[outer.handle.slot].scroll_offset.y == 32 &&
          nodes[inner.handle.slot].scroll_offset.y == 32,
          "scroll preference: virtual owner exclusion preserves retained fallback routing");
    ui_widget_manager_reset(&manager);
    check(ui_scroll_host_wheel_widgets(&host, &manager, {20, 20}, -120, 1, false, 4000) == UiScrollDispatch::unhandled,
          "scroll preference: reset widget generation cannot route stale owner input");
}
} // namespace maicord
