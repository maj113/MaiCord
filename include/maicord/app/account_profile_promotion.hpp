#pragma once

#include "maicord/app/account_profile.hpp"

namespace maicord {

enum class AccountProfilePromotionPhase : u8 {
    empty,
    staged,
    committed,
};

enum class AccountProfilePromotionError : u8 {
    none,
    invalid_input,
    credential,
    profile,
};

struct AccountProfilePromotionResult {
    u32 bytes;
    u32 native_error;
    AccountProfileError profile_error;
    SecureBufferError credential_error;
    AccountProfilePromotionPhase phase;
    AccountProfilePromotionError error;

    constexpr bool ok() const noexcept {
        return error == AccountProfilePromotionError::none;
    }
};

// Owns the single credential between an authentication owner and the atomic
// DPAPI profile commit. Stage transfers the existing locked allocation without
// copying. A failed commit keeps the exact credential staged for an explicit
// retry; successful commit and release both erase it.
struct AccountProfilePromotion {
    AccountProfile profile;
    AccountProfilePromotionResult state;
};

AccountProfilePromotionResult account_profile_promotion_stage(AccountProfilePromotion* promotion,
                                                              AccountProfile* source) noexcept;
AccountProfilePromotionResult account_profile_promotion_commit(AccountProfilePromotion* promotion,
                                                               const wchar_t* path) noexcept;
void account_profile_promotion_release(AccountProfilePromotion* promotion) noexcept;
bool account_profile_promotion_has_credential(const AccountProfilePromotion& promotion) noexcept;

static_assert(sizeof(AccountProfilePromotion) == 48);

} // namespace maicord
