#include "maicord/ui/layout_debug.hpp"
#include "maicord/ui/scene.hpp"

namespace maicord {

void test_layout_anchors(void (*check)(bool, const char*) noexcept) noexcept {
    UiWidget widgets[8]{};
    UiLayoutNode nodes[8]{};
    UiLayoutAnchor anchors[3]{};
    UiWidgetManager manager{};
    if (!ui_widget_manager_initialize(&manager, {widgets, 8}, {nodes, 8}) ||
        !ui_layout_engine_bind_anchors(&manager.layout, {anchors, 3})) {
        check(false, "anchor manager initialization");
        return;
    }
    const auto add = [&](u32 id, UiWidgetHandle parent, UiLayoutStyle style) noexcept {
        return ui_widget_manager_add(&manager, parent,
                                     {id,
                                      UiWidgetKind::custom,
                                      ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable,
                                      {utf8_literal("Anchor"), style, {}, 0, id}});
    };
    auto root_style = ui_layout_style();
    root_style.width = root_style.height = ui_layout_fill();
    root_style.padding = {10, 20, 30, 40};
    root_style.flags |=
        ui_layout_node_clip_children | ui_layout_node_scroll_horizontal | ui_layout_node_scroll_vertical;
    auto root = add(1, ui_widget_invalid_handle(), root_style);
    auto style = ui_layout_style();
    style.width = style.height = ui_layout_fill();
    style.margin = {5, 7, 11, 13};
    auto area = add(2, root.handle, style);
    check(area.ok() && ui_widget_manager_set_anchors(&manager, area.handle, {0.25f, 0.25f, 0.75f, 0.75f}),
          "bind anchored region to retained widget");
    auto point_style = ui_layout_style();
    point_style.width = ui_layout_fixed(40);
    point_style.height = ui_layout_fixed(20);
    point_style.horizontal_alignment = point_style.vertical_alignment = UiLayoutAlignment::center;
    auto point = add(3, root.handle, point_style);
    check(point.ok() && ui_widget_manager_set_anchors(&manager, point.handle, {0.5f, 0.5f, 0.5f, 0.5f}),
          "bind point anchor");
    point_style.width = point_style.height = ui_layout_fixed(10);
    point_style.horizontal_alignment = point_style.vertical_alignment = UiLayoutAlignment::end;
    auto corner = add(4, area.handle, point_style);
    check(corner.ok() && ui_widget_manager_set_anchors(&manager, corner.handle, {1, 1, 1, 1}),
          "bind nested corner anchor");
    if (!root.ok() || !area.ok() || !point.ok() || !corner.ok()) {
        check(false, "anchor hierarchy construction");
        return;
    }
    const auto box = [&](UiWidgetHandle handle) noexcept {
        return ui_widget_manager_layout_node(manager, handle)->rectangle;
    };
    auto unbound = add(5, root.handle, point_style);
    check(unbound.ok() && !ui_widget_manager_set_anchors(&manager, unbound.handle, {0, 0, 1, 1}) &&
              manager.layout.anchor_count == 3 &&
              ui_widget_manager_set_anchors(&manager, point.handle, {0.5f, 0.5f, 0.5f, 0.5f}) &&
              manager.layout.anchor_count == 3,
          "anchor capacity failure is atomic and updates reuse the same binding");
    check(ui_widget_manager_layout_rectangle(&manager, {100, 200, 500, 500}).ok() &&
              ui_rect_equal(box(area.handle), {205, 287, 369, 387}) &&
              ui_rect_equal(box(point.handle), {270, 330, 310, 350}) &&
              ui_rect_equal(box(corner.handle), {359, 377, 369, 387}),
          "anchors compose parent origin padding margins point pivots and nested corner");
    UiLayoutDiagnostic diagnostics[16]{};
    check(ui_layout_engine_validate(manager.layout, {diagnostics, 16}).clean(),
          "valid anchor hierarchy passes bounds alignment and clipping diagnostics");
    check(ui_widget_manager_hit_test(manager, 360, 380).slot == corner.handle.slot &&
              ui_widget_manager_focus(&manager, corner.handle),
          "anchored children participate in shared hit testing and focus");
    auto* area_node = ui_layout_engine_resolve(&manager.layout, widgets[area.handle.slot].layout);
    area_node->spec.style.maximum_size.width = 120;
    area_node->spec.style.horizontal_alignment = UiLayoutAlignment::end;
    check(ui_widget_manager_layout_rectangle(&manager, {100, 200, 700, 600}).ok() &&
              ui_rect_equal(box(area.handle), {399, 312, 519, 462}) &&
              ui_rect_equal(box(point.handle), {370, 380, 410, 400}),
          "resizing anchors honors max width and trailing alignment without rewriting child specs");
    check(ui_layout_engine_set_scroll(&manager.layout, widgets[root.handle.slot].layout, {15, 25}) &&
              ui_widget_manager_layout_rectangle(&manager, {100, 200, 700, 600}).ok() &&
              ui_rect_equal(box(area.handle), {384, 287, 504, 437}) &&
              ui_rect_equal(box(corner.handle), {494, 427, 504, 437}) &&
              ui_widget_manager_hit_test(manager, 495, 430).slot == corner.handle.slot,
          "nested anchored content follows parent scrolling exactly once");
    PaintStream paint{};
    paint_stream_reset(&paint, 800, 700);
    check(ui_widget_manager_debug_paint(manager, {}, ui_layout_debug_anchors, kDefaultUiLayoutDebugPalette,
                                        &paint) &&
              paint.count >= 12,
          "debug overlay draws anchor regions and point guides");
    check(ui_layout_engine_set_scroll(&manager.layout, widgets[root.handle.slot].layout, {1000, 1000}) &&
              ui_widget_manager_layout_rectangle(&manager, {100, 200, 700, 600}).ok() &&
              ui_widget_manager_hit_test(manager, box(corner.handle).left + 1, box(corner.handle).top + 1)
                      .slot == kInvalidUiWidgetSlot,
          "scrolled anchored children cannot receive hits outside the parent clip");
    check(ui_layout_engine_set_scroll(&manager.layout, widgets[root.handle.slot].layout, {15, 25}) &&
              ui_widget_manager_layout_rectangle(&manager, {100, 200, 700, 600}).ok(),
          "anchor scroll restore preserves retained hierarchy");
    area_node->rectangle.left += 4;
    auto validation = ui_layout_engine_validate(manager.layout, {diagnostics, 16});
    bool found_anchor = false;
    char warning[256]{};
    for (u32 i = 0; i < validation.count; ++i)
        if (diagnostics[i].kind == UiLayoutDiagnosticKind::invalid_anchor) {
            found_anchor =
                diagnostics[i].node.slot == area.handle.slot &&
                ui_widget_manager_format_diagnostic(manager, diagnostics[i], {warning, 256}) > 32;
        }
    check(found_anchor, "anchor violation identifies offending hierarchy and expected bounds");
    const auto previous = box(point.handle);
    anchors[area_node->spec.style.anchor_slot].region = {0.75f, 0, 0.25f, 1};
    check(ui_widget_manager_layout_rectangle(&manager, {0, 0, 800, 600}).error ==
                  UiLayoutError::invalid_style &&
              ui_rect_equal(box(point.handle), previous),
          "invalid retained anchor edits reject before geometry mutation");
    validation = ui_layout_engine_validate(manager.layout, {diagnostics, 16});
    found_anchor = false;
    for (u32 i = 0; i < validation.count; ++i)
        found_anchor = found_anchor || diagnostics[i].kind == UiLayoutDiagnosticKind::invalid_anchor;
    check(found_anchor, "invalid normalized anchor interval is diagnosed");
    auto invalid = style;
    invalid.flags |= ui_layout_node_anchored;
    check(add(6, root.handle, invalid).error == UiWidgetError::layout &&
              !ui_widget_manager_set_anchors(&manager, area.handle, {0, 0, 1.1f, 1}),
          "foreign anchor indices and out of range regions rejected");
    const auto stale_handle = widgets[area.handle.slot].layout;
    ui_widget_manager_reset(&manager);
    check(!ui_layout_engine_anchor_region(manager.layout, stale_handle) && manager.layout.anchor_count == 0,
          "scene reset invalidates anchor ownership and reuses caller storage");
    root = add(1, ui_widget_invalid_handle(), root_style);
    check(root.ok() && !ui_widget_manager_set_anchors(&manager, root.handle, {0, 0, 1, 1}),
          "root cannot depend on parent anchors");
    ui_widget_manager_reset(&manager);
    root_style.flow = UiLayoutFlow::vertical;
    root = add(1, ui_widget_invalid_handle(), root_style);
    area = add(2, root.handle, style);
    check(root.ok() && area.ok() && !ui_widget_manager_set_anchors(&manager, area.handle, {0, 0, 1, 1}),
          "anchors require explicit overlay ownership rather than ambiguous stack placement");

    // Wrapping must use the arranged anchor width, never a viewport guess.
    ui_widget_manager_reset(&manager);
    root_style = ui_layout_style();
    root_style.width = root_style.height = ui_layout_fill();
    root = add(1, ui_widget_invalid_handle(), root_style);
    style = ui_layout_style();
    style.flags |= ui_layout_node_text_content;
    style.width = ui_layout_fill();
    auto text = add(2, root.handle, style);
    check(text.ok() && ui_widget_manager_set_anchors(&manager, text.handle, {0, 0, 0.5f, 1}),
          "anchor text region");
    ui_widget_manager_set_measure_callback(
        &manager,
        +[](void*, const UiLayoutMeasureRequest& request, UiSize* size, float* baseline) noexcept {
            const u32 width = static_cast<u32>(maximum(1.0f, request.constraint.width));
            *size = {minimum(300.0f, request.constraint.width),
                     static_cast<float>((300u + width - 1u) / width) * 10.0f};
            *baseline = 8;
            return true;
        },
        nullptr);
    check(root.ok() && text.ok() && ui_widget_manager_layout(&manager, {300, 200}).ok() &&
              ui_rect_equal(box(text.handle), {0, 0, 150, 20}) &&
              ui_widget_manager_layout(&manager, {600, 200}).ok() &&
              ui_rect_equal(box(text.handle), {0, 0, 300, 10}),
          "anchored text remeasures and converges on resize");
    const auto* text_node = ui_widget_manager_layout_node(manager, text.handle);
    const UiSize retained_constraint = text_node->measure_constraint;
    check(ui_widget_manager_set_anchors(&manager, text.handle, {0, 0, 0.5f, 1}) &&
              text_node->measure_constraint.width == retained_constraint.width &&
              text_node->measure_constraint.height == retained_constraint.height,
          "unchanged anchors preserve cached text measurement constraints");
    // Content-driven containers with anchored adornments have no parent-size
    // dependency cycle: only their own intrinsic content sizes the container.
    ui_widget_manager_reset(&manager);
    root_style = ui_layout_style();
    root = add(1, ui_widget_invalid_handle(), root_style);
    auto panel_style = ui_layout_style();
    panel_style.minimum_size = {80, 60};
    const auto panel = add(2, root.handle, panel_style);
    corner = add(3, panel.handle, point_style);
    check(corner.ok() && ui_widget_manager_set_anchors(&manager, corner.handle, {1, 1, 1, 1}),
          "anchor content-driven container adornment");
    const u32 passes_before = manager.layout.measure_passes;
    check(root.ok() && panel.ok() && corner.ok() && ui_widget_manager_layout(&manager, {300, 200}).ok() &&
              ui_rect_equal(box(panel.handle), {0, 0, 80, 60}) &&
              ui_rect_equal(box(corner.handle), {70, 50, 80, 60}) &&
              manager.layout.measure_passes == passes_before + 1,
          "anchored adornments do not create intrinsic parent sizing cycles");

    UiScene shell{};
    const bool initialized = ui_scene_initialize_bootstrap(&shell) && ui_scene_layout(&shell, 1280, 800);
    check(initialized, "shell header: shared retained geometry initializes");
    if (initialized) {
        const auto wide = ui_scene_header_geometry(shell);
        const auto* messages = ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::message_view));
        const auto* members = ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::member_sidebar));
        check(ui_rect_equal(wide.title, {312, 0, 1280, 48}) &&
                  ui_rect_equal(wide.search_field, {944, 8, 1224, 40}) && ui_rect_equal(wide.inbox, {1228, 8, 1264, 40}) &&
                  ui_rect_equal(wide.member_toggle, {900, 8, 936, 40}) && wide.label.right == 892 &&
                  messages && ui_rect_equal(messages->rectangle, {312, 48, 1040, 726}) && members &&
                  ui_rect_equal(members->rectangle, {1040, 48, 1280, 800}) &&
                  ui_scene_validate(shell, {diagnostics, 16}).clean(),
              "shell header: member control precedes trailing search while message/member pane bounds stay "
              "unchanged");
        const auto button = ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::member_toggle));
        check(button && ui_widget_manager_hit_test(shell.widgets, 910, 20).slot == button->widget.slot,
              "shell header: painted member control uses the actual retained hit target");
        auto hidden = kGuildChannelShellLayoutMetrics;
        hidden.flags &= ~ui_shell_layout_members;
        ++hidden.revision;
        check(ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, hidden, 1280, 800) &&
                  ui_rect_equal(ui_scene_header_geometry(shell).member_toggle, wide.member_toggle) &&
                  ui_rect_equal(ui_scene_header_geometry(shell).search_field, wide.search_field) &&
                  !ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::member_sidebar)),
              "shell header: hiding member pane leaves trailing header anchors stationary");
        const u32 rebuilds = shell.layout_rebuilds;
        check(ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, hidden, 1200, 800) &&
                  shell.layout_rebuilds == rebuilds &&
                  ui_rect_equal(ui_scene_header_geometry(shell).member_toggle, {820, 8, 856, 40}) &&
                  ui_rect_equal(ui_scene_header_geometry(shell).search_field, {864, 8, 1144, 40}) &&
                  ui_scene_validate(shell, {diagnostics, 16}).clean(),
              "shell header: resize rearranges shared trailing anchors without a tree rebuild");
        check(ui_scene_layout(&shell, 640, 800) &&
                  ui_rect_equal(ui_scene_header_geometry(shell).search_field, {464, 8, 584, 40}) &&
                  ui_rect_equal(ui_scene_header_geometry(shell).member_toggle, {420, 8, 456, 40}) &&
                  ui_scene_header_geometry(shell).label.right == 412 &&
                  ui_scene_validate(shell, {diagnostics, 16}).clean(),
              "shell header: compact search preserves readable title and distinct member hit target");
    }
    ui_scene_release(&shell);
}

} // namespace maicord
