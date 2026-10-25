#pragma once

#include "maicord/app/rest_client.hpp"

namespace maicord {

// The account adapters under test submit through rest_client_submit and its
// siblings. tests/rest_submit_hook.cpp defines them once and forwards to the
// functions a test installs, so those tests share one executable. A function a
// test does not install is refused.
struct RestHooks {
    RestClientSubmitResult (*submit)(RestClient*, const RestClientRequest&) noexcept;
    RestClientSubmitResult (*submit_large)(RestClient*, const RestClientRequest&) noexcept;
    RestClientSubmitResult (*submit_large_response)(RestClient*, const RestClientRequest&, u32) noexcept;
    RestClientSubmitResult (*submit_large_owned)(RestClient*, const RestClientRequest&, u8**) noexcept;
    bool (*cancel)(RestClient*, u64) noexcept;
};
void rest_install_hooks(const RestHooks& hooks) noexcept;

// What the account request tests check after a submission: the request as REST
// would see it, plus a pending request a cancellation can claim.
struct RestCapture {
    RestClient* client{};
    RestRouteKey route{};
    u64 request_id{}, active_id{};
    u32 submissions{}, cancellations{}, target_bytes{}, body_bytes{};
    u8 target[kRestClientMaximumTargetBytes]{};
    u8 body[kRestClientMaximumRequestBodyBytes]{};
    bool safe{}, json{}, published{};
    RestClientSubmitError error{};
};
// Submissions record into capture and answer capture.error. A cancellation
// claims active_id unless published, which a request already answered keeps.
// A request that breaks the ordinary shape (identity response, other timeout,
// a body without JSON, oversize) clears capture.safe and is refused.
void rest_install_capture(RestCapture& capture) noexcept;

} // namespace maicord
