#include "http_network.hpp"
#include "maicord/core/scratch_pages.hpp"

namespace maicord {

UpdateHttpsTransport* plugin_http_network_open() noexcept {
    auto* https = commit_pages<UpdateHttpsTransport>();
    if (https && !update_https_initialize(https)) {
        pages_release(https);
        return nullptr;
    }
    return https;
}

UpdateTransportError plugin_http_network_exchange(UpdateHttpsTransport* https, const UpdateHttpRequest& request,
                                                  const HttpsExchangeOptions& options,
                                                  UpdateHttpResponse* response) noexcept {
    return https_exchange(https, request, options, response);
}

void plugin_http_network_cancel(UpdateHttpsTransport* https) noexcept {
    update_https_cancel(https);
}

void plugin_http_network_close(UpdateHttpsTransport* https) noexcept {
    update_https_release(https);
    pages_release(https);
}

} // namespace maicord
