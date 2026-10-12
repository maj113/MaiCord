#pragma once
#include "maicord/app/plugin_settings.hpp"
#include "maicord/ui/settings_scene.hpp"

namespace maicord {
enum class NativePluginSettingsError : u8 { none, invalid_argument, unsupported, capacity };
struct NativePluginSettingsSnapshot {
    Utf8View title, description;
    ConstSlice<NativePluginSettingsControl> controls;
    u32 revision;
};
// No allocation or module callback. Reads only the page prefix; context must be a
// host-format page (kNativePluginSettingsFormatHost).
// Inputs borrow immutable UI-thread module data until removal; outputs own DTOs.
// The native scene must receive project()'s native content, never the DLL context.
NativePluginSettingsError native_plugin_settings_snapshot(const void* context,
    NativePluginSettingsSnapshot* output) noexcept;
NativePluginSettingsError native_plugin_settings_control(const NativePluginSettingsSnapshot& snapshot,
    u32 index, NativePluginSettingsControl* output) noexcept;
// Validates every control before writing either output. Only labels/choice text
// remain borrowed. Native descriptors are owned by the caller until scene detach.
NativePluginSettingsError native_plugin_settings_project(const NativePluginSettingsSnapshot& snapshot,
    Slice<UiSettingsControl> controls, UiSettingsPageContent* output) noexcept;
} // namespace maicord
