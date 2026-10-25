#pragma once
#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/rest.hpp"
namespace maicord {
enum class AccountRequirement : u8 {
    none,
    captcha,
    verify_email,
    verify_phone,
    reverify_email,
    reverify_phone,
    email_or_phone,
    reverify_email_or_phone,
    email_or_reverify_phone,
    reverify_either,
    agreements,
    safety_flows,
    other,
};
AccountRequirement decode_account_requirement(JsonValue value) noexcept;
bool account_requirement_update(JsonValue data, Slice<u32> scratch, AccountRequirement* output) noexcept;
Utf8View account_requirement_notice(AccountRequirement value) noexcept;
constexpr bool account_request_allowed(AccountRequirement requirement, RestMethod method) noexcept {
    return requirement == AccountRequirement::none || method == RestMethod::get ||
           method == RestMethod::head;
}
struct ApiRejection {
    u32 code;
    bool has_code;
    bool captcha;
};
__declspec(noinline) constexpr bool api_rejection_requires_action(ApiRejection value) noexcept {
    return value.captcha ||
           (value.has_code && (value.code == 40002 || value.code == 40004 || value.code == 20026));
}
// Retain only numeric codes and challenge presence. Never retain error prose,
// captcha material, or nested validation values from the response body.
bool api_rejection_decode(Utf8View body, Slice<u32> scratch, ApiRejection* output) noexcept;
// Recognizes only a complete 60003 error with a nonempty MFA ticket and method
// list. Visits bounded JSON without retaining any ticket or challenge material.
bool api_rejection_is_mfa_challenge(Utf8View body, Slice<u32> scratch) noexcept;
Utf8View api_rejection_notice(ApiRejection value) noexcept;
} // namespace maicord
