#pragma once
#include "maicord/ui/popup_scene.hpp"

namespace maicord {
struct UiSettingsControl;
inline constexpr u32 kUiSettingsChoiceWindow = 32;
inline constexpr u32 kUiSettingsChoiceMaximum = 1024;
inline constexpr u32 kUiSettingsChoiceMaximumItems = kUiSettingsChoiceWindow + 2;
inline constexpr u32 kUiSettingsChoiceActionFirst = 0xE000;
inline constexpr u32 kUiSettingsChoicePrevious = kUiSettingsChoiceActionFirst + kUiSettingsChoiceMaximum;
inline constexpr u32 kUiSettingsChoiceNext = kUiSettingsChoicePrevious + 1;
// No allocation/retention. Descriptors are projected into the caller's existing
// popup bank and labels borrow the immutable control revision. Failure writes
// no output. Windows always begin on a 32-choice boundary.
u32 ui_settings_choice_items(const UiSettingsControl&, u32 first, Slice<PopupItem>) noexcept;
bool ui_settings_choice_page(u32 count, u32 first, bool previous, u32* next_first) noexcept;
bool ui_settings_choice_selected(const UiSettingsControl&, u32 first, u32 action, u32* selected) noexcept;
} // namespace maicord
