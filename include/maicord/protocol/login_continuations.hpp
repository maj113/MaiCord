#pragma once

#include "maicord/protocol/user_auth.hpp"

namespace maicord {
inline constexpr u32 kLoginVerificationMaximumTokenBytes = 4096;
inline constexpr Utf8View kLoginVerificationTarget = utf8_literal("/api/v9/auth/verify");

// The public verify action submits only {token}; this is a link token,
// never a manually supplied account credential. All validation precedes writes.
UserAuthWriteResult login_verification_write_request(Utf8View token, Slice<u8> output) noexcept;

inline constexpr u32 kLoginResetMaximumTokenBytes = 4096;
inline constexpr Utf8View kLoginResetTarget = utf8_literal("/api/v9/auth/reset");
struct UserAuthResetPasswordRequest { Utf8View token; Utf8View password; };
struct UserAuthResetWriteResult {
    u32 bytes;
    u32 token_prefix_bytes;
    UserAuthWriteError error;
    constexpr bool ok() const noexcept { return error == UserAuthWriteError::none; }
};
// Explicit measurements validate inputs and never touch storage. Writers
// reject source/output overlap and perform every capacity check before writing.
// token_prefix_bytes ends after the encoded token, before the password comma.
UserAuthResetWriteResult user_auth_measure_reset_password_request(
    const UserAuthResetPasswordRequest& request) noexcept;
UserAuthResetWriteResult user_auth_write_reset_password_request(
    const UserAuthResetPasswordRequest& request, Slice<u8> output) noexcept;
// Starts with ,"password": and closes the complete initial reset object.
UserAuthWriteResult user_auth_measure_reset_password_tail(Utf8View password) noexcept;
UserAuthWriteResult user_auth_write_reset_password_tail(Utf8View password, Slice<u8> output) noexcept;
// Replace the retained initial body's final brace with this complete suffix.
// Code normalization is the caller's method-specific responsibility. WebAuthn
// accepts only the typed native assertion schema, never an arbitrary token.
UserAuthWriteResult user_auth_measure_reset_mfa_suffix(
    Utf8View code, Utf8View ticket, UserAuthMfaType type) noexcept;
UserAuthWriteResult user_auth_write_reset_mfa_suffix(
    Utf8View code, Utf8View ticket, UserAuthMfaType type, Slice<u8> output) noexcept;
} // namespace maicord
