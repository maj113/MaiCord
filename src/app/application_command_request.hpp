#pragma once
#include "maicord/app/rest_client.hpp"

namespace maicord::app_detail {
// Body bytes borrow the caller's bounded serialization until submit copies
// them. Execution and autocomplete use the same JSON transport contract.
inline RestClientRequest application_command_interaction_request(
    ConstSlice<u8> body, u64 request_id, bool autocomplete) noexcept {
    constexpr auto target = utf8_literal("/api/v10/interactions");
    return {{reinterpret_cast<const u8*>(target.data), target.bytes}, body,
            {0x434d4458, 0, RestMethod::post, RestMajorParameter::none},
            request_id, autocomplete ? 3000u : kRestClientDefaultTimeoutMs, true};
}
} // namespace maicord::app_detail
