#include "maicord/ui/layout/widget_scroll.hpp"
#include "maicord/ui/layout/scroll_host.hpp"
#include "maicord/ui/layout/virtual_list.hpp"
#include "maicord/ui/layout/scrollbar.hpp"
#include "maicord/ui/core/paint.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {

void test_widget_scroll(void (*check)(bool, const char*) noexcept) noexcept {
    {
        PaintStream paint{};
        const bool ready = paint_stream_initialize(&paint) && paint_stream_reset(&paint, 200, 200);
        const auto scrollbar = ui_scrollbar_geometry({0, 0, 200, 200}, 50, 1000);
        Theme gradient = kDefaultTheme;
        gradient.background = background_theme_defaults();
        // One look in every theme: a flat token pill at the thumb's right edge, 4 DIP at rest and 8 DIP while
        // hovered or pressed; the bar takes the stream's hot state only for its own track.
        const Theme* themes[]{&kDefaultTheme, &gradient};
        bool look = ready;
        for (const Theme* theme : themes)
            for (u32 state = 0; look && state < 3; ++state) {
                const auto& tokens = theme->tokens;
                const Color colors[]{tokens.fill.muted, tokens.state.pressed, tokens.text.muted};
                const float width = state ? 8.f : 4.f;
                paint.scrollbar_hot = {scrollbar.track, static_cast<UiScrollbarVisualState>(state)};
                const u32 first = paint.count;
                look = ui_scrollbar_paint(&paint, scrollbar, *theme) &&
                       paint_stream_push_rounded(&paint, scrollbar.thumb.right - width, scrollbar.thumb.top,
                                                 scrollbar.thumb.right, scrollbar.thumb.bottom, colors[state],
                                                 width * .5f) &&
                       paint.count == first + 2 &&
                       bytes_equal(&paint.quads[first], &paint.quads[first + 1], sizeof(PaintQuad));
            }
        check(look, "scrollbar: one flat look in every theme widens from 4 to 8 DIP when hovered or pressed");
        paint.scrollbar_hot = {{0, 0, 10, 10}, UiScrollbarVisualState::pressed};
        const u32 other = paint.count;
        check(ready && ui_scrollbar_paint(&paint, scrollbar, kDefaultTheme) && paint.count == other + 1 &&
                  paint.quads[other].left == scrollbar.thumb.right - 4,
              "scrollbar: another track's hot state leaves the bar at rest");
        paint.scrollbar_hot = {};
        bool filtered = ready && paint_stream_reset(&paint, 200, 200);
        paint.filter_scrollbars = true;
        paint.scrollbar_viewport = {0, 0, 100, 200};
        filtered = filtered && ui_scrollbar_paint(&paint, scrollbar, kDefaultTheme) && paint.count == 0 &&
                   paint.scrollbar_tracks.count == 1 && ui_rect_equal(paint.scrollbar_tracks.tracks[0], scrollbar.track);
        paint.scrollbar_viewport = {0, 0, 200, 200};
        filtered = filtered && ui_scrollbar_paint(&paint, scrollbar, kDefaultTheme) && paint.count == 1 &&
                   paint.scrollbar_tracks.count == 2;
        check(filtered, "scrollbar: the shell filter paints only the pointer's pane yet records every track");
        paint_stream_release(&paint);
    }
    {
        u64 revealed = 0;
        const u64 distant = (1ull << 50) + 3;
        check(ui_scroll_reveal_units(distant, 77, distant + 1000, 0, 200, &revealed) &&
                  revealed == distant - 123 &&
                  ui_scroll_reveal_units(distant, 77, distant + 1000, revealed, 200, &revealed) &&
                  revealed == distant - 123,
              "fixed-point reveal retains exact subpixel units in very long histories");
        check(ui_scroll_reveal_units(150, 200, 500, 200, 100, &revealed) && revealed == 200 &&
                  ui_scroll_reveal_units(150, 200, 500, 0, 100, &revealed) && revealed == 150 &&
                  ui_scroll_reveal_units(150, 200, 500, 400, 100, &revealed) && revealed == 250,
              "oversized logical rows reveal nearest edge without oscillating inside the row");
        check(ui_scroll_reveal_units(0, 100, 100, ~0ull, ~0ull, &revealed) && revealed == 0 &&
                  !ui_scroll_reveal_units(~0ull, 1, ~0ull, 0, 100, &revealed) &&
                  !ui_scroll_reveal_units(0, 1, 100, 0, 0, &revealed),
              "logical reveal clamps huge offsets and rejects overflow and empty viewports");
        u32 heights[4]{};
        u64 prefixes[4]{};
        UiVariableList variable{};
        check(ui_variable_list_initialize(&variable, {heights, 4}, {prefixes, 4}, 4, 100) &&
                  ui_variable_list_set_height(&variable, 1, 300) &&
                  ui_variable_list_reveal(variable, 3, 0, 200, &revealed) && revealed == 400 &&
                  ui_variable_list_reveal(variable, 0, revealed, 200, &revealed) && revealed == 0 &&
                  !ui_variable_list_reveal(variable, 4, 0, 200, &revealed),
              "variable-height reveal uses prefix extents and validates the logical index");
    }
    UiWidget widgets[8]{};
    UiLayoutNode nodes[8]{};
    UiWidgetManager manager{};
    check(ui_widget_manager_initialize(&manager, {widgets, 8}, {nodes, 8}), "reveal manager initialize");
    const auto add = [&](u32 id, UiWidgetHandle parent, UiWidgetKind kind, UiLayoutStyle style,
                         u16 flags) noexcept {
        return ui_widget_manager_add(&manager, parent, {id, kind, flags, {utf8_literal("Reveal"), style}});
    };
    auto style = ui_layout_style();
    style.width = style.height = ui_layout_fill();
    const auto root = add(1, ui_widget_invalid_handle(), UiWidgetKind::container, style, ui_widget_none);
    style.width = style.height = ui_layout_fixed(100);
    style.flow = UiLayoutFlow::vertical;
    style.padding = {10, 10, 10, 10};
    style.flags |= ui_layout_node_scroll_horizontal | ui_layout_node_scroll_vertical;
    const auto outer = add(2, root.handle, UiWidgetKind::scroll_view, style, ui_widget_none);
    style = ui_layout_style();
    style.width = ui_layout_fixed(10);
    style.height = ui_layout_fixed(120);
    const auto spacer = add(3, outer.handle, UiWidgetKind::container, style, ui_widget_none);
    style.width = ui_layout_fixed(80);
    style.height = ui_layout_fixed(60);
    style.margin = {80, 0, 0, 10};
    style.padding = {5, 5, 5, 5};
    style.flags |= ui_layout_node_scroll_horizontal | ui_layout_node_scroll_vertical;
    const auto inner = add(4, outer.handle, UiWidgetKind::scroll_view, style, ui_widget_none);
    style = ui_layout_style();
    style.width = style.height = ui_layout_fixed(20);
    style.margin = {140, 150, 0, 0};
    const auto target = add(5, inner.handle, UiWidgetKind::button, style,
                            ui_widget_enabled | ui_widget_focusable | ui_widget_hit_test);
    if (!root.ok() || !outer.ok() || !spacer.ok() || !inner.ok() || !target.ok() ||
        !ui_widget_manager_layout_rectangle(&manager, {50, 75, 250, 275}).ok()) {
        check(false, "nested reveal hierarchy");
        return;
    }
    UiWidgetScrollGeometry geometry{};
    check(ui_widget_scroll_geometry(manager, outer.handle, &geometry) && geometry.maximum.x == 80 &&
              geometry.maximum.y == 110,
          "outer range excludes clipped inner overflow and includes trailing margin");
    check(ui_widget_scroll_geometry(manager, inner.handle, &geometry) && geometry.maximum.x == 90 &&
              geometry.maximum.y == 120,
          "inner range retains both offscreen axes");
    const u32 revision = manager.layout.revision;
    ui_widget_manager_focus(&manager, target.handle);
    const auto revealed = ui_widget_reveal(&manager, target.handle);
    check(revealed.ok() && revealed.fully_visible && revealed.scrolled == 2 &&
              manager.layout.revision == revision + 1 && manager.focused.slot == target.handle.slot &&
              nodes[outer.handle.slot].scroll_offset.x == 75 &&
              nodes[outer.handle.slot].scroll_offset.y == 95 &&
              nodes[inner.handle.slot].scroll_offset.x == 90 &&
              nodes[inner.handle.slot].scroll_offset.y == 120,
          "nested two-axis reveal commits one layout and preserves focus");
    const auto unchanged = ui_widget_reveal(&manager, target.handle);
    check(unchanged.ok() && unchanged.fully_visible && unchanged.scrolled == 0 &&
              manager.layout.revision == revision + 1,
          "visible reveal does not arrange again");
    ui_widget_manager_block_input(&manager, inner.handle, true);
    check(!ui_widget_reveal(&manager, target.handle).ok() && manager.layout.revision == revision + 1,
          "blocked reveal rejects before mutation");
    ui_widget_manager_block_input(&manager, inner.handle, false);
    ui_widget_manager_set_scroll(&manager, inner.handle, {});
    check(ui_widget_manager_layout_rectangle(&manager, {50, 75, 250, 275}).ok(),
          "restore inner scroll for overflow test");
    nodes[inner.handle.slot].spec.style.flags &= static_cast<u16>(~ui_layout_node_clip_children);
    check(ui_widget_scroll_geometry(manager, outer.handle, &geometry) && geometry.maximum.x == 165 &&
              geometry.maximum.y == 215,
          "unclipped descendant participates in owner range");
    nodes[inner.handle.slot].spec.style.flags |= ui_layout_node_clip_children;
    ui_widget_manager_reset(&manager);
    check(!ui_widget_reveal(&manager, target.handle).ok(), "reveal rejects reset handle");

    // A target larger than its viewport aligns the nearest edge once, then
    // remains stationary while it spans the viewport.
    style = ui_layout_style();
    style.width = style.height = ui_layout_fill();
    style.flags |= ui_layout_node_scroll_vertical;
    const auto big_root =
        add(1, ui_widget_invalid_handle(), UiWidgetKind::scroll_view, style, ui_widget_none);
    style = ui_layout_style();
    style.width = ui_layout_fixed(50);
    style.height = ui_layout_fixed(200);
    style.margin.top = 150;
    const auto big = add(2, big_root.handle, UiWidgetKind::button, style,
                         ui_widget_enabled | ui_widget_focusable | ui_widget_hit_test);
    check(big.ok() && ui_widget_manager_layout_rectangle(&manager, {0, 0, 100, 100}).ok(), "oversized reveal layout");
    const auto big_reveal = ui_widget_reveal(&manager, big.handle);
    check(big_reveal.ok() && !big_reveal.fully_visible && big_reveal.scrolled == 1 &&
              nodes[0].scroll_offset.y == 150 && ui_widget_reveal(&manager, big.handle).scrolled == 0,
          "oversized reveal uses nearest edge without oscillation");
    ui_layout_engine_set_scroll(&manager.layout, {0, manager.layout.generation}, {0, 250});
    check(ui_widget_manager_layout_rectangle(&manager, {0, 0, 100, 100}).ok() &&
              ui_widget_reveal(&manager, big.handle).scrolled == 0,
          "oversized trailing edge remains stable");
    UiScrollHost host{};
    bool changed = false;
    check(ui_scroll_host_wheel_widgets(&host, &manager, {10, 10}, 120, 3, false, 1'000'000) ==
              UiScrollDispatch::handled,
          "retained wheel accepts noninteractive scroll owner");
    nodes[big.handle.slot].spec.style.flags &= static_cast<u16>(~ui_layout_node_visible);
    check(ui_widget_scroll_geometry(manager, big_root.handle, &geometry) && geometry.maximum.y == 0 &&
              ui_scroll_host_tick(&host, 1'250'000, &changed) && changed && nodes[0].scroll_offset.y == 0 &&
              !ui_scroll_host_active(host),
          "wheel uses shared range after hidden content shrinks");
}

} // namespace maicord
