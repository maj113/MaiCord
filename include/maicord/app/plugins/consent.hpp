#pragma once

#include "maicord/app/plugins/binding.hpp"

// What the user is asked before a plugin runs, and what is remembered of the answer: the permissions
// and network hosts a plugin needs, listed by name (high-risk ones set apart), and the approved set
// kept per plugin so an update that asks for more asks again. UI thread only.
namespace maicord {

struct PluginConsentRequest {
    Utf8View name, version, author;
    // Everything the plugin needs: descriptor.permissions plus what its hooks, buttons and hosts imply.
    u64 permissions;
    ConstSlice<Utf8View> hosts;
    // What the user approved before for this plugin (0 on the first enable); additions are marked new.
    u64 approved_permissions;
    u32 approved_hosts;
};

// The label of one MC_PERM_* bit (index 0..63); empty for an unknown one.
Utf8View plugin_permission_label(u32 bit) noexcept;
// Permissions that act for the user or keep their data: shown separately, and refused by the official catalog.
inline constexpr u64 kPluginHighRiskPermissions = MC_PERM_AUTOMATED_ACTIONS | MC_PERM_PERSISTENT_LOG;

// The dialog body: who publishes it, what it can do (new things first on a repeat), the hosts it may
// connect to by name, and the note that plugins are native code. Returns the bytes written, 0 when
// output is too small. A plugin that asks for nothing says so.
u32 plugin_consent_text(const PluginConsentRequest&, Slice<char> output) noexcept;
inline constexpr u32 kPluginConsentTextBytes = 2048;

// Fills a request from a loaded image: its name, version, author, hosts and permissions. hosts must
// hold MC_NETWORK_HOSTS_MAX entries and is valid while the image is loaded.
PluginConsentRequest plugin_consent_request(const PluginImage&, Utf8View (&hosts)[MC_NETWORK_HOSTS_MAX]) noexcept;

// What the user approved for one plugin: permissions and, by bit of the descriptor's host list, hosts.
struct PluginApproval {
    u64 permissions;
    u32 hosts;
};
// Remembers the binding's approved permissions and hosts under its data directory (approval.bin).
bool plugin_approval_save(const PluginBinding&) noexcept;
// What was remembered for owner_id, with the hosts matched to d's list by name. Nothing remembered, or
// an unreadable file, is an empty approval.
PluginApproval plugin_approval_load(const PluginRuntime&, u64 owner_id, const mc_descriptor& d) noexcept;
// Remembers an approval before the plugin is enabled (the consent dialog's accept).
bool plugin_approval_store(const PluginRuntime&, u64 owner_id, const mc_descriptor& d, const PluginApproval&) noexcept;

} // namespace maicord
