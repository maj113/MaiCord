#pragma once
#include "maicord/core/slice.hpp"
#include <stddef.h>

namespace maicord {
// Format tag of host-owned settings pages ("STG4"): the ABI-5 adapter builds
// them from copied plugin data. Plugin memory is never read as a page.
inline constexpr u32 kNativePluginSettingsFormatHost = 0x53544734u;
inline constexpr u32 kNativePluginSettingsMaximumControls = 8192;
inline constexpr u32 kNativePluginSettingsMaximumChoices = 1024;
// Values match UiSettingsControlKind. Slider, category and notice exist only on
// host-format pages.
enum class NativePluginSettingsKind : u8 { toggle, choice, action, slider = 5, category = 8, notice = 9 };
enum class NativePluginSettingsTargetKind : u8 { none, page, control, close, save, reset };
struct NativePluginSettingsPageKey { u64 owner_id, page_id; };
struct NativePluginSettingsTarget {
    NativePluginSettingsTargetKind kind;
    NativePluginSettingsPageKey page;
    u64 control_id;
};
struct NativePluginSettingsActivation {
    NativePluginSettingsTarget target;
    NativePluginSettingsKind kind;
    bool checked;
    u32 selected_choice;
};
struct NativePluginSettingsControl {
    u64 id;
    Utf8View label, description;
    NativePluginSettingsKind kind;
    bool enabled, checked;
    ConstSlice<Utf8View> choices;
    u32 selected_choice;
    Utf8View action_label;
    // Reserved; must be null.
    const void* editor = nullptr;
};
// The prefix that native_plugin_settings_snapshot reads from every page.
struct NativePluginSettingsPage {
    Utf8View title, description;
    ConstSlice<NativePluginSettingsControl> controls;
    u32 revision;
    u32 format = kNativePluginSettingsFormatHost;
};
static_assert(sizeof(NativePluginSettingsControl) == 96);
static_assert(offsetof(NativePluginSettingsControl, choices) == 48);
static_assert(offsetof(NativePluginSettingsControl, editor) == 88);
static_assert(sizeof(NativePluginSettingsActivation) == 40);
} // namespace maicord
