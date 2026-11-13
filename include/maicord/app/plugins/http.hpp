#pragma once

#include "maicord/app/plugins/binding.hpp"
#include "maicord/app/update/https.hpp"

// The HTTP broker behind mc_api.http. A plugin may reach only the HTTPS hosts its descriptor
// declares and the user approved: no Discord-owned host, no credentials and no cookies of the
// client's, bounded bodies and response sizes, a few redirects between approved hosts, and a rate
// limit. Requests run on a worker thread through the update client's HTTPS stack; the results come
// back on the UI thread as completions (plugin_http_deliver). UI thread only, except where noted.
namespace maicord {

// A declared host pattern: "api.example.com" or "*.example.com" (every subdomain, not the apex);
// lowercase ASCII DNS names as update_origin_host_valid accepts, never a Discord-owned host.
bool plugin_network_pattern_valid(const Utf8View& pattern) noexcept;
// Whether a lowercase host name matches a valid pattern.
bool plugin_network_pattern_matches(const Utf8View& pattern, const Utf8View& host) noexcept;
// Hosts the broker refuses whatever a plugin declares: Discord's domains and their subdomains.
bool plugin_network_host_denied(Utf8View host) noexcept;
// The loader's check of descriptor.network once its memory is known to be inside the image: at most
// MC_NETWORK_HOSTS_MAX valid, distinct patterns, and a completion hook to receive the answers.
bool plugin_network_hosts_valid(const mc_str* hosts, u32 count, bool has_completion_hook) noexcept;

// Validates and queues a request for binding (the body of mc_api.http). Returns an MC_* status.
mc_status plugin_http_submit(PluginBinding*, const mc_http_request&, u64 tag) noexcept;

// Signaled when the worker finished a request (an auto-reset event; null until the first request).
// The UI thread waits on it and calls plugin_http_deliver.
HANDLE plugin_http_notification(PluginRuntime*) noexcept;
// Delivers every finished request to its plugin's completion hook. Returns how many were delivered.
u32 plugin_http_deliver(PluginRuntime*) noexcept;
// Requests of the binding that have not run yet fail with MC_HTTP_E_DENIED (a host was revoked) or are
// dropped (disabled: drop = true); running ones finish and are discarded.
void plugin_http_cancel(PluginRuntime*, const PluginBinding*, bool drop) noexcept;
// Stops the worker and frees the broker (plugin_runtime_close).
void plugin_http_close(PluginRuntime*) noexcept;

} // namespace maicord
