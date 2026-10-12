#pragma once

#include "maicord/platform/secure_buffer.hpp"
#include "maicord/protocol/user_auth.hpp"

namespace maicord {

inline constexpr u32 kLoginMfaMethodCount = 4;
inline constexpr u32 kLoginMfaCodeDigits = 6;
inline constexpr u32 kLoginMfaBackupMinimumCharacters = 8;
inline constexpr u32 kLoginMfaBackupMaximumCharacters = 11;
inline constexpr u32 kLoginMfaNormalizedCodeBytes = kLoginMfaBackupMaximumCharacters;
inline constexpr u32 kLoginMfaBackupAcknowledgementMilliseconds = 10'000;
inline constexpr u64 kLoginMfaInvalidCode = 60008;

enum class LoginMfaCodeError : u8 {
    none,
    invalid_input,
    invalid_type,
    invalid_length,
    invalid_character,
    output_too_small,
};

struct LoginMfaCodeResult {
    u32 bytes;
    LoginMfaCodeError error;

    constexpr bool ok() const noexcept {
        return error == LoginMfaCodeError::none;
    }
};

// Login MFA is separate from protected-action MFA. This policy follows
// the official client's fixed login order and never exposes password as a login-MFA
// method.
u8 login_mfa_capability(UserAuthMfaType type) noexcept;
bool login_mfa_first_supported(u8 capabilities, UserAuthMfaType* type) noexcept;
bool login_mfa_invalid_error(u64 error_code) noexcept;
u32 login_mfa_backup_acknowledgement_seconds(u64 now_ms, u64 ready_at_ms) noexcept;

// TOTP/SMS are six ASCII digits. Backup input is a printable-ASCII
// recovery-code field; hyphens are removed for the wire value. Validation is
// atomic: destination remains untouched on any failure. Production callers
// provide a view into an already-locked auth arena, so no secret enters pageable
// scratch storage and no allocation is performed here.
LoginMfaCodeResult login_mfa_validate_code(UserAuthMfaType type, ConstSlice<u8> input) noexcept;
LoginMfaCodeResult login_mfa_prepare_code(UserAuthMfaType type, ConstSlice<u8> input,
                                          SecureBuffer* destination) noexcept;

} // namespace maicord
