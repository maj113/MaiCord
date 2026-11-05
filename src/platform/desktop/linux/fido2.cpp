// libfido2 behind the security-key ceremony of Linux: the dlopen and the assertion, whose steps follow what the key says.
#include "fido2.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/core/time.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord::fido2 {
namespace {

// 0: not tried, 1: loaded, 2: not available. The ceremony asks from the UI thread and its worker, never at once.
u32 g_state;
SharedLibrary g_library;
Api g_api;

constexpr u32 kPathCapacity = 256;

struct Paths {
    char path[kMaximumDevices][kPathCapacity];
    u32 count;
};

// The devices of the manifest, their paths copied out so that the list can be freed.
bool list_devices(const Api& api, Paths* paths) noexcept {
    paths->count = 0;
    DeviceInfo* list = api.fido_dev_info_new(kMaximumDevices);
    if (!list)
        return false;
    usize found = 0;
    if (api.fido_dev_info_manifest(list, kMaximumDevices, &found) == kOk) {
        for (usize i = 0; i < found && paths->count < kMaximumDevices; ++i) {
            const DeviceInfo* info = api.fido_dev_info_ptr(list, i);
            const char* path = info ? api.fido_dev_info_path(info) : nullptr;
            if (!path || !path[0])
                continue;
            u32 length = 0;
            while (path[length] && length + 1 < kPathCapacity) {
                paths->path[paths->count][length] = path[length];
                ++length;
            }
            paths->path[paths->count][length] = 0;
            ++paths->count;
        }
    }
    api.fido_dev_info_free(&list, kMaximumDevices);
    return true;
}

// Authenticator data comes back as a CBOR byte string: the header off, the bytes of the data left. Data that is not a byte
// string whose length matches is taken as it is.
void authenticator_data(const u8* encoded, u32 bytes, const u8** data, u32* count) noexcept {
    *data = encoded;
    *count = bytes;
    if (bytes < 2)
        return;
    const u8 initial = encoded[0];
    u32 header = 0, length = 0;
    if (initial >= 0x40 && initial <= 0x57) {
        header = 1;
        length = initial & 0x1Fu;
    } else if (initial == 0x58) {
        header = 2;
        length = encoded[1];
    } else if (initial == 0x59 && bytes >= 3) {
        header = 3;
        length = static_cast<u32>(encoded[1]) << 8 | encoded[2];
    }
    if (header && header + length == bytes) {
        *data = encoded + header;
        *count = length;
    }
}

bool copy_out(const u8* source, usize bytes, u8* destination, u32 capacity, u32* count) noexcept {
    if (bytes > capacity)
        return false;
    if (bytes)
        copy_bytes(destination, source, static_cast<u32>(bytes));
    *count = static_cast<u32>(bytes);
    return true;
}

enum class Step : u8 { assertion, not_registered, again, ended };

struct Round {
    const Api* api;
    const WebAuthnMfaRequestOptions* request;
    const u8* hash;
    const Hooks* hooks;
    Result* result;
    u64 deadline;
    Outcome ended;  // the outcome when a step returns Step::ended
};

u64 now() noexcept {
    return clock_monotonic_milliseconds();
}

bool plain_cancelled(const Round& round) noexcept {
    return round.hooks->cancelled(round.hooks->context);
}

// One key: the assertion, with the PIN asked for when the key wants one.
Step ask(Round* round, Device* device) noexcept {
    const Api& api = *round->api;
    const WebAuthnMfaRequestOptions& request = *round->request;
    Assertion* assertion = api.fido_assert_new();
    if (!assertion) {
        round->ended = Outcome::failed;
        return Step::ended;
    }
    bool prepared = api.fido_assert_set_rp(assertion, request.rp_id) == kOk &&
                    api.fido_assert_set_clientdata_hash(assertion, round->hash, 32) == kOk;
    for (u32 i = 0; prepared && i < request.credential_count; ++i) {
        const WebAuthnAllowedCredential& credential = request.credentials[i];
        prepared = credential.id_bytes != 0 && credential.id_offset <= request.credential_id_bytes &&
                   credential.id_bytes <= request.credential_id_bytes - credential.id_offset &&
                   api.fido_assert_allow_cred(assertion, request.credential_ids + credential.id_offset,
                                              credential.id_bytes) == kOk;
    }
    // Discouraged asks for no verification; required asks for it; preferred leaves the choice to the key.
    const i32 verification = request.user_verification == WebAuthnUserVerification::required    ? kOptionTrue
                             : request.user_verification == WebAuthnUserVerification::discouraged ? kOptionFalse
                                                                                                  : kOptionOmit;
    prepared = prepared && api.fido_assert_set_uv(assertion, verification) == kOk;
    if (!prepared) {
        api.fido_assert_free(&assertion);
        round->ended = Outcome::failed;
        return Step::ended;
    }

    char pin[kMaximumPinBytes + 1] = {};
    bool have_pin = false;
    bool wrong = false;
    i32 retries = -1;
    Step step = Step::again;
    for (;;) {
        if (plain_cancelled(*round)) {
            round->ended = Outcome::cancelled;
            step = Step::ended;
            break;
        }
        const i32 status = api.fido_dev_get_assert(device, assertion, have_pin ? pin : nullptr);
        round->result->native = status;
        if (status == kOk) {
            const usize count = api.fido_assert_count(assertion);
            const u8* identifier = count ? api.fido_assert_id_ptr(assertion, 0) : nullptr;
            usize identifier_bytes = identifier ? api.fido_assert_id_len(assertion, 0) : 0;
            const u8* encoded = count ? api.fido_assert_authdata_ptr(assertion, 0) : nullptr;
            const usize encoded_bytes = encoded ? api.fido_assert_authdata_len(assertion, 0) : 0;
            const u8* signature = count ? api.fido_assert_sig_ptr(assertion, 0) : nullptr;
            const usize signature_bytes = signature ? api.fido_assert_sig_len(assertion, 0) : 0;
            const u8* handle = count ? api.fido_assert_user_id_ptr(assertion, 0) : nullptr;
            const usize handle_bytes = handle ? api.fido_assert_user_id_len(assertion, 0) : 0;
            // A key may leave the credential id out when the request allowed exactly one.
            if (identifier_bytes == 0 && request.credential_count == 1) {
                identifier = request.credential_ids + request.credentials[0].id_offset;
                identifier_bytes = request.credentials[0].id_bytes;
            }
            const u8* data = nullptr;
            u32 data_bytes = 0;
            authenticator_data(encoded, static_cast<u32>(encoded_bytes), &data, &data_bytes);
            Result* result = round->result;
            const bool ok = identifier_bytes != 0 && data_bytes != 0 && signature_bytes != 0 &&
                            copy_out(identifier, identifier_bytes, result->credential_id, sizeof(result->credential_id),
                                     &result->credential_id_bytes) &&
                            copy_out(data, data_bytes, result->authenticator_data, sizeof(result->authenticator_data),
                                     &result->authenticator_data_bytes) &&
                            copy_out(signature, signature_bytes, result->signature, sizeof(result->signature),
                                     &result->signature_bytes) &&
                            copy_out(handle, handle_bytes, result->user_handle, sizeof(result->user_handle),
                                     &result->user_handle_bytes);
            if (ok) {
                step = Step::assertion;
            } else {
                round->ended = Outcome::failed;
                step = Step::ended;
            }
            break;
        }
        if (status == kErrorKeepaliveCancel) {
            round->ended = Outcome::cancelled;
            step = Step::ended;
            break;
        }
        if (status == kErrorNoCredentials) {
            step = Step::not_registered;
            break;
        }
        if (status == kErrorPinBlocked || status == kErrorPinAuthBlocked || status == kErrorUvBlocked) {
            round->ended = Outcome::pin_blocked;
            step = Step::ended;
            break;
        }
        if (status == kErrorPinNotSet) {
            round->ended = Outcome::pin_not_set;
            step = Step::ended;
            break;
        }
        const bool refused = status == kErrorPinInvalid || status == kErrorPinAuthInvalid;
        if (status == kErrorPinRequired || refused) {
            wrong = refused;
            retries = -1;
            if (refused)
                api.fido_dev_get_retry_count(device, &retries);
            if (!round->hooks->pin(round->hooks->context, wrong, retries, pin, sizeof(pin))) {
                round->ended = plain_cancelled(*round) ? Outcome::cancelled : Outcome::timed_out;
                step = Step::ended;
                break;
            }
            have_pin = true;
            continue;
        }
        if (status == kErrorActionTimeout || status == kErrorUserActionTimeout || status == kErrorTimeout) {
            // The key stopped waiting for a touch; ask again until the request's own time runs out.
            if (now() >= round->deadline) {
                round->ended = Outcome::timed_out;
                step = Step::ended;
                break;
            }
            continue;
        }
        // The key went away or answered something else: another look at the keys decides.
        step = Step::again;
        break;
    }
    secure_clear_bytes(pin, sizeof(pin));
    api.fido_assert_free(&assertion);
    return step;
}

} // namespace

const Api* library() noexcept {
    if (g_state == 1)
        return &g_api;
    if (g_state == 2)
        return nullptr;
    g_state = 2;
    // MAICORD_FIDO2_LIBRARY replaces the soname (an unusual install, or a test of the no-library path).
    const char* soname = environment_get("MAICORD_FIDO2_LIBRARY");
    if (!soname || !*soname)
        soname = "libfido2.so.1";
    if (!shared_library_open(&g_library, soname)) {
        log_write(LogLevel::info, "fido2", "libfido2.so.1 is not installed: security keys are unavailable");
        return nullptr;
    }
    const char* missing = nullptr;
    Api api{};
#define MAICORD_FIDO2_RESOLVE(result, name, arguments)                                                                 \
    if (!missing) {                                                                                                    \
        void* address = shared_library_symbol(g_library, #name);                                                       \
        if (address)                                                                                                   \
            api.name = reinterpret_cast<decltype(api.name)>(address);                                                  \
        else                                                                                                           \
            missing = #name;                                                                                           \
    }
    MAICORD_FIDO2_FUNCTIONS(MAICORD_FIDO2_RESOLVE)
#undef MAICORD_FIDO2_RESOLVE
    if (missing) {
        log_write(LogLevel::warning, "fido2", "libfido2 lacks a function: security keys are unavailable");
        log_write(LogLevel::warning, "fido2", missing);
        shared_library_close(&g_library);
        return nullptr;
    }
    api.fido_init(0);
    g_api = api;
    g_state = 1;
    return &g_api;
}

Outcome run(const Api& api, const WebAuthnMfaRequestOptions& request, const u8* client_data_hash, const Hooks& hooks,
            Result* result) noexcept {
    clear_bytes(result, static_cast<u32>(sizeof(*result)));
    const u32 timeout = request.timeout_ms == 0 ? kDefaultTimeoutMs
                        : request.timeout_ms > kMaximumTimeoutMs ? kMaximumTimeoutMs
                                                                  : request.timeout_ms;
    Round round{&api, &request, client_data_hash, &hooks, result, now() + timeout, Outcome::failed};
    bool seen = false;       // a key was listed in some round
    u32 failed_opens = 0;    // consecutive rounds in which no listed key could be opened
    bool unreadable = false; // a key answered with an error that is no answer
    for (;;) {
        if (plain_cancelled(round))
            return Outcome::cancelled;
        if (now() >= round.deadline)
            return !seen ? Outcome::no_device : unreadable ? Outcome::failed : Outcome::timed_out;
        Paths paths;
        if (!list_devices(api, &paths))
            return Outcome::failed;
        if (paths.count == 0) {
            hooks.pause(hooks.context, kRescanMs);
            continue;
        }
        seen = true;
        u32 opened = 0, unregistered = 0;
        bool again = false;
        for (u32 i = 0; i < paths.count; ++i) {
            Device* device = api.fido_dev_new();
            if (!device)
                return Outcome::failed;
            if (api.fido_dev_open(device, paths.path[i]) != kOk) {
                api.fido_dev_free(&device);
                continue;
            }
            ++opened;
            hooks.device(hooks.context, device);
            const Step step = ask(&round, device);
            hooks.device(hooks.context, nullptr);
            api.fido_dev_close(device);
            api.fido_dev_free(&device);
            if (step == Step::assertion)
                return Outcome::assertion;
            if (step == Step::ended)
                return round.ended;
            if (step == Step::not_registered)
                ++unregistered;
            else
                again = unreadable = true;
        }
        if (opened == 0) {
            if (++failed_opens >= 3)
                return Outcome::cannot_open;
        } else {
            failed_opens = 0;
        }
        if (opened != 0 && unregistered == opened && !again)
            return Outcome::not_registered;
        hooks.pause(hooks.context, kRescanMs);
    }
}

} // namespace maicord::fido2
