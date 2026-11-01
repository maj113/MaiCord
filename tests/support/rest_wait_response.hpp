#pragma once
// Blocking wait for a REST client's published response, for tests and the live probe. The shipping client
// is pumped from its response event and never blocks on it.
#include "maicord/app/net/rest_client.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_process.hpp"

namespace maicord {
inline RestClientResult rest_client_wait_response(RestClient* client, u32 timeout_ms) noexcept {
    if (!client || !client->response_event) {
        RestClientResult invalid{};
        invalid.error = RestClientError::invalid_input;
        return invalid;
    }
    const DWORD waited = WaitForSingleObject(client->response_event, timeout_ms);
    RestClientResult value = rest_client_status(client);
    if (waited != WAIT_OBJECT_0 && value.error == RestClientError::none) {
        value.error = waited == WAIT_TIMEOUT ? RestClientError::timeout : RestClientError::worker;
        value.native_error = waited == WAIT_TIMEOUT ? WAIT_TIMEOUT : thread_last_error();
    }
    return value;
}
} // namespace maicord
