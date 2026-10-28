#pragma once

#include "settings_catalog.hpp"
#include "maicord/ui/settings_scene.hpp"

namespace maicord {
// Native settings pages are registered in catalog panel order, then search.
inline constexpr UiPageKey kNotificationsPage{1, 1}, kAppearancePage{1, 2}, kAccessibilityPage{1, 3},
    kPluginsPage{1, 4}, kSearchPage{1, kSettingsPanelCount + 1};
inline u64 cid(const char* key) noexcept {
    const u32 bytes = c_string_length(key);
    UiSettingsSchemaLocation location{};
    return ui_settings_schema_find(settings_catalog(), {key, bytes}, &location) && location.node != kUiSettingsNoChoice
               ? settings_control_id(location.panel, location.node)
               : 0;
}
} // namespace maicord
