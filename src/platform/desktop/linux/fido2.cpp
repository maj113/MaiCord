// libfido2 behind the security-key ceremony of Linux: the dlopen and the assertion, whose steps follow what the key says.
#include "fido2.hpp"
#include "maicord/core/log_event.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/test_hook.hpp"

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

enum class Step : u8 { done, declined, unsupported, again, ended };

struct Round {
    const Api* api;
    const Hooks* hooks;
    u64 deadline;
    Outcome ended;  // the outcome when a step returns Step::ended
    i32* native;    // where the last status libfido2 returned goes
};

bool plain_cancelled(const Round& round) noexcept {
    return round.hooks->cancelled(round.hooks->context);
}

u32 clamped_timeout(u32 timeout_ms) noexcept {
    return timeout_ms == 0 ? kDefaultTimeoutMs : timeout_ms > kMaximumTimeoutMs ? kMaximumTimeoutMs : timeout_ms;
}

// What a ceremony asks of one key: `call` is libfido2's blocking call (given the PIN once the key has asked for it),
// `collect` copies the key's answer out of the object, and `declined` is the status that says this key takes no part: it
// holds none of an assertion's credentials, or one of those a creation excludes. A creation also ends when the key is
// full and when the user turned the request down on the key.
struct Operation {
    void* object;
    i32 (*call)(const Api& api, Device* device, void* object, const char* pin) noexcept;
    bool (*collect)(const Api& api, void* object) noexcept;
    i32 declined;
    bool creation;
};

// One key: the call, with the PIN asked for when the key wants one.
Step drive(Round* round, Device* device, const Operation& operation) noexcept {
    const Api& api = *round->api;
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
        const i32 status = operation.call(api, device, operation.object, have_pin ? pin : nullptr);
        *round->native = status;
        if (status == kOk) {
            if (operation.collect(api, operation.object)) {
                step = Step::done;
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
        if (status == operation.declined) {
            step = Step::declined;
            break;
        }
        if (status == kErrorUnsupportedAlgorithm) {
            step = Step::unsupported;
            break;
        }
        if (operation.creation && status == kErrorKeyStoreFull) {
            round->ended = Outcome::key_full;
            step = Step::ended;
            break;
        }
        if (operation.creation && status == kErrorOperationDenied) {
            round->ended = Outcome::cancelled;
            step = Step::ended;
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
            if (clock_monotonic_milliseconds() >= round->deadline) {
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
    return step;
}

// An assertion over one key: the request's relying party, credentials and verification.
struct AssertionJob {
    const WebAuthnMfaRequestOptions* request;
    const u8* hash;
    Result* result;
    Assertion* assertion;
};

i32 call_assertion(const Api& api, Device* device, void* object, const char* pin) noexcept {
    auto& job = *static_cast<AssertionJob*>(object);
    return api.fido_dev_get_assert(device, job.assertion, pin);
}

bool collect_assertion(const Api& api, void* object) noexcept {
    auto& job = *static_cast<AssertionJob*>(object);
    const WebAuthnMfaRequestOptions& request = *job.request;
    Assertion* assertion = job.assertion;
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
    Result* result = job.result;
    return identifier_bytes != 0 && data_bytes != 0 && signature_bytes != 0 &&
           copy_out(identifier, identifier_bytes, result->credential_id, sizeof(result->credential_id),
                    &result->credential_id_bytes) &&
           copy_out(data, data_bytes, result->authenticator_data, sizeof(result->authenticator_data),
                    &result->authenticator_data_bytes) &&
           copy_out(signature, signature_bytes, result->signature, sizeof(result->signature),
                    &result->signature_bytes) &&
           copy_out(handle, handle_bytes, result->user_handle, sizeof(result->user_handle), &result->user_handle_bytes);
}

Step ask_assertion(Round* round, Device* device, void* context) noexcept {
    auto& job = *static_cast<AssertionJob*>(context);
    const Api& api = *round->api;
    const WebAuthnMfaRequestOptions& request = *job.request;
    Assertion* assertion = api.fido_assert_new();
    if (!assertion) {
        round->ended = Outcome::failed;
        return Step::ended;
    }
    bool prepared = api.fido_assert_set_rp(assertion, request.rp_id) == kOk &&
                    api.fido_assert_set_clientdata_hash(assertion, job.hash, 32) == kOk;
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
    job.assertion = assertion;
    const Operation operation{&job, call_assertion, collect_assertion, kErrorNoCredentials, false};
    const Step step = drive(round, device, operation);
    api.fido_assert_free(&assertion);
    return step == Step::unsupported ? Step::again : step;
}

// A creation over one key: the relying party, user, algorithm and the credentials to exclude.
struct CreationJob {
    const WebAuthnMfaCreationOptions* creation;
    const u8* hash;
    MakeResult* result;
    Credential* credential;
};

i32 call_creation(const Api& api, Device* device, void* object, const char* pin) noexcept {
    auto& job = *static_cast<CreationJob*>(object);
    return api.fido_dev_make_cred(device, job.credential, pin);
}

bool collect_creation(const Api& api, void* object) noexcept {
    auto& job = *static_cast<CreationJob*>(object);
    Credential* credential = job.credential;
    const u8* identifier = api.fido_cred_id_ptr(credential);
    const usize identifier_bytes = identifier ? api.fido_cred_id_len(credential) : 0;
    const u8* encoded = api.fido_cred_authdata_ptr(credential);
    const usize encoded_bytes = encoded ? api.fido_cred_authdata_len(credential) : 0;
    const u8* statement = api.fido_cred_attstmt_ptr(credential);
    const usize statement_bytes = statement ? api.fido_cred_attstmt_len(credential) : 0;
    const char* format = api.fido_cred_fmt(credential);
    u32 format_bytes = 0;
    while (format && format[format_bytes] && format_bytes < sizeof(job.result->format) - 1)
        ++format_bytes;
    const u8* data = nullptr;
    u32 data_bytes = 0;
    authenticator_data(encoded, static_cast<u32>(encoded_bytes), &data, &data_bytes);
    MakeResult* result = job.result;
    if (identifier_bytes == 0 || data_bytes == 0 || format_bytes == 0 ||
        !copy_out(identifier, identifier_bytes, result->credential_id, sizeof(result->credential_id),
                  &result->credential_id_bytes) ||
        !copy_out(data, data_bytes, result->authenticator_data, sizeof(result->authenticator_data),
                  &result->authenticator_data_bytes) ||
        !copy_out(statement, statement_bytes, result->attestation_statement, sizeof(result->attestation_statement),
                  &result->attestation_statement_bytes))
        return false;
    copy_bytes(result->format, format, format_bytes);
    result->format[format_bytes] = 0;
    return true;
}

Step ask_creation(Round* round, Device* device, void* context) noexcept {
    auto& job = *static_cast<CreationJob*>(context);
    const Api& api = *round->api;
    const WebAuthnMfaCreationOptions& creation = *job.creation;
    const WebAuthnMfaRequestOptions& request = creation.request;
    // Discouraged and preferred ask for an ordinary credential; required asks for a resident one. User verification
    // follows the request as an assertion does.
    const i32 resident = creation.resident_key == WebAuthnResidentKey::required ? kOptionTrue : kOptionOmit;
    const i32 verification = request.user_verification == WebAuthnUserVerification::required    ? kOptionTrue
                             : request.user_verification == WebAuthnUserVerification::discouraged ? kOptionFalse
                                                                                                  : kOptionOmit;
    const Operation operation{&job, call_creation, collect_creation, kErrorCredentialExcluded, true};
    // The server's algorithms in its order: the first that this library and the key take.
    for (u32 index = 0; index < creation.algorithm_count; ++index) {
        const i32 algorithm = creation.algorithms[index];
        if (algorithm != kCoseEs256 && algorithm != kCoseEdDsa && algorithm != kCoseRs256)
            continue;
        Credential* credential = api.fido_cred_new();
        if (!credential) {
            round->ended = Outcome::failed;
            return Step::ended;
        }
        bool prepared = api.fido_cred_set_type(credential, algorithm) == kOk &&
                        api.fido_cred_set_clientdata_hash(credential, job.hash, 32) == kOk &&
                        api.fido_cred_set_rp(credential, request.rp_id, creation.rp_name[0] ? creation.rp_name : nullptr) ==
                            kOk &&
                        api.fido_cred_set_user(credential, creation.user_id, creation.user_id_bytes,
                                               creation.user_name[0] ? creation.user_name : nullptr,
                                               creation.user_display_name[0] ? creation.user_display_name : nullptr,
                                               nullptr) == kOk &&
                        api.fido_cred_set_rk(credential, resident) == kOk &&
                        api.fido_cred_set_uv(credential, verification) == kOk;
        for (u32 i = 0; prepared && i < request.credential_count; ++i) {
            const WebAuthnAllowedCredential& excluded = request.credentials[i];
            prepared = excluded.id_bytes != 0 && excluded.id_offset <= request.credential_id_bytes &&
                       excluded.id_bytes <= request.credential_id_bytes - excluded.id_offset &&
                       api.fido_cred_exclude(credential, request.credential_ids + excluded.id_offset,
                                             excluded.id_bytes) == kOk;
        }
        if (!prepared) {
            api.fido_cred_free(&credential);
            round->ended = Outcome::failed;
            return Step::ended;
        }
        job.credential = credential;
        const Step step = drive(round, device, operation);
        api.fido_cred_free(&credential);
        job.credential = nullptr;
        if (step != Step::unsupported)
            return step;
    }
    round->ended = Outcome::unsupported;
    return Step::ended;
}

using Ask = Step (*)(Round* round, Device* device, void* context) noexcept;

// Looks for keys until one takes part (`success`), every key present declines (`declined`), the ceremony ends some
// other way or the time runs out; each key present is asked by `ask`.
Outcome search(const Api& api, const Hooks& hooks, u64 deadline, i32* native, Ask ask, void* context, Outcome success,
               Outcome declined) noexcept {
    Round round{&api, &hooks, deadline, Outcome::failed, native};
    bool seen = false;       // a key was listed in some round
    u32 failed_opens = 0;    // consecutive rounds in which no listed key could be opened
    bool unreadable = false; // a key answered with an error that is no answer
    for (;;) {
        if (plain_cancelled(round))
            return Outcome::cancelled;
        if (clock_monotonic_milliseconds() >= round.deadline)
            return !seen ? Outcome::no_device : unreadable ? Outcome::failed : Outcome::timed_out;
        Paths paths;
        if (!list_devices(api, &paths))
            return Outcome::failed;
        if (paths.count == 0) {
            hooks.pause(hooks.context, kRescanMs);
            continue;
        }
        seen = true;
        u32 opened = 0, refusals = 0;
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
            const Step step = ask(&round, device, context);
            hooks.device(hooks.context, nullptr);
            api.fido_dev_close(device);
            api.fido_dev_free(&device);
            if (step == Step::done)
                return success;
            if (step == Step::ended)
                return round.ended;
            if (step == Step::declined)
                ++refusals;
            else
                again = unreadable = true;
        }
        if (opened == 0) {
            if (++failed_opens >= 3)
                return Outcome::cannot_open;
        } else {
            failed_opens = 0;
        }
        if (opened != 0 && refusals == opened && !again)
            return declined;
        hooks.pause(hooks.context, kRescanMs);
    }
}

} // namespace

const Api* library() noexcept {
    if (g_state == 1)
        return &g_api;
    if (g_state == 2)
        return nullptr;
    g_state = 2;
    // A test can name another soname (the no-library path); users and operators have MAICORD_LIBRARY_PATH.
    const char* soname = test_hook("MAICORD_FIDO2_LIBRARY");
    if (!soname || !*soname)
        soname = "libfido2.so.1";
    if (!shared_library_open(&g_library, soname)) {
        log_write(LogLevel::info, "fido2", "libfido2.so.1 is not installed: security keys are unavailable");
        return nullptr;
    }
    SharedLibraryResolver resolve{g_library, nullptr};
    Api api{};
#define MAICORD_FIDO2_RESOLVE(result, name, arguments) resolve(&api.name, #name);
    MAICORD_FIDO2_FUNCTIONS(MAICORD_FIDO2_RESOLVE)
#undef MAICORD_FIDO2_RESOLVE
    if (const char* missing = resolve.missing) {
        // libfido2 lacks a function: security keys are unavailable.
        LogEvent("library_incomplete").field("missing", missing).write(LogLevel::warning, "fido2");
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
    AssertionJob job{&request, client_data_hash, result, nullptr};
    return search(api, hooks, clock_monotonic_milliseconds() + clamped_timeout(request.timeout_ms), &result->native,
                  ask_assertion, &job, Outcome::assertion, Outcome::not_registered);
}

Outcome make(const Api& api, const WebAuthnMfaCreationOptions& creation, const u8* client_data_hash, const Hooks& hooks,
             MakeResult* result) noexcept {
    clear_bytes(result, static_cast<u32>(sizeof(*result)));
    CreationJob job{&creation, client_data_hash, result, nullptr};
    return search(api, hooks, clock_monotonic_milliseconds() + clamped_timeout(creation.request.timeout_ms),
                  &result->native, ask_creation, &job, Outcome::created, Outcome::excluded);
}

} // namespace maicord::fido2
