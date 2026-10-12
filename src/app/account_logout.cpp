#include "maicord/app/account_logout.hpp"

namespace maicord {
namespace {
constexpr char kTarget[] = "/api/v10/auth/logout";
constexpr char kBody[] = "{\"provider\":null,\"voip_provider\":null}";

bool equal(ConstSlice<u8> bytes, const char* text, u32 count) noexcept {
    if (!bytes.data || bytes.count != count)
        return false;
    for (u32 i = 0; i < count; ++i)
        if (bytes[i] != static_cast<u8>(text[i]))
            return false;
    return true;
}
} // namespace

bool account_logout_request_valid(const RestClientRequest& request) noexcept {
    return request.request_id && request.route.route_id == kAccountLogoutRouteId &&
           request.route.method == RestMethod::post && request.route.major == RestMajorParameter::none &&
           !request.route.major_key && request.json_body && !request.identity_response &&
           equal(request.target, kTarget, sizeof(kTarget) - 1) &&
           equal(request.body, kBody, sizeof(kBody) - 1);
}

AccountLogoutResult account_logout(RestClient* primary, AccountLogoutOperation operation, u64 request_id,
                                   AccountLogoutResponse* response) noexcept {
    if (!primary || !request_id)
        return {RestClientSubmitError::invalid_input, RestClientTakeError::invalid_input, false, false};
    switch (operation) {
    case AccountLogoutOperation::begin: {
        const auto prepared = rest_client_prepare_logout(primary, request_id);
        if (!prepared.ok())
            return {prepared.error, RestClientTakeError::empty, false, false};
        const RestClientRequest request{
            {reinterpret_cast<const u8*>(kTarget), sizeof(kTarget) - 1},
            {reinterpret_cast<const u8*>(kBody), sizeof(kBody) - 1},
            {kAccountLogoutRouteId, 0, RestMethod::post, RestMajorParameter::none},
            request_id,
            10'000,
            true,
            false};
        return {rest_client_submit(primary, request).error, RestClientTakeError::empty, false, false};
    }
    case AccountLogoutOperation::cancel: {
        const bool cancelled = rest_client_cancel_logout(primary, request_id);
        return {cancelled ? RestClientSubmitError::none : RestClientSubmitError::busy,
                RestClientTakeError::empty, cancelled, false};
    }
    case AccountLogoutOperation::take: {
        if (!response)
            return {RestClientSubmitError::invalid_input, RestClientTakeError::invalid_input, false, false};
        RestClientResponse receipt{};
        const auto taken = rest_client_take_logout_response(primary, request_id, &receipt);
        if (!taken.ok())
            return {RestClientSubmitError::none, taken.error, false, false};
        *response = {receipt.request_id,  receipt.retry_at_ms, receipt.rejection,   receipt.native_error,
                     receipt.status_code, receipt.error,       receipt.rate_limited};
        return {RestClientSubmitError::none, RestClientTakeError::none, false, true};
    }
    default:
        return {RestClientSubmitError::invalid_input, RestClientTakeError::invalid_input, false, false};
    }
}
} // namespace maicord
