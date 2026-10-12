#pragma once

#include "maicord/platform/secure_buffer.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
inline constexpr u32 kLoginVerificationMaximumLinkBytes = 3 * 4096 + 1024;
// Exact supported origin/path prefix; this does not establish token validity.
bool login_verification_link_candidate(Utf8View link) noexcept;
enum class LoginVerificationLinkError : u8 {
    none, invalid_input, unsupported_link, missing_token, duplicate_token,
    malformed_encoding, token_too_large, output_too_small,
};
struct LoginVerificationLinkResult {
    u32 token_bytes;
    LoginVerificationLinkError error;
    constexpr bool ok() const noexcept { return error == LoginVerificationLinkError::none; }
};
// Accepts only https://discord.com/verify with query/fragment parameters.
// Fragment token takes precedence, including an empty (invalid) fragment token.
// Decode form-percent UTF-8 straight into caller's locked storage; retain no URL.
// Duplicate selected-component tokens are rejected. Every failure clears a valid
// initialized output; input must not alias it. The caller must redact/discard
// the original URL immediately after consumption and never persist or log it.
LoginVerificationLinkResult login_verification_consume_link(
    Utf8View link, SecureBuffer* token) noexcept;
inline constexpr u32 kLoginResetMaximumLinkBytes = 3 * 4096 + 1024;
using LoginResetLinkError = LoginVerificationLinkError;
using LoginResetLinkResult = LoginVerificationLinkResult;
bool login_reset_link_candidate(Utf8View link) noexcept;
// Exact HTTPS discord.com/reset. Both components must be well-formed UTF-8
// form parameters without duplicate/array token keys; fragment takes priority.
// Valid locked output is erased on every failure. Input must not alias output;
// the caller immediately erases/discards the original URI on every outcome.
LoginResetLinkResult login_reset_consume_link(Utf8View link, SecureBuffer* token) noexcept;
} // namespace maicord
