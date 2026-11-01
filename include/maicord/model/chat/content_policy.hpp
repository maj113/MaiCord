#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Account-scoped service metadata. Unknown eligibility is distinct from a
// restriction and is never evidence that age confirmation has succeeded.
enum class AccountContentEligibility : u8 { unknown, allowed, restricted };
enum class AccountAgeVerification : u8 {
    unknown,
    unverified,
    verified_teen,
    verified_adult,
    inferred_adult,
};
enum AccountContentFlag : u8 {
    account_content_teen_defaults = 1u << 0,
    account_content_age_confirmation = 1u << 1,
    account_content_parent_managed = 1u << 2,
};
struct AccountContentPolicy {
    AccountContentEligibility eligibility;
    AccountAgeVerification age_verification;
    u8 flags;
};
static_assert(sizeof(AccountContentPolicy) == 3);

constexpr bool account_content_confirmation_required(const AccountContentPolicy& policy) noexcept {
    return (policy.flags & account_content_age_confirmation) &&
           policy.age_verification != AccountAgeVerification::verified_adult &&
           policy.age_verification != AccountAgeVerification::inferred_adult;
}

} // namespace maicord
