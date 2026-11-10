#pragma once

#include "maicord/app/plugins/binding.hpp"
#include "maicord/app/plugins/ui.hpp"

// Shared by ui_registry.cpp, ui_menu.cpp, ui_command.cpp and ui_feedback.cpp: the enabled
// plugins in enable order, and the banner each may show.
namespace maicord::plugin_ui_detail {

inline constexpr u32 kBannerTitleBytes = 96;
inline constexpr u32 kBannerTextBytes = 192;
inline constexpr u32 kBannerActionBytes = 32;

struct Banner {
    bool present, warning;
    u64 tag;
    u32 serial;
    u32 title_bytes, text_bytes, action_bytes;
    char title[kBannerTitleBytes], text[kBannerTextBytes], action[kBannerActionBytes];
};

struct Slot {
    PluginBinding* binding;
    Banner banner;
};

extern Slot g_slots[kPluginMaximumBindings];
extern u32 g_slot_count;
extern u32 g_revision;

Slot* find_slot(u64 owner_id) noexcept;

inline const mc_descriptor& descriptor_of(const PluginBinding& binding) noexcept {
    return *binding.image.descriptor;
}

// A plugin that struck out takes no further calls.
inline bool callable(const PluginBinding& binding) noexcept {
    return !binding.struck_out;
}

// The plugin's banner goes away with the plugin; its modal too (ui_feedback.cpp).
void feedback_remove(Slot* slot) noexcept;

// The mc_api entries behind banners, modals and actions (ui_feedback.cpp); api.cpp validates the context.
mc_status banner_set(PluginBinding&, const mc_banner&) noexcept;
mc_status banner_clear(PluginBinding&, u64 tag) noexcept;
mc_status modal_open(PluginBinding&, const mc_modal&, u64 tag) noexcept;
mc_status open_url(PluginBinding&, mc_str url) noexcept;
mc_status copy_text(PluginBinding&, mc_str text) noexcept;
mc_status open_target(PluginBinding&, u32 what, u64 id, u64 sub) noexcept;

} // namespace maicord::plugin_ui_detail
