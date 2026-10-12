#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/scroll_host.hpp"

// The server rail and scrollable menus scroll through the shared scroll host
// exactly like the message and channel lists: wheel and keyboard targets
// glide over several frames with smooth scrolling on and jump with it off.
namespace maicord {
namespace {
struct RailFixture {
    GuildRailScene scene;
    NavigationView* view;
};
// Mirrors the client's guild surface adapter (application_scroll_host.cpp).
bool rail_read(void* context, u64, UiScrollRange* range) noexcept {
    const auto& rail = static_cast<RailFixture*>(context)->scene;
    *range = {rail.viewport, rail.scroll_dips, maximum(0.0f, rail.content_height - ui_rect_height(rail.viewport)),
              32, 1};
    return rail.active;
}
bool rail_write(void* context, u64, double offset, bool* changed) noexcept {
    auto& f = *static_cast<RailFixture*>(context);
    const auto result =
        guild_rail_scene_scroll(&f.scene, f.view, static_cast<float>(offset - f.scene.scroll_dips));
    *changed = result.changed;
    return result.ok();
}
// Mirrors the client's menu surface adapter.
bool popup_read(void* context, u64, UiScrollRange* range) noexcept {
    const auto& popup = *static_cast<PopupScene*>(context);
    *range = {popup.rectangle, popup.scroll_offset, popup.scroll_max, kPopupRowHeight, 1};
    return popup.active;
}
bool popup_write(void* context, u64, double offset, bool* changed) noexcept {
    auto& popup = *static_cast<PopupScene*>(context);
    return popup_scene_scroll(&popup, static_cast<float>(offset - popup.scroll_offset), changed);
}
bool settled_at(float value, float target) noexcept {
    return value > target - .01f && value < target + .01f;
}
// Ticks a 60 Hz clock until the curve settles; counts frames that moved.
u32 settle(UiScrollHost* host, u64* now_us) noexcept {
    u32 moved = 0;
    for (u32 frame = 0; frame < 60 && ui_scroll_host_active(*host); ++frame) {
        *now_us += 16'667;
        bool changed = false;
        if (!ui_scroll_host_tick(host, *now_us, &changed))
            return 0;
        moved += changed;
    }
    return ui_scroll_host_active(*host) ? 0 : moved;
}
} // namespace

void test_menu_rail_smooth_scroll(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr u32 kGuilds = 40;
    static GuildViewRow guilds[kGuilds]{};
    for (u32 i = 0; i < kGuilds; ++i)
        guilds[i] = {.id = 7000 + i, .generation = 1};
    NavigationView view{};
    view.guild_rows = guilds;
    view.guild_capacity = view.guild_count = kGuilds;
    view.guild_revision = 1;
    static RailFixture rail{};
    static GuildRailRowWidget rail_rows[24]{};
    static UiWidget rail_widgets[32]{};
    static UiLayoutNode rail_nodes[32]{};
    rail.view = &view;
    const UiRect viewport{0, 48, 72, 348};
    check(guild_rail_scene_initialize(&rail.scene, {rail_rows, 24}, {rail_widgets, 32}, {rail_nodes, 32}) &&
              guild_rail_scene_update(&rail.scene, &view, viewport, 0).ok(),
          "rail smooth scroll: fixture materializes");
    const UiScrollBinding rail_binding{&rail, 2, rail_read, rail_write};
    UiScrollHost host{};
    u64 now = 1'000'000;
    const UiPoint inside{36, 200};

    // Wheel: queued, then several frames each moving part of the way.
    check(ui_scroll_host_wheel(&host, rail_binding, inside, -120, 3, false, now) == UiScrollDispatch::handled &&
              rail.scene.scroll_dips == 0 && ui_scroll_host_active(host),
          "rail smooth scroll: a wheel notch queues an animation instead of jumping");
    bool changed = false;
    now += 16'667;
    check(ui_scroll_host_tick(&host, now, &changed) && changed && rail.scene.scroll_dips > 0 &&
              rail.scene.scroll_dips < 96 && ui_scroll_host_active(host),
          "rail smooth scroll: the first frame moves only part of the notch");
    // Hit testing follows the animated offset, not the target.
    const GuildRailHit mid = guild_rail_scene_hit_test(rail.scene, inside.x, inside.y);
    const GuildRailRowWidget* mid_row = guild_rail_scene_resolve(rail.scene, mid.widget);
    const UiLayoutNode* mid_node = mid_row ? guild_rail_scene_layout_node(rail.scene, *mid_row) : nullptr;
    check(!mid.id || (mid_node && ui_rect_contains(mid_node->rectangle, inside.x, inside.y)),
          "rail smooth scroll: hit testing resolves the row under the animated layout");
    check(settle(&host, &now) >= 3 && settled_at(rail.scene.scroll_dips, 96),
          "rail smooth scroll: the notch lands on its exact target over multiple frames");

    // Precision touchpad: a 30-unit delta is a quarter notch, still animated.
    check(ui_scroll_host_wheel(&host, rail_binding, inside, -30, 3, false, now) == UiScrollDispatch::handled &&
              settled_at(rail.scene.scroll_dips, 96) && settle(&host, &now) >= 2 && settled_at(rail.scene.scroll_dips, 120),
          "rail smooth scroll: precision deltas glide by their fractional distance");

    // Keyboard and scroll-into-view: the scene places the target, the host
    // replays the move from where the rail was.
    const float before = rail.scene.scroll_dips;
    check(guild_rail_scene_reveal_guild(&rail.scene, view, 7000 + kGuilds - 1), "rail smooth scroll: reveal places the last guild");
    const float after = rail.scene.scroll_dips;
    check(after > before + 200 && guild_rail_scene_update(&rail.scene, &view, viewport, before).ok() &&
              rail.scene.focused_guild_id == 7000 + kGuilds - 1 &&
              ui_scroll_host_scroll_to(&host, rail_binding, after, now) == UiScrollDispatch::handled &&
              rail.scene.scroll_dips == before && settle(&host, &now) >= 3 && settled_at(rail.scene.scroll_dips, after),
          "rail smooth scroll: reveal glides to the revealed row over multiple frames");
    const auto* focused = guild_rail_scene_resolve(rail.scene, rail.scene.widgets.focused);
    check(focused && focused->view.id == 7000 + kGuilds - 1,
          "rail smooth scroll: the revealed guild is focused once it materializes");

    // Smooth scrolling off: every input jumps synchronously.
    host.immediate = true;
    check(guild_rail_scene_update(&rail.scene, &view, viewport, 0).ok() &&
              ui_scroll_host_wheel(&host, rail_binding, inside, -120, 3, false, now) == UiScrollDispatch::handled &&
              settled_at(rail.scene.scroll_dips, 96) && !ui_scroll_host_active(host) &&
              ui_scroll_host_scroll_to(&host, rail_binding, 400, now) == UiScrollDispatch::handled &&
              settled_at(rail.scene.scroll_dips, 400) && !ui_scroll_host_active(host),
          "rail smooth scroll: with smoothing off wheel and reveal jump immediately");

    // Menus (server dropdown, context menus, submenus) share the same path.
    constexpr u32 kItems = 30;
    static PopupItem items[kItems]{};
    for (u32 i = 0; i < kItems; ++i)
        items[i] = {utf8_literal("Menu row"), i + 1, popup_item_enabled};
    static PopupScene popup{};
    static PopupRowWidget popup_rows[kItems]{};
    static UiWidget popup_widgets[2 + 3 * kItems]{};
    static UiLayoutNode popup_nodes[2 + 3 * kItems]{};
    check(popup_scene_initialize(&popup, {items, kItems}, {popup_rows, kItems}, {popup_widgets, 2 + 3 * kItems},
                                 {popup_nodes, 2 + 3 * kItems}) &&
              popup_scene_open(&popup, PopupKind::context_menu, 9, {items, kItems}, {20, 20, 240, 40},
                               {0, 0, 640, 320}, 220)
                  .ok() &&
              popup.scroll_max > 96,
          "menu smooth scroll: a menu taller than its space scrolls");
    const UiScrollBinding popup_binding{&popup, 14, popup_read, popup_write};
    const UiPoint over{(popup.rectangle.left + popup.rectangle.right) * .5f,
                       (popup.rectangle.top + popup.rectangle.bottom) * .5f};
    host = {};
    // The client's retained-tree pass skips the panel (its virtual owner), so
    // the wheel reaches the menu surface binding mirrored here.
    check(ui_scroll_host_wheel_widgets(&host, &popup.widgets, over, -120, 3, false, now, 1, popup.panel) ==
                  UiScrollDispatch::unhandled &&
              ui_scroll_host_wheel_widgets(&host, &rail.scene.widgets, inside, -120, 3, false, now, 1,
                                           rail.scene.root) == UiScrollDispatch::unhandled &&
              !ui_scroll_host_active(host),
          "menu smooth scroll: retained trees leave menu and rail wheels to their surface bindings");
    const float step = 3 * kPopupRowHeight;
    check(ui_scroll_host_wheel(&host, popup_binding, over, -120, 3, false, now) == UiScrollDispatch::handled &&
              popup.scroll_offset == 0 && ui_scroll_host_active(host),
          "menu smooth scroll: a wheel notch queues an animation instead of jumping");
    now += 16'667;
    check(ui_scroll_host_tick(&host, now, &changed) && changed && popup.scroll_offset > 0 &&
              popup.scroll_offset < step,
          "menu smooth scroll: the first frame moves only part of the notch");
    const PopupHit hit = popup_scene_hit_test(popup, over.x, over.y);
    PopupItemLayout hit_layout{};
    check(hit.source_index < kItems && popup_scene_resolve_item_layout(popup, hit.source_index, &hit_layout) &&
              ui_rect_contains(hit_layout.row->rectangle, over.x, over.y),
          "menu smooth scroll: hover and click hit testing follow the animated offset");
    check(settle(&host, &now) >= 3 && settled_at(popup.scroll_offset, step),
          "menu smooth scroll: the notch lands on its exact target over multiple frames");
    // Keyboard End: selection reveal is replayed through the host.
    const float menu_before = popup.scroll_offset;
    check(popup_scene_select_index(&popup, kItems - 1, &changed) && popup.scroll_offset > menu_before + step,
          "menu smooth scroll: End selects and reveals the last row");
    const float menu_after = popup.scroll_offset;
    check(popup_scene_scroll(&popup, menu_before - menu_after, &changed) &&
              ui_scroll_host_scroll_to(&host, popup_binding, menu_after, now) == UiScrollDispatch::handled &&
              popup.scroll_offset == menu_before && settle(&host, &now) >= 3 && settled_at(popup.scroll_offset, menu_after) &&
              popup.selected_index == kItems - 1,
          "menu smooth scroll: keyboard reveal glides over multiple frames and keeps the selection");
    host.immediate = true;
    check(ui_scroll_host_wheel(&host, popup_binding, over, 120, 3, false, now) == UiScrollDispatch::handled &&
              settled_at(popup.scroll_offset, menu_after - step) && !ui_scroll_host_active(host),
          "menu smooth scroll: with smoothing off a wheel notch jumps immediately");
}

} // namespace maicord
