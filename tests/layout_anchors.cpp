#include "maicord/ui/scene.hpp"

namespace maicord {

void test_layout_anchors(void (*check)(bool, const char*) noexcept) noexcept {
    UiLayoutDiagnostic diagnostics[16]{};
    UiScene shell{};
    const bool initialized = ui_scene_initialize_bootstrap(&shell) && ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics, 1280, 800);
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
                  ui_layout_engine_validate(shell.widgets.layout, {diagnostics, 16}).clean(),
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
                  ui_layout_engine_validate(shell.widgets.layout, {diagnostics, 16}).clean(),
              "shell header: resize rearranges shared trailing anchors without a tree rebuild");
        check(ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics, 640, 800) &&
                  ui_rect_equal(ui_scene_header_geometry(shell).search_field, {464, 8, 584, 40}) &&
                  ui_rect_equal(ui_scene_header_geometry(shell).member_toggle, {420, 8, 456, 40}) &&
                  ui_scene_header_geometry(shell).label.right == 412 &&
                  ui_layout_engine_validate(shell.widgets.layout, {diagnostics, 16}).clean(),
              "shell header: compact search preserves readable title and distinct member hit target");
    }
    ui_scene_release(&shell);
}

} // namespace maicord
