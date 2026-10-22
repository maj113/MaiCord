#include "maicord/ui/widget_builder.hpp"

namespace maicord {
namespace {
struct Measurement {
    u32 calls{}, key{};
};
bool measure(void* opaque, const UiLayoutMeasureRequest& request, UiSize* size, float* baseline) noexcept {
    auto& observed = *static_cast<Measurement*>(opaque);
    ++observed.calls;
    observed.key = request.key;
    if (request.key != 2 || !size || !baseline)
        return false;
    *size = {minimum(70.0f, request.constraint.width), 18.0f};
    *baseline = 13.0f;
    return true;
}
} // namespace

void test_widget_builder(void (*check)(bool, const char*) noexcept) noexcept {
    UiWidget storage[6]{};
    UiLayoutNode layout_storage[6]{};
    UiWidgetManager manager{};
    check(ui_widget_manager_initialize(&manager, {storage, 6}, {layout_storage, 6}),
          "widget builder initializes caller-owned retained storage");

    UiLayoutStyle root_style = ui_layout_style();
    root_style.width = ui_layout_fill();
    root_style.height = ui_layout_fill();
    UiLayoutStyle control_style = ui_layout_style();
    control_style.width = ui_layout_fixed(40);
    control_style.height = ui_layout_fixed(20);
    UiWidgetBuilder widgets(manager);
    const auto root = widgets.container(ui_widget_invalid_handle(), {1, utf8_literal("Root")}, root_style);
    const auto button = widgets.button(root.handle, {2, utf8_literal("Accept")}, control_style);
    const auto field = widgets.text_field(root.handle, {3, utf8_literal("Input")}, control_style, false);
    check(widgets.ok() && root.ok() && button.ok() && field.ok() &&
              ui_widget_manager_layout(&manager, {100, 100}).ok() &&
              ui_widget_manager_focus(&manager, button.handle) &&
              !ui_widget_manager_focus(&manager, field.handle),
          "widget builder shares retained layout and native focus policy without owning storage");

    const u32 retained_count = manager.count;
    const auto duplicate = widgets.button(root.handle, {2, utf8_literal("Duplicate")}, control_style);
    const u32 rejections = manager.rejected;
    const auto after_error = widgets.button(root.handle, {4, utf8_literal("After failure")}, control_style);
    check(duplicate.error == UiWidgetError::duplicate_id && after_error.error == duplicate.error &&
              widgets.error() == duplicate.error && manager.count == retained_count &&
              manager.rejected == rejections,
          "widget builder preserves its first failure and stops subsequent tree mutation");

    UiWidgetBuilder before_reset(manager);
    ui_widget_manager_reset(&manager);
    const auto stale =
        before_reset.container(ui_widget_invalid_handle(), {1, utf8_literal("Stale")}, root_style);
    check(stale.error == UiWidgetError::invalid_parent && !manager.count &&
              !ui_widget_manager_resolve(manager, button.handle),
          "widget builder rejects an owner reset and cannot revive prior handles");

    UiWidgetBuilder replacement(manager);
    const auto replacement_root =
        replacement.container(ui_widget_invalid_handle(), {1, utf8_literal("Replacement")}, root_style);
    for (u32 id = 2; id <= 6; ++id) {
        check(replacement.button(replacement_root.handle, {id, utf8_literal("Button")}, control_style).ok(),
              "widget builder admits controls within the caller storage bound");
    }
    check(!ui_widget_manager_resolve(manager, root.handle) &&
              !ui_widget_manager_resolve(manager, button.handle) &&
              ui_widget_manager_find(manager, 1).generation == replacement_root.handle.generation,
          "widget reset rejects old handles after their slots are reused");
    const auto exhausted =
        replacement.button(replacement_root.handle, {7, utf8_literal("Capacity")}, control_style);
    check(exhausted.error == UiWidgetError::capacity && manager.count == 6 &&
              ui_widget_manager_resolve(manager, replacement_root.handle),
          "widget builder refuses exhausted storage while preserving retained nodes");

    UiWidget measured_widgets[2]{};
    UiLayoutNode measured_nodes[2]{};
    UiWidgetManager measured_manager{};
    Measurement measured{};
    check(ui_widget_manager_initialize(&measured_manager, {measured_widgets, 2}, {measured_nodes, 2}) &&
              ui_widget_manager_set_measure_callback(&measured_manager, measure, &measured),
          "widget builder text binds the actual retained layout measurement callback");
    UiWidgetBuilder measured_builder(measured_manager);
    root_style.flow = UiLayoutFlow::vertical;
    const auto measured_root = measured_builder.container(ui_widget_invalid_handle(),
                                                          {1, utf8_literal("Measured root")}, root_style);
    const UiLayoutStyle ordinary_text_style = ui_layout_style();
    const auto measured_text = measured_builder.text(
        measured_root.handle, {2, utf8_literal("Measured text")}, ordinary_text_style, {1, 1});
    const auto laid_out = ui_widget_manager_layout(&measured_manager, {100, 100});
    const auto* node = ui_widget_manager_layout_node(measured_manager, measured_text.handle);
    check(measured_text.ok() && laid_out.ok() && measured.calls && measured.key == 2 && node &&
              node->measured_intrinsic_size.width == 70 && node->measured_intrinsic_size.height == 18 &&
              node->measured_intrinsic_baseline == 13 && ui_rect_width(node->rectangle) == 70 &&
              ui_rect_height(node->rectangle) == 18 &&
              !(ordinary_text_style.flags & ui_layout_node_text_content) && measured_manager.count == 2,
          "widget builder text measures callback content rather than the fallback extent, without altering "
          "caller style or adding nodes");
}

} // namespace maicord
