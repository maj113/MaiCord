#pragma once
#include "../src/app/application_retained_projection.hpp"
#include "maicord/core/win32_user.hpp"

// This header is included after the existing isolated host's observations.
namespace maicord::settings_runtime_test {
inline void test_retained_hover_projection() noexcept {
    using namespace app_detail;
    const auto old_scene = g_app.message_scene;
    const auto old_bar = g_app.message_action_bar;
    auto* old_account = g_app.account_ui;
    const bool old_ready = g_app.renderer_ready, old_login = g_app.login.visible,
               old_live_resize = g_app.renderer.live_resize, old_pending = g_app.frame_pending;
    const u32 old_dpi = g_app.renderer.dpi;
    struct Restore {
        MessageScene scene;
        MessageActionBar bar;
        AccountUiRuntime* account;
        bool ready, login, live_resize, pending;
        u32 dpi;
        ~Restore() noexcept {
            message_scene_release(&g_app.message_scene);
            g_app.message_scene = scene;
            g_app.message_action_bar = bar;
            g_app.account_ui = account;
            g_app.renderer_ready = ready;
            g_app.login.visible = login;
            g_app.renderer.live_resize = live_resize;
            g_app.frame_pending = pending;
            g_app.renderer.dpi = dpi;
        }
    } restore{old_scene, old_bar, old_account, old_ready, old_login, old_live_resize, old_pending, old_dpi};
    g_app.message_scene = {};
    g_app.message_action_bar = {};
    g_app.account_ui = nullptr;
    g_app.renderer_ready = true;
    g_app.login.visible = g_app.renderer.live_resize = g_app.frame_pending = false;
    g_app.renderer.dpi = 96;
    MessageRecord records[2]{};
    u32 order[2]{}, heights[2]{};
    u64 fenwick[2]{};
    u8 text[128]{};
    MessageStore store{};
    UiVariableList list{};
    MessageTimeline timeline{};
    MessageSceneRowWidget rows[4]{};
    UiWidget widgets[1 + 4 * kMessageSceneWidgetsPerRow]{};
    UiLayoutNode nodes[1 + 4 * kMessageSceneWidgetsPerRow]{};
    const bool ready =
        message_store_initialize(&store, {records, 2}, {order, 2}, {text, 128}) &&
        message_store_upsert(&store, {901, 70, 1001, 0, 0, utf8_literal("Original first row"), 0}).ok() &&
        message_store_upsert(&store, {902, 70, 1002, 1000, 0, utf8_literal("Original second row"), 0})
            .ok() &&
        ui_variable_list_initialize(&list, {heights, 2}, {fenwick, 2}, 2, ui_extent_units(72)) &&
        message_timeline_initialize(&timeline, &store, &list, ui_extent_units(72), ui_extent_units(240),
                                    false) &&
        message_scene_initialize(&g_app.message_scene, &timeline, {rows, 4},
                                 {widgets, 1 + 4 * kMessageSceneWidgetsPerRow},
                                 {nodes, 1 + 4 * kMessageSceneWidgetsPerRow}) &&
        message_scene_update(&g_app.message_scene, {0, 0, 640, 240}).ok();
    check(ready && g_app.message_scene.count == 2,
          "hover host: real message store, timeline and native retained row targets initialize");
    if (!ready || g_app.message_scene.count != 2)
        return;
    const auto point = [&](u32 row) noexcept {
        const auto* layout = message_scene_layout_node(g_app.message_scene, rows[row].widget);
        return MAKELPARAM(static_cast<short>(layout->rectangle.left + 80),
                          static_cast<short>((layout->rectangle.top + layout->rectangle.bottom) * .5f));
    };
    const u32 base_forces = observed.forced_account_text_updates,
              account_checks = observed.account_text_updates, text_updates = observed.text_builds,
              layout_passes = g_app.message_scene.layout_passes,
              preparations = g_app.message_scene.preparation_passes;
    const auto timeline_before = timeline;
    const auto store_before = store;
    const u16 generation = g_app.message_scene.widgets.generation;
    check(handle_message_hover(point(0)) && g_app.message_action_bar.active &&
              g_app.message_action_bar.message_id == 901 && g_app.frame_pending,
          "hover host: actual cursor dispatch publishes first row action target before the next frame");
    const u32 repeat_checks = observed.account_text_updates, repeat_text = observed.text_builds;
    bool stable = true;
    for (u32 i = 0; i < 64; ++i)
        stable &= handle_message_hover(point(0));
    check(stable && observed.account_text_updates == repeat_checks && observed.text_builds == repeat_text,
          "hover host: repeated same-row pointer events perform no account refresh or native text rebuild");
    check(handle_message_hover(point(1)) && g_app.message_action_bar.message_id == 902 &&
              observed.account_text_updates == account_checks + 2 &&
              observed.text_builds == text_updates + 2 &&
              observed.forced_account_text_updates == base_forces &&
              g_app.message_scene.layout_passes == layout_passes &&
              g_app.message_scene.preparation_passes == preparations &&
              g_app.message_scene.widgets.generation == generation &&
              bytes_equal(&timeline, &timeline_before, sizeof(timeline)) &&
              bytes_equal(&store, &store_before, sizeof(store)),
          "hover host: moving to another row reconciles authority without forcing or changing conversation "
          "geometry");
    const auto more = g_app.message_action_bar.items[5].rectangle;
    const auto hit = message_action_bar_hit_test(g_app.message_action_bar, (more.left + more.right) * .5f,
                                                 (more.top + more.bottom) * .5f);
    check(hit.inside && hit.action == MessageActionBarAction::more && hit.message_id == 902,
          "hover host: current More action resolves the latest authoritative hovered row without waiting "
          "for paint");
    // A real message viewport movement still needs authoritative convergence.
    const u32 retained_forces = observed.forced_account_text_updates, retained_text = observed.text_builds;
    const bool resized = message_scene_update(&g_app.message_scene, {0, 0, 640, 80}).ok();
    const u64 prior_scroll = timeline.scroll_units;
    bool scrolled = false;
    const bool moved = resized && message_scene_scroll_by(&g_app.message_scene, ui_extent_units(36),
                                                          ui_extent_units(1), &scrolled);
    g_app.frame_pending = false;
    check(moved && scrolled && timeline.scroll_units != prior_scroll &&
              refresh_retained_projection(timeline.scroll_units != prior_scroll) && g_app.frame_pending &&
              observed.forced_account_text_updates == retained_forces + 1 &&
              observed.text_builds == retained_text,
          "scroll projection: an actual message viewport change retains the full authoritative convergence "
          "path");
}
} // namespace maicord::settings_runtime_test
