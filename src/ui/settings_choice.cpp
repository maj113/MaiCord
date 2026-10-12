#include "maicord/ui/settings_choice.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
bool valid(const UiSettingsControl& control, u32 first) noexcept {
    return control.id && control.enabled && control.kind == UiSettingsControlKind::choice &&
        control.choices.count && control.choices.count <= kUiSettingsChoiceMaximum &&
        (control.selected_choice < control.choices.count || control.selected_choice == kUiSettingsNoChoice) &&
        first < control.choices.count &&
        first % kUiSettingsChoiceWindow == 0 && byte_range_valid({reinterpret_cast<const u8*>(control.choices.data),
            control.choices.count * static_cast<u32>(sizeof(Utf8View))});
}
}
u32 ui_settings_choice_items(const UiSettingsControl& control, u32 first, Slice<PopupItem> output) noexcept {
    if (!valid(control, first)) return 0;
    const u32 count = minimum(kUiSettingsChoiceWindow, control.choices.count - first);
    const bool previous = first != 0, next = first + count < control.choices.count;
    const u32 total = count + static_cast<u32>(previous) + static_cast<u32>(next);
    const ConstSlice<u8> destination{reinterpret_cast<const u8*>(output.data), total * static_cast<u32>(sizeof(PopupItem))};
    if (!output.data || output.count < total || !byte_range_valid(destination) ||
        byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(&control), sizeof(control)}) ||
        byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(control.choices.data),
            control.choices.count * static_cast<u32>(sizeof(Utf8View))})) return 0;
    for (u32 index = first; index < first + count; ++index) {
        const auto label = control.choices[index];
        const ConstSlice<u8> text{reinterpret_cast<const u8*>(label.data), label.bytes};
        if (!label.bytes || !byte_range_valid(text) || byte_ranges_overlap(text, destination) || !utf8_validate(label).valid) return 0;
    }
    u32 used = 0;
    if (previous) output[used++] = {utf8_literal("Previous choices"), kUiSettingsChoicePrevious, popup_item_enabled};
    for (u32 index = first; index < first + count; ++index)
        output[used++] = {control.choices[index], kUiSettingsChoiceActionFirst + index,
            static_cast<u16>(popup_item_enabled | (index == control.selected_choice ? popup_item_checked : 0))};
    if (next) output[used++] = {utf8_literal("Next choices"), kUiSettingsChoiceNext, popup_item_enabled};
    return used;
}
bool ui_settings_choice_page(u32 count, u32 first, bool previous, u32* next_first) noexcept {
    if (!next_first || !count || count > kUiSettingsChoiceMaximum || first >= count || first % kUiSettingsChoiceWindow) return false;
    if (previous ? first == 0 : first + kUiSettingsChoiceWindow >= count) return false;
    *next_first = previous ? first - kUiSettingsChoiceWindow : first + kUiSettingsChoiceWindow;
    return true;
}
bool ui_settings_choice_selected(const UiSettingsControl& control, u32 first, u32 action, u32* selected) noexcept {
    if (!selected || !valid(control, first) || action < kUiSettingsChoiceActionFirst) return false;
    const u32 index = action - kUiSettingsChoiceActionFirst;
    if (index < first || index >= first + minimum(kUiSettingsChoiceWindow, control.choices.count - first)) return false;
    *selected = index;
    return true;
}
} // namespace maicord
