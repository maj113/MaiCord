#pragma once

#include "maicord/app/update/https.hpp"

// The network under the plugin HTTP broker (http.cpp): the update client's HTTPS stack, in a file of its own
// (http_network.cpp) so a test links a scripted exchange in its place. Called on the broker's worker thread, except
// cancel, which may come from any thread.
namespace maicord {

// An initialized HTTPS transport, or null when the network stack cannot start.
UpdateHttpsTransport* plugin_http_network_open() noexcept;
// One exchange, as https_exchange.
UpdateTransportError plugin_http_network_exchange(UpdateHttpsTransport*, const UpdateHttpRequest&,
                                                  const HttpsExchangeOptions&, UpdateHttpResponse*) noexcept;
// Fails the exchange in progress.
void plugin_http_network_cancel(UpdateHttpsTransport*) noexcept;
void plugin_http_network_close(UpdateHttpsTransport*) noexcept;

} // namespace maicord
