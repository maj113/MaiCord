#include "maicord/app/account_profile_promotion.hpp"

#include <Windows.h>

namespace maicord {
namespace {

AccountProfilePromotionResult failure(AccountProfilePromotionPhase phase,
                                      AccountProfilePromotionError error, u32 native_error = 0,
                                      AccountProfileError profile_error = AccountProfileError::none,
                                      SecureBufferError credential_error = SecureBufferError::none,
                                      u32 bytes = 0) noexcept {
    return AccountProfilePromotionResult{bytes, native_error, profile_error, credential_error,
                                         phase, error};
}

AccountProfilePromotionResult success(AccountProfilePromotionPhase phase, u32 bytes = 0) noexcept {
    return AccountProfilePromotionResult{bytes,
                                         0,
                                         AccountProfileError::none,
                                         SecureBufferError::none,
                                         phase,
                                         AccountProfilePromotionError::none};
}

bool valid_source(const AccountProfile& source) noexcept {
    return source.metadata.profile_id != 0 && source.credential.data && source.credential.locked &&
           source.credential.capacity != 0 && source.credential.bytes != 0 &&
           source.credential.bytes <= source.credential.capacity &&
           source.credential.bytes <= kAccountProfileMaximumCredentialBytes;
}

} // namespace

AccountProfilePromotionResult account_profile_promotion_stage(AccountProfilePromotion* promotion,
                                                              AccountProfile* source) noexcept {
    if (!promotion || !source || static_cast<void*>(promotion) == static_cast<void*>(source) ||
        promotion->state.phase != AccountProfilePromotionPhase::empty ||
        promotion->profile.credential.data || !valid_source(*source)) {
        return failure(promotion ? promotion->state.phase : AccountProfilePromotionPhase::empty,
                       AccountProfilePromotionError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    const u32 bytes = source->credential.bytes;
    if (!secure_buffer_transfer(&promotion->profile.credential, &source->credential)) {
        promotion->state =
            failure(AccountProfilePromotionPhase::empty, AccountProfilePromotionError::credential,
                    ERROR_INVALID_STATE, AccountProfileError::none, SecureBufferError::invalid_input);
        return promotion->state;
    }
    promotion->profile.metadata = source->metadata;
    source->metadata = AccountProfileMetadata{};
    promotion->state = success(AccountProfilePromotionPhase::staged, bytes);
    return promotion->state;
}

AccountProfilePromotionResult account_profile_promotion_commit(AccountProfilePromotion* promotion,
                                                               const wchar_t* path) noexcept {
    if (!promotion || !path || promotion->state.phase != AccountProfilePromotionPhase::staged ||
        !valid_source(promotion->profile)) {
        return failure(promotion ? promotion->state.phase : AccountProfilePromotionPhase::empty,
                       AccountProfilePromotionError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    const AccountProfileResult saved =
        account_profile_save(path, promotion->profile.metadata, promotion->profile.credential);
    if (!saved.ok()) {
        promotion->state =
            failure(AccountProfilePromotionPhase::staged, AccountProfilePromotionError::profile,
                    saved.native_error, saved.error, SecureBufferError::none, saved.bytes);
        return promotion->state;
    }
    account_profile_release(&promotion->profile);
    promotion->state = success(AccountProfilePromotionPhase::committed, saved.bytes);
    return promotion->state;
}

void account_profile_promotion_release(AccountProfilePromotion* promotion) noexcept {
    if (!promotion) {
        return;
    }
    account_profile_release(&promotion->profile);
    *promotion = AccountProfilePromotion{};
}

bool account_profile_promotion_has_credential(const AccountProfilePromotion& promotion) noexcept {
    return promotion.state.phase == AccountProfilePromotionPhase::staged && valid_source(promotion.profile);
}

} // namespace maicord
