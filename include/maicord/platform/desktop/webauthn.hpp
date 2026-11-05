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
// answers `unavailable` where it cannot). Linux has it when libfido2 (`libfido2.so.1`) can be loaded.
#if defined(_WIN32)
inline constexpr bool webauthn_platform_available() noexcept {
    return true;
}
#else
bool webauthn_platform_available() noexcept;

// The Linux ceremony (src/platform/desktop/linux/webauthn_ceremony.cpp), over libfido2. Windows asks webauthn.dll, whose
// call is one modal OS prompt the caller's loop sits in; libfido2 only blocks on the key, so the ceremony runs on a worker
// thread and the caller's loop goes on. One ceremony at a time:
//
//   start      builds the client data and starts the worker: it finds the keys on hidraw (waiting for one to be plugged in),
//              asks for the assertion and follows what the key says
//   wake       the worker posts `wake` with a = the token given to start and b = kWebAuthnWakePin (the key wants a PIN)
//              or kWebAuthnWakeDone (finished: take the result)
//   PIN        webauthn_ceremony_pin_prompt says whether the last PIN was refused; submit_pin answers
//   cancel     from any thread, at any time; the worker ends with `cancelled`
//   finish     after Done: joins the worker and writes the credential JSON of the Windows path
//   release    cancel and join, for shutdown
inline constexpr u64 kWebAuthnWakePin = 1;
inline constexpr u64 kWebAuthnWakeDone = 2;

enum class WebAuthnCeremonyState : u8 { idle, running, need_pin, done };

enum class WebAuthnCeremonyError : u8 {
    none,
    unavailable,     // no libfido2
    invalid_input,
    client_data,
    no_device,       // no key plugged in before the request's time ran out
    cannot_open,     // keys are there and none can be opened
    not_registered,  // the keys present hold none of the credentials
    pin_blocked,
    pin_not_set,
    cancelled,
    timed_out,       // the key was not touched in time
    output_too_small,
    failed,
};

struct WebAuthnCeremonyResult {
    WebAuthnCeremonyError error;
    i32 native;  // the last status libfido2 returned

    constexpr bool ok() const noexcept {
        return error == WebAuthnCeremonyError::none;
    }
};

struct WebAuthnPinPrompt {
    bool wrong;   // the previous PIN was refused
    i32 retries;  // attempts left before the key blocks its PIN; -1 when the key did not say
};

bool webauthn_ceremony_start(const WebAuthnMfaRequestOptions& request, const Waker& wake, u64 token) noexcept;
WebAuthnCeremonyState webauthn_ceremony_state() noexcept;
WebAuthnPinPrompt webauthn_ceremony_pin_prompt() noexcept;
// The user's PIN (UTF-8, 4 to 63 bytes); only while the state is need_pin. The ceremony copies it and the caller clears its own.
bool webauthn_ceremony_submit_pin(ConstSlice<u8> pin) noexcept;
void webauthn_ceremony_cancel() noexcept;
// Only when the state is done. Writes the credential into the locked buffer when the key answered.
WebAuthnCeremonyResult webauthn_ceremony_finish(SecureBuffer* serialized_credential) noexcept;
void webauthn_ceremony_release() noexcept;
#endif

} // namespace maicord
