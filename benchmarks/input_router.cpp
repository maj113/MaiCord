#include "maicord/core/time.hpp"
#include "maicord/app/input_layers.hpp"
#include "maicord/ui/button_keyboard.hpp"

namespace maicord {
namespace {

struct InputCounters { u64 popup; u64 content; };
UiInputDisposition popup_callback(void* context, const UiInputEvent&) noexcept {
    ++static_cast<InputCounters*>(context)->popup;
    return UiInputDisposition::ignored;
}
UiInputDisposition content_callback(void* context, const UiInputEvent&) noexcept {
    ++static_cast<InputCounters*>(context)->content;
    return UiInputDisposition::handled;
}

} // namespace

bool benchmark_input_router(Clock clock, void (*write)(const char*, u64, const char*) noexcept) noexcept {
    constexpr u32 iterations = 1'000'000;
    const AppPopupInput modes[]{AppPopupInput::none, AppPopupInput::autocomplete, AppPopupInput::modal};
    const char* names[]{"ui_input_content_snapshot_route", "ui_input_autocomplete_snapshot_route",
        "ui_input_modal_snapshot_route"};
    for (u32 mode = 0; mode < 3; ++mode) {
        InputCounters counters{};
        const i64 begin = clock_now();
        for (u32 i = 0; i < iterations; ++i) {
            const auto frame = app_input_layers({modes[mode], false, false},
                {&counters, nullptr, nullptr, popup_callback, content_callback});
            const auto result = ui_input_route({frame.layers, frame.count}, {UiInputKind::character, 'x'});
            if (result.disposition != (mode == 2 ? UiInputDisposition::blocked : UiInputDisposition::handled))
                return false;
        }
        const u64 elapsed = clock_microseconds(clock, begin, clock_now());
        if (!elapsed || counters.popup != (mode == 0 ? 0 : iterations) ||
            counters.content != (mode == 2 ? 0 : iterations)) return false;
        write(names[mode], iterations * 1'000'000ull / elapsed, "runs/s");
    }
    write("ui_input_layer_size", sizeof(UiInputLayer), "bytes");
    write("ui_input_app_frame_size", sizeof(AppInputLayers), "bytes");
    write("ui_input_event_size", sizeof(UiInputEvent), "bytes");
    write("ui_button_keyboard_size", sizeof(UiButtonKeyboard), "bytes");
    UiWidget widgets[3]{};
    UiLayoutNode nodes[3]{};
    UiWidgetManager manager{};
    UiButtonKeyboard keyboard{};
    if (!ui_widget_manager_initialize(&manager, {widgets, 3}, {nodes, 3})) return false;
    auto style = ui_layout_style();
    style.width = style.height = ui_layout_fill();
    style.flow = UiLayoutFlow::vertical;
    const auto root = ui_widget_manager_add(&manager, ui_widget_invalid_handle(),
        {1, UiWidgetKind::container, ui_widget_none, {utf8_literal("Controls"), style}});
    style.width = ui_layout_fixed(80);
    style.height = ui_layout_fixed(24);
    for (u32 i = 0; i < 2; ++i)
        if (!ui_widget_manager_add(&manager, root.handle,
            {i + 2, UiWidgetKind::button, ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable,
                {utf8_literal("Button"), style}}).ok()) return false;
    if (!ui_widget_manager_layout_rectangle(&manager, {0, 0, 100, 100}).ok()) return false;
    const i64 begin = clock_now();
    for (u32 i = 0; i < iterations; ++i) {
        const auto next = ui_widget_manager_next_focus(manager, manager.focused, false, true);
        if (!ui_widget_manager_focus(&manager, next)) return false;
        const auto down = ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, true);
        const auto up = ui_button_keyboard_key(&keyboard, &manager, UiButtonKey::space, false);
        if (!down.handled || down.activate || !up.activate || up.target.slot != next.slot) return false;
    }
    const u64 elapsed = clock_microseconds(clock, begin, clock_now());
    if (!elapsed) return false;
    write("ui_button_tab_space_cycle", iterations * 1'000'000ull / elapsed, "cycles/s");
    return true;
}

} // namespace maicord
