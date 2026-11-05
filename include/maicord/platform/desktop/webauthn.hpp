#pragma once

#include "maicord/platform/crypto/secure_buffer.hpp"
#include "maicord/protocol/auth/webauthn_mfa.hpp"
#include "maicord/platform/window/window.hpp"

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
    WindowsWebAuthnError error;

    constexpr bool ok() const noexcept {
        return error == WindowsWebAuthnError::none;
    }
};

// Binds the inbox Windows WebAuthn library at run time. This keeps the import
// table and the minimum Windows 10 version independent of the newest SDK while
// still using Windows Hello, security keys, and hybrid authenticators exposed
// by the installed OS.
WindowsWebAuthnResult windows_webauthn_authenticate(
    Window owner, const WebAuthnMfaRequestOptions& request,
    SecureBuffer* serialized_credential) noexcept;

// Whether this system has a security-key ceremony. Windows does (webauthn.dll is asked when the ceremony runs, and
// answers `unavailable` where it cannot). Linux has none: libfido2 (the only route to USB and NFC keys there) was
// evaluated and not bound, because its call blocks on a touch while the Windows ceremony is a modal OS dialog that the
// caller's loop sits in (AGENTS.md, "WebAuthn on Linux"). The login says so at once rather than start one.
#if defined(_WIN32)
inline constexpr bool kWebAuthnAvailable = true;
#else
inline constexpr bool kWebAuthnAvailable = false;
#endif

} // namespace maicord
