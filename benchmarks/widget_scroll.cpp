#include "maicord/core/time.hpp"
#include "maicord/ui/widget_scroll.hpp"

namespace maicord {

bool benchmark_widget_scroll(Clock clock, void (*write)(const char*, u64, const char*) noexcept) noexcept {
    constexpr u32 rows = 64;
    constexpr u32 range_iterations = 100'000;
    constexpr u32 reveal_iterations = 10'000;
    UiWidget widgets[rows + 1]{};
    UiLayoutNode nodes[rows + 1]{};
    UiWidgetManager manager{};
    if (!ui_widget_manager_initialize(&manager, {widgets, rows + 1}, {nodes, rows + 1})) return false;
    auto style = ui_layout_style();
    style.flow = UiLayoutFlow::vertical;
    style.width = style.height = ui_layout_fill();
    const auto root = ui_widget_manager_add(&manager, ui_widget_invalid_handle(),
        {1, UiWidgetKind::scroll_view, ui_widget_none, {utf8_literal("Scroll"), style}});
    if (!root.ok()) return false;
    style.height = ui_layout_fixed(24);
    UiWidgetHandle first{}, last{};
    for (u32 i = 0; i < rows; ++i) {
        const auto row = ui_widget_manager_add(&manager, root.handle,
            {i + 2, UiWidgetKind::button, ui_widget_enabled | ui_widget_focusable,
                {utf8_literal("Row"), style}});
        if (!row.ok()) return false;
        if (!i) first = row.handle;
        last = row.handle;
    }
    if (!ui_widget_manager_layout_rectangle(&manager, {0, 0, 240, 240}).ok()) return false;
    UiWidgetScrollGeometry geometry{};
    u64 checksum = 0;
    i64 begin = clock_now();
    for (u32 i = 0; i < range_iterations; ++i) {
        if (!ui_widget_scroll_geometry(manager, root.handle, &geometry) || geometry.maximum.y != 1296) return false;
        checksum += static_cast<u64>(geometry.maximum.y);
    }
    const u64 range_us = clock_microseconds(clock, begin, clock_now());
    begin = clock_now();
    for (u32 i = 0; i < reveal_iterations; ++i) {
        const auto result = ui_widget_reveal(&manager, i & 1 ? first : last);
        if (!result.ok() || !result.fully_visible || result.scrolled != 1) return false;
        checksum += result.scrolled;
    }
    const u64 reveal_us = clock_microseconds(clock, begin, clock_now());
    const u32 revision = manager.layout.revision;
    begin = clock_now();
    for (u32 i = 0; i < reveal_iterations; ++i) {
        const auto result = ui_widget_reveal(&manager, first);
        if (!result.ok() || !result.fully_visible || result.scrolled || manager.layout.revision != revision) return false;
        checksum += result.fully_visible;
    }
    const u64 visible_us = clock_microseconds(clock, begin, clock_now());
    if (!range_us || !reveal_us || !visible_us || !checksum) return false;
    write("ui_scroll_range_64_rows", range_iterations * 1'000'000ull / range_us, "runs/s");
    write("ui_scroll_reveal_64_rows", reveal_iterations * 1'000'000ull / reveal_us, "runs/s");
    write("ui_scroll_reveal_already_visible_64_rows", reveal_iterations * 1'000'000ull / visible_us, "runs/s");
    return true;
}

} // namespace maicord
