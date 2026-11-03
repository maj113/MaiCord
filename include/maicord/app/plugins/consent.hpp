#pragma once

#include "maicord/app/plugins/binding.hpp"

// What the user is asked before a plugin runs, and what is remembered of the answer: the permissions
// and network hosts a plugin needs, and the approved set kept per plugin so an update that asks for more
// asks again. UI thread only.
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

} // namespace maicord
