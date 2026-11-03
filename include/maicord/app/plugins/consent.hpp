#pragma once

#include "maicord/app/plugins/binding.hpp"

// What the user is asked before a plugin runs: the permissions and network hosts a plugin needs, shown against
// what the user approved before so an update that asks for more asks again. The approved set itself is kept
// in catalog-state.bin (catalog_state.hpp). UI thread only.
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

} // namespace maicord
