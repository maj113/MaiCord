// The security-key ceremony of Linux (platform/desktop/webauthn.hpp): the WebAuthn request of the sign-in answered by a key
// through libfido2. The assertion runs on a worker thread (fido2.cpp holds the steps), because fido_dev_get_assert blocks on
// the key; the caller's loop goes on, is told by a wake when the key wants a PIN and when the ceremony ends, and can cancel
// at any time. The response is mapped to the credential JSON by the code the Windows path uses.
#include "fido2.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/crypto/hkdf_sha256.hpp"

namespace maicord {
namespace {

constexpr u32 kWorkerStackBytes = 256 * 1024;
constexpr u32 kJoinMs = 5000;
// A registration's attestation object: the format name, an attestation statement and the authenticator data.
constexpr u32 kAttestationObjectBytes = 3200;

struct Ceremony {
    const fido2::Api* api;
    Thread* thread;
    Waker wake;
    u64 token;
    u64 deadline;
    // What the ceremony asks: an assertion uses only `creation.request`.
    WebAuthnMfaCreationOptions creation;
    bool creating;
    u8 client_data[kWebAuthnMfaMaximumClientDataBytes];
    u32 client_data_bytes;
    u8 client_data_hash[32];
    Waitable event;               // auto-reset: a PIN came, or a cancel
    volatile LONG state;          // WebAuthnCeremonyState
    volatile LONG cancel;
    volatile LONG device_lock;
    fido2::Device* device;        // the key the worker is asking, under device_lock
    volatile LONG pin_given;
    char pin[fido2::kMaximumPinBytes + 1];
    WebAuthnPinPrompt prompt;
    fido2::Outcome outcome;
    fido2::Result result;
    fido2::MakeResult made;
};
Ceremony g_ceremony;

void set_state(Ceremony& c, WebAuthnCeremonyState state) noexcept {
    InterlockedExchange(&c.state, static_cast<LONG>(state));
}

WebAuthnCeremonyState state_of(Ceremony& c) noexcept {
    return static_cast<WebAuthnCeremonyState>(InterlockedCompareExchange(&c.state, 0, 0));
}

void lock_device(Ceremony& c) noexcept {
    while (InterlockedCompareExchange(&c.device_lock, 1, 0) != 0)
        thread_sleep(0);
}

void unlock_device(Ceremony& c) noexcept {
    InterlockedExchange(&c.device_lock, 0);
}

bool cancelled(void* context) noexcept {
    return InterlockedCompareExchange(&static_cast<Ceremony*>(context)->cancel, 0, 0) != 0;
}

void on_device(void* context, fido2::Device* device) noexcept {
    auto& c = *static_cast<Ceremony*>(context);
    lock_device(c);
    c.device = device;
    // A cancel that came between two keys still stops the next one.
    if (device && cancelled(&c))
        c.api->fido_dev_cancel(device);
    unlock_device(c);
}

bool on_pin(void* context, bool wrong, i32 retries, char* pin, u32 capacity) noexcept {
    auto& c = *static_cast<Ceremony*>(context);
    c.prompt = {wrong, retries};
    InterlockedExchange(&c.pin_given, 0);
    set_state(c, WebAuthnCeremonyState::need_pin);
    window_post(c.wake, c.token, kWebAuthnWakePin);
    for (;;) {
        if (cancelled(&c))
            return false;
        if (InterlockedCompareExchange(&c.pin_given, 0, 0) != 0) {
            u32 length = 0;
            while (c.pin[length] && length + 1 < capacity) {
                pin[length] = c.pin[length];
                ++length;
            }
            pin[length] = 0;
            secure_clear_bytes(c.pin, sizeof(c.pin));
            set_state(c, WebAuthnCeremonyState::running);
            return true;
        }
        const u64 now = clock_monotonic_milliseconds();
        if (now >= c.deadline)
            return false;
        wait_one(c.event, static_cast<u32>(minimum<u64>(c.deadline - now, 1000)));
    }
}

void on_pause(void* context, u32 milliseconds) noexcept {
    wait_one(static_cast<Ceremony*>(context)->event, milliseconds);
}

u32 worker(void* context) noexcept {
    auto& c = *static_cast<Ceremony*>(context);
    const fido2::Hooks hooks{&c, cancelled, on_device, on_pin, on_pause};
    c.outcome = c.creating ? fido2::make(*c.api, c.creation, c.client_data_hash, hooks, &c.made)
                           : fido2::run(*c.api, c.creation.request, c.client_data_hash, hooks, &c.result);
    set_state(c, WebAuthnCeremonyState::done);
    window_post(c.wake, c.token, kWebAuthnWakeDone);
    return 0;
}

WebAuthnCeremonyError error_of(fido2::Outcome outcome) noexcept {
    switch (outcome) {
    case fido2::Outcome::assertion:
    case fido2::Outcome::created:
        return WebAuthnCeremonyError::none;
    case fido2::Outcome::no_device:
        return WebAuthnCeremonyError::no_device;
    case fido2::Outcome::cannot_open:
        return WebAuthnCeremonyError::cannot_open;
    case fido2::Outcome::not_registered:
        return WebAuthnCeremonyError::not_registered;
    case fido2::Outcome::pin_blocked:
        return WebAuthnCeremonyError::pin_blocked;
    case fido2::Outcome::pin_not_set:
        return WebAuthnCeremonyError::pin_not_set;
    case fido2::Outcome::cancelled:
        return WebAuthnCeremonyError::cancelled;
    case fido2::Outcome::timed_out:
        return WebAuthnCeremonyError::timed_out;
    case fido2::Outcome::excluded:
        return WebAuthnCeremonyError::excluded;
    case fido2::Outcome::unsupported:
        return WebAuthnCeremonyError::unsupported;
    case fido2::Outcome::key_full:
        return WebAuthnCeremonyError::key_full;
    default:
        return WebAuthnCeremonyError::failed;
    }
}

// Joins the worker (it is done or about to be) and puts the ceremony back to idle, its secrets erased.
void reset(Ceremony& c) noexcept {
    if (c.thread) {
        thread_wait(c.thread, kJoinMs);
        thread_close(c.thread);
    }
    if (c.event)
        waitable_close(c.event);
    secure_clear_bytes(&c, static_cast<u32>(sizeof(c)));
}

// The credential JSON of a sign-in: the key's answer in the shape the Windows path writes.
WebAuthnMfaResult serialize_assertion(const Ceremony& c, SecureBuffer* output) noexcept {
    const fido2::Result& answer = c.result;
    const WebAuthnMfaAssertionView view{
        ConstSlice<u8>{answer.credential_id, answer.credential_id_bytes},
        ConstSlice<u8>{c.client_data, c.client_data_bytes},
        ConstSlice<u8>{answer.authenticator_data, answer.authenticator_data_bytes},
        ConstSlice<u8>{answer.signature, answer.signature_bytes},
        ConstSlice<u8>{answer.user_handle, answer.user_handle_bytes},
        WebAuthnAuthenticatorAttachment::cross_platform};
    return webauthn_mfa_write_assertion(view, Slice<u8>{output->data, output->capacity});
}

// The credential JSON of a registration: the attestation object from the key's format, statement and authenticator
// data (a "none" attestation drops the statement and the key's AAGUID, as a browser does).
WebAuthnMfaResult serialize_creation(Ceremony& c, SecureBuffer* output) noexcept {
    fido2::MakeResult& made = c.made;
    WebAuthnMfaAttestationObjectView object{{made.format, c_string_length(made.format)},
                                            {made.attestation_statement, made.attestation_statement_bytes},
                                            {made.authenticator_data, made.authenticator_data_bytes}};
    if (c.creation.attestation_none) {
        webauthn_mfa_clear_aaguid(made.authenticator_data, made.authenticator_data_bytes);
        object.format = utf8_literal("none");
        object.attestation_statement = {};
    }
    u8 encoded[kAttestationObjectBytes];
    const WebAuthnMfaResult written = webauthn_mfa_write_attestation_object(object, Slice<u8>{encoded, sizeof(encoded)});
    if (!written.ok())
        return written;
    i32 algorithm = webauthn_mfa_credential_algorithm(object.authenticator_data);
    if (algorithm == 0)
        algorithm = c.creation.algorithms[0];
    // A key on hidraw is reached over USB.
    const WebAuthnMfaAttestationView view{ConstSlice<u8>{made.credential_id, made.credential_id_bytes},
                                          ConstSlice<u8>{c.client_data, c.client_data_bytes},
                                          ConstSlice<u8>{encoded, written.bytes},
                                          object.authenticator_data,
                                          algorithm,
                                          webauthn_transport_usb,
                                          WebAuthnAuthenticatorAttachment::cross_platform};
    const WebAuthnMfaResult serialized = webauthn_mfa_write_attestation(view, Slice<u8>{output->data, output->capacity});
    secure_clear_bytes(encoded, sizeof(encoded));
    return serialized;
}

// Starts the worker over the options already copied into the ceremony: the client data, its hash and the thread.
bool begin(const fido2::Api& api, bool creating, const Waker& wake, u64 token) noexcept {
    Ceremony& c = g_ceremony;
    const WebAuthnMfaRequestOptions& request = c.creation.request;
    if (request.challenge_bytes == 0 || request.challenge_bytes > kWebAuthnMfaMaximumChallengeBytes ||
        request.credential_count > kWebAuthnMfaMaximumAllowedCredentials ||
        request.credential_id_bytes > kWebAuthnMfaCredentialIdStorageBytes ||
        (creating && (c.creation.user_id_bytes == 0 || c.creation.user_id_bytes > kWebAuthnMfaMaximumUserIdBytes ||
                      c.creation.algorithm_count == 0 ||
                      c.creation.algorithm_count > kWebAuthnMfaMaximumAlgorithms))) {
        reset(c);
        return false;
    }
    c.creating = creating;
    const Slice<u8> client_data_output{c.client_data, static_cast<u32>(sizeof(c.client_data))};
    const WebAuthnMfaResult client_data = creating ? webauthn_mfa_write_creation_client_data(c.creation, client_data_output)
                                                   : webauthn_mfa_write_client_data(request, client_data_output);
    if (!client_data.ok()) {
        reset(c);
        return false;
    }
    c.client_data_bytes = client_data.bytes;
    // The key signs the hash of the client data; the data itself goes back to the server in the credential.
    KdfSha256Workspace workspace{};
    bool hashed = kdf_sha256_initialize(&workspace).ok();
    if (hashed) {
        const ConstSlice<u8> parts[1] = {{c.client_data, c.client_data_bytes}};
        hashed = sha256(workspace, ConstSlice<ConstSlice<u8>>{parts, 1}, Slice<u8>{c.client_data_hash, 32}).ok();
        kdf_sha256_release(&workspace);
    }
    c.event = event_create(false, false);
    if (!hashed || !c.event) {
        reset(c);
        return false;
    }
    c.api = &api;
    c.wake = wake;
    c.token = token;
    const u32 timeout = request.timeout_ms == 0 ? fido2::kDefaultTimeoutMs
                        : request.timeout_ms > fido2::kMaximumTimeoutMs ? fido2::kMaximumTimeoutMs
                                                                         : request.timeout_ms;
    c.deadline = clock_monotonic_milliseconds() + timeout;
    set_state(c, WebAuthnCeremonyState::running);
    c.thread = thread_start(worker, &c, kWorkerStackBytes, nullptr);
    if (!c.thread) {
        reset(c);
        return false;
    }
    return true;
}

} // namespace

bool webauthn_ceremony_start_with(const fido2::Api& api, const WebAuthnMfaRequestOptions& request, const Waker& wake,
                                  u64 token) noexcept {
    if (state_of(g_ceremony) != WebAuthnCeremonyState::idle)
        return false;
    clear_bytes(&g_ceremony, static_cast<u32>(sizeof(g_ceremony)));
    copy_bytes(&g_ceremony.creation.request, &request, static_cast<u32>(sizeof(request)));
    return begin(api, false, wake, token);
}

bool webauthn_ceremony_start_creation_with(const fido2::Api& api, const WebAuthnMfaCreationOptions& creation,
                                           const Waker& wake, u64 token) noexcept {
    if (state_of(g_ceremony) != WebAuthnCeremonyState::idle)
        return false;
    clear_bytes(&g_ceremony, static_cast<u32>(sizeof(g_ceremony)));
    copy_bytes(&g_ceremony.creation, &creation, static_cast<u32>(sizeof(creation)));
    return begin(api, true, wake, token);
}

bool webauthn_ceremony_start(const WebAuthnMfaRequestOptions& request, const Waker& wake, u64 token) noexcept {
    const fido2::Api* api = fido2::library();
    return api && webauthn_ceremony_start_with(*api, request, wake, token);
}

bool webauthn_ceremony_start_creation(const WebAuthnMfaCreationOptions& creation, const Waker& wake,
                                      u64 token) noexcept {
    const fido2::Api* api = fido2::library();
    return api && webauthn_ceremony_start_creation_with(*api, creation, wake, token);
}

bool webauthn_platform_available() noexcept {
    return fido2::library() != nullptr;
}

WebAuthnCeremonyState webauthn_ceremony_state() noexcept {
    return state_of(g_ceremony);
}

WebAuthnPinPrompt webauthn_ceremony_pin_prompt() noexcept {
    return g_ceremony.prompt;
}

bool webauthn_ceremony_submit_pin(ConstSlice<u8> pin) noexcept {
    Ceremony& c = g_ceremony;
    // CTAP2 PINs are 4 to 63 bytes of UTF-8 and carry no NUL.
    if (state_of(c) != WebAuthnCeremonyState::need_pin || !pin.data || pin.count < 4 || pin.count > fido2::kMaximumPinBytes)
        return false;
    for (u32 i = 0; i < pin.count; ++i)
        if (pin.data[i] == 0)
            return false;
    copy_bytes(c.pin, pin.data, pin.count);
    c.pin[pin.count] = 0;
    InterlockedExchange(&c.pin_given, 1);
    event_set(c.event);
    return true;
}

void webauthn_ceremony_cancel() noexcept {
    Ceremony& c = g_ceremony;
    if (state_of(c) == WebAuthnCeremonyState::idle)
        return;
    InterlockedExchange(&c.cancel, 1);
    lock_device(c);
    if (c.device)
        c.api->fido_dev_cancel(c.device);
    unlock_device(c);
    if (c.event)
        event_set(c.event);
}

WebAuthnCeremonyResult webauthn_ceremony_finish(SecureBuffer* serialized_credential) noexcept {
    Ceremony& c = g_ceremony;
    if (state_of(c) != WebAuthnCeremonyState::done)
        return {WebAuthnCeremonyError::invalid_input, 0};
    WebAuthnCeremonyResult result{error_of(c.outcome), c.creating ? c.made.native : c.result.native};
    if (result.ok()) {
        if (!serialized_credential || !serialized_credential->data || !serialized_credential->locked ||
            serialized_credential->capacity == 0) {
            result.error = WebAuthnCeremonyError::invalid_input;
        } else {
            secure_buffer_clear(serialized_credential);
            const WebAuthnMfaResult serialized = c.creating ? serialize_creation(c, serialized_credential)
                                                            : serialize_assertion(c, serialized_credential);
            if (serialized.ok())
                serialized_credential->bytes = serialized.bytes;
            else
                result.error = serialized.error == WebAuthnMfaError::output_too_small
                                   ? WebAuthnCeremonyError::output_too_small
                                   : WebAuthnCeremonyError::failed;
        }
    }
    reset(c);
    return result;
}

void webauthn_ceremony_release() noexcept {
    Ceremony& c = g_ceremony;
    if (state_of(c) == WebAuthnCeremonyState::idle)
        return;
    webauthn_ceremony_cancel();
    reset(c);
}

} // namespace maicord
