#pragma once

#include "maicord/app/rest_client.hpp"

namespace maicord {

inline constexpr u64 kAccountLogoutRouteId = 0x415554484C4F474Full;

enum class AccountLogoutOperation : u8 {
    begin,
    cancel,
    take,
};

// A response contains metadata only. No credential, URL, response
// prose or challenge secret crosses the executable/module boundary.
struct AccountLogoutResponse {
    u64 request_id;
    u64 retry_at_ms;
    ApiRejection rejection;
    u32 native_error;
    u16 http_status;
    RestClientError transport_error;
    bool rate_limited;
};

struct AccountLogoutResult {
    RestClientSubmitError submit_error;
    bool cancelled;
    bool consumed;
};

// Begin reserves PRIMARY, cancels any unrelated request that is in flight,
// then submits one fixed authenticated request. A busy result may be polled
// with the same intent ID; it never replays an accepted logout. Cancel keeps
// the request alive until its result is taken or the worker is joined;
// cancelling before submission also prevents a later begin from issuing the
// POST. Take returns only this logout's response, including after
// authentication rejection.
AccountLogoutResult account_logout(RestClient* primary, AccountLogoutOperation operation, u64 request_id,
                                   AccountLogoutResponse* response) noexcept;

// Shared by request validation and the worker's pre-send restriction check.
// Accepts only the canonical no-push logout operation.
bool account_logout_request_valid(const RestClientRequest& request) noexcept;

static_assert(sizeof(AccountLogoutResponse) == 32);
static_assert(sizeof(AccountLogoutResult) == 3);

} // namespace maicord
