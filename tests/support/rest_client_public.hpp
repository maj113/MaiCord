#pragma once
// A credential-free REST client for tests and the live probe (documented public endpoints such as Get Gateway): the
// shipping worker without Authorization or user metadata. It connects lazily on its first request.
#include "../../src/app/net/rest_client_internal.hpp"

namespace maicord {
inline RestClientResult rest_client_start_public(RestClient* client, u32 response_capacity,
                                                 RestSharedState* shared = nullptr) noexcept {
    return start_client(client, nullptr, RestAuthorization::none, nullptr, response_capacity, false, shared);
}
} // namespace maicord
