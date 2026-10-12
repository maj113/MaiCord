#pragma once

#include "maicord/platform/secure_buffer.hpp"
#include "maicord/protocol/webauthn_mfa.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {

enum class WindowsWebAuthnError : u8 {
    none,
    invalid_input,
    unavailable,
    api_version,
    client_data,
    cancelled,
    timed_out,
    platform,
    assertion,
    output_too_small,
};

struct WindowsWebAuthnResult {
    i32 hresult;
    u32 api_version;
    u32 credential_bytes;
    WindowsWebAuthnError error;
    WebAuthnMfaError codec_error;

    constexpr bool ok() const noexcept {
        return error == WindowsWebAuthnError::none;
    }
};

// Binds the inbox Windows WebAuthn library at run time. This keeps the import
// table and the minimum Windows 10 version independent of the newest SDK while
// still using Windows Hello, security keys, and hybrid authenticators exposed
// by the installed OS.
WindowsWebAuthnResult windows_webauthn_authenticate(
    HWND owner, const WebAuthnMfaRequestOptions& request,
    SecureBuffer* serialized_credential) noexcept;

} // namespace maicord
