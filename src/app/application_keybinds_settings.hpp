#pragma once
#include "settings_catalog.hpp"
#include "maicord/app/shortcut_registry.hpp"
#include "maicord/ui/input_router.hpp"

namespace maicord::app_detail {
// Custom keybinds (keybinds.bin), loaded on first use. The revision advances
// whenever the set changes so background polling can rebase its key state.
const CustomKeybinds& application_custom_keybinds() noexcept;
u32 application_custom_keybinds_revision() noexcept;

// Settings > Keybinds: custom keybinds (add with the shared recorder, delete)
// and the read-only default shortcuts from the registry. Push to talk edits
// the voice preferences' key, the same one Voice settings records.
void application_keybinds_settings_visible(bool visible) noexcept;
bool application_keybinds_settings_project(SettingBinding, UiSettingsControl*, UiSettingsControlPresentation*) noexcept;
// Expands keybinds_custom and keybinds_defaults into rows after their node.
u32 application_keybinds_settings_list(SettingBinding, Slice<UiSettingsControl>,
                                       Slice<UiSettingsControlPresentation>) noexcept;
bool application_keybinds_settings_activate(SettingBinding) noexcept;
bool application_keybinds_settings_activate_row(u64 control_id, u32 choice) noexcept;
// List rows of this page (custom or default keybinds).
bool application_keybinds_settings_row(u64 control_id) noexcept;
// While recording, takes keys (and mouse buttons for push to talk); true when consumed.
bool application_keybinds_settings_capture(UiInputKind kind, u32 code) noexcept;
bool application_keybinds_settings_recording() noexcept;

// The Keybinds page projects into this bank instead of the shared settings
// bank (21 rows): it lists every default shortcut.
inline constexpr u32 kKeybindsSettingsBankCapacity = 48;
void application_keybinds_settings_bank(UiSettingsControl** controls, UiSettingsControlPresentation** presentation) noexcept;

// Row indexes inside keybinds_custom.
inline constexpr u32 kKeybindsPushToTalkRow = 8, kKeybindsDraftActionRow = 16, kKeybindsDraftRecordRow = 17,
                     kKeybindsDraftCancelRow = 18;
} // namespace maicord::app_detail
