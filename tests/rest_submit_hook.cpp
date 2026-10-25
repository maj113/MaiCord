#include "rest_submit_hook.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
RestHooks installed;
RestCapture* captured;
constexpr RestClientSubmitResult refused{RestClientSubmitError::invalid_input};

RestClientSubmitResult capture_submit(RestClient* client, const RestClientRequest& request) noexcept {
    RestCapture& c = *captured;
    ++c.submissions;
    c.client = client;
    c.route = request.route;
    c.request_id = request.request_id;
    c.target_bytes = request.target.count;
    c.body_bytes = request.body.count;
    c.json = request.json_body;
    c.safe = request.target.data && c.target_bytes <= sizeof(c.target) && c.body_bytes <= sizeof(c.body) &&
             (!c.body_bytes || request.body.data) && !request.identity_response &&
             request.timeout_ms == kRestClientDefaultTimeoutMs &&
             (request.json_body || !c.body_bytes);
    if (!c.safe)
        return refused;
    copy_bytes(c.target, request.target.data, c.target_bytes);
    if (c.body_bytes)
        copy_bytes(c.body, request.body.data, c.body_bytes);
    return {c.error};
}

bool capture_cancel(RestClient* client, u64 request_id) noexcept {
    RestCapture& c = *captured;
    ++c.cancellations;
    c.client = client;
    if (request_id != c.active_id || c.published)
        return false;
    c.active_id = 0;
    return true;
}
} // namespace

void rest_install_hooks(const RestHooks& hooks) noexcept {
    installed = hooks;
}

void rest_install_capture(RestCapture& capture) noexcept {
    captured = &capture;
    rest_install_hooks({.submit = capture_submit, .cancel = capture_cancel});
}

RestClientSubmitResult rest_client_submit(RestClient* client, const RestClientRequest& request) noexcept {
    return installed.submit ? installed.submit(client, request) : refused;
}

bool rest_client_cancel(RestClient* client, u64 request_id) noexcept {
    return installed.cancel && installed.cancel(client, request_id);
}

RestClientSubmitResult rest_client_submit_large(RestClient* client, const RestClientRequest& request) noexcept {
    return installed.submit_large ? installed.submit_large(client, request) : refused;
}

RestClientSubmitResult rest_client_submit_large_response(RestClient* client, const RestClientRequest& request,
                                                         u32 response_capacity) noexcept {
    return installed.submit_large_response ? installed.submit_large_response(client, request, response_capacity)
                                           : refused;
}

RestClientSubmitResult rest_client_submit_large_owned(RestClient* client, const RestClientRequest& request,
                                                      u8** body_owner) noexcept {
    return installed.submit_large_owned ? installed.submit_large_owned(client, request, body_owner) : refused;
}

} // namespace maicord
