#pragma once

#include "maicord/platform/desktop/fido2/abi.hpp"
#include "maicord/platform/desktop/webauthn.hpp"
#include "maicord/protocol/auth/webauthn_mfa.hpp"

// A WebAuthn assertion over libfido2 (Linux): find the security keys on hidraw, ask them for the assertion of a request and
// follow what the key asks on the way (a PIN, again after a wrong one). The logic takes the library's entry points as a
// table and its surroundings (cancel, the PIN, waiting) as hooks, so the tests drive it with a fake library and no key;
// webauthn_ceremony.cpp runs it on a worker thread against the real one.
namespace maicord::fido2 {

// Resolves libfido2.so.1 (MAICORD_FIDO2_LIBRARY names another one) once and runs fido_init; null when it is not
// installed or lacks a function. The answer is logged once.
const Api* library() noexcept;

enum class Outcome : u8 {
    assertion,      // the key answered: the Result holds it
    no_device,      // no security key was plugged in before the request's time ran out
    cannot_open,    // keys are there and none can be opened (no access to their hidraw nodes)
    not_registered, // every key present answered that it holds none of the credentials
    pin_blocked,    // the key's PIN or user verification is blocked
    pin_not_set,    // the key must verify the user and has no PIN
    cancelled,
    timed_out,      // the key was not touched in time
    failed,         // anything else; Result::native says what libfido2 said last
};

inline constexpr u32 kMaximumDevices = 16;
inline constexpr u32 kMaximumPinBytes = 63;  // CTAP2: at most 63 bytes of UTF-8
inline constexpr u32 kDefaultTimeoutMs = 120'000;
inline constexpr u32 kMaximumTimeoutMs = 600'000;
inline constexpr u32 kRescanMs = 250;  // between looks for a key that is not there yet

struct Result {
    i32 native;  // the last status libfido2 returned
    u8 credential_id[kWebAuthnMfaMaximumCredentialIdBytes];
    u32 credential_id_bytes;
    u8 authenticator_data[1024];
    u32 authenticator_data_bytes;
    u8 signature[1024];
    u32 signature_bytes;
    u8 user_handle[128];
    u32 user_handle_bytes;
};

struct Hooks {
    void* context;
    // Polled between steps; a cancel also makes the blocked fido_dev_get_assert return (the hook for `device`).
    bool (*cancelled)(void* context) noexcept;
    // The key being asked now (null when none): where a cancel sends fido_dev_cancel.
    void (*device)(void* context, Device* device) noexcept;
    // The key wants a PIN: waits for the user and writes it (NUL terminated, at most kMaximumPinBytes) into `pin`.
    // `wrong` is set when the previous PIN was refused and `retries` is the attempts left (-1 unknown). False: the user
    // gave up or the request ended.
    bool (*pin)(void* context, bool wrong, i32 retries, char* pin, u32 capacity) noexcept;
    // Waits `milliseconds` (or less when cancelled).
    void (*pause)(void* context, u32 milliseconds) noexcept;
};

// Runs the request: the relying party and the allowed credentials of `request`, user verification as it asks, the 32-byte
// SHA-256 of the client data as `client_data_hash`, within the request's timeout. Clears the PIN it handled.
Outcome run(const Api& api, const WebAuthnMfaRequestOptions& request, const u8* client_data_hash, const Hooks& hooks,
            Result* result) noexcept;

} // namespace maicord::fido2

namespace maicord {

// webauthn_ceremony_start over the given library (the tests' fake); the public one uses fido2::library().
bool webauthn_ceremony_start_with(const fido2::Api& api, const WebAuthnMfaRequestOptions& request, const Waker& wake,
                                  u64 token) noexcept;

} // namespace maicord
