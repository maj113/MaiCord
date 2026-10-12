#pragma once

#include "maicord/app/plugin_module_loader.hpp"
#include "maicord/ui/popup_scene.hpp"

namespace maicord::native_plugin_fixture {
inline constexpr u64 owner_id = 0x4D43504649585432ull;
inline constexpr u64 friends_extension_id = 1;
inline constexpr u64 chat_extension_id = 2;

// The typed mc_menu_context the last menu handler call received.
struct FixtureMenuActivation {
    u64 item_id, user_id, channel_id, message_id, guild_id;
    u32 target, flags;
};
// Test-only export contract shared by the actual ABI-5 DLL and its runtime callers.
// The DLL writes only into caller-owned observations while that caller is live.
struct FixtureAudit {
    UiPageRegistry* pages;
    u32 queries, starts, stops, host_detaches, dll_detaches;
    bool state_zero, state_aligned, stop_after_detach, stop_without_pages;
    bool stop_services_inactive, dll_after_stop;
    u32 menu_activations;
    FixtureMenuActivation last_menu_activation;
    bool stop_menu_services_inactive;
    const PopupScene* popup;
    bool menu_callback_popup_dismissed;
    void (*menu_callback_hook)() noexcept;
    void (*stop_callback_hook)() noexcept;
    void (*start_callback_hook)() noexcept;
    // Set by the DLL after a successful start and cleared before it stops: calls the
    // plugin's own mc.ui.menus unregister_item on its live binding.
    bool (*remove_menu)(u64 extension_id) noexcept;
};
using Configure = void(WINAPI*)(u32 mode, u64 owner, void* audit) noexcept;
} // namespace maicord::native_plugin_fixture
