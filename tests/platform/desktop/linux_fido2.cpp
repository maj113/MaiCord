// The security-key assertion of Linux (src/platform/desktop/linux/fido2.cpp and webauthn_ceremony.cpp) against a fake libfido2:
// a table of entry points that plays a key. It checks what the library is asked (the relying party, the client data hash, the
// allowed credentials, user verification as the request says), what comes back into the credential, and how the steps follow
// the key: a PIN asked for, a wrong PIN, a blocked PIN, no key plugged in, a key without the credential, a touch that never
// comes, a cancel while the key waits. The worker-thread ceremony runs the same scripts, and a missing libfido2 is a state.
//
//   --no-library   MAICORD_FIDO2_LIBRARY names a library that does not exist: security keys are unavailable
//
// Real hardware is not exercised here: the fake's answers are what libfido2 documents, not what a key was seen to say.
#include "../../../src/platform/desktop/linux/fido2.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/protocol/auth/webauthn_mfa.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord {

// The fake's objects (the library's opaque types).
namespace fido2 {
struct Device {
    u32 index;
    bool open;
};
struct DeviceInfo {
    u32 count;
};
struct Assertion {
    char rp[64];
    u8 hash[32];
    bool hash_set;
    u32 credentials;
    i32 uv;
};
struct Credential {
    char rp[64], rp_name[32], user_name[32];
    u8 hash[32], user_id[64];
    u32 user_id_bytes, excluded;
    i32 type, rk, uv;
};
} // namespace fido2

namespace {

u32 g_failures;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

bool same_text(const char* a, const char* b) noexcept {
    u32 i = 0;
    while (a[i] && a[i] == b[i])
        ++i;
    return a[i] == b[i];
}

bool contains(const char* text, u32 bytes, const char* needle) noexcept {
    u32 length = 0;
    while (needle[length])
        ++length;
    for (u32 i = 0; i + length <= bytes; ++i) {
        u32 j = 0;
        while (j < length && text[i + j] == needle[j])
            ++j;
        if (j == length)
            return true;
    }
    return false;
}

// The key -----------------------------------------------------------------------------------------------------------------

enum class Mode : u8 {
    plain,           // answers at once
    needs_pin,       // PIN_REQUIRED without a PIN; the PIN is 1234; three wrong ones block it
    no_credentials,  // NO_CREDENTIALS
    never_touched,   // ACTION_TIMEOUT each time
    blocks,          // waits for fido_dev_cancel
    uv_without_pin,  // PIN_NOT_SET
    gone,            // TX error: the key went away
    excluded,        // CREDENTIAL_EXCLUDED (a creation)
    key_full,        // KEY_STORE_FULL (a creation)
    denied,          // OPERATION_DENIED (a creation: the user turned it down on the key)
    unsupported_es256, // UNSUPPORTED_ALGORITHM for ES256, then accepts (a creation)
    unsupported_all, // UNSUPPORTED_ALGORITHM for every algorithm (a creation)
};

struct Fake {
    Mode mode;
    u32 devices;        // what the manifest lists
    bool open_fails;
    volatile LONG in_assert;   // a get_assert is waiting
    volatile LONG cancelled;   // fido_dev_cancel came
    u32 manifests, opens, closes, asserts, cancels;
    u32 wrong_pins;
    i32 retries;
    // What the library was asked.
    char rp[64];
    u8 hash[32];
    u32 allowed;
    i32 uv;
    char pins[8][8];
    u32 pin_count;
    // What a creation asked of the library, beside what a sign-in does.
    i32 type, rk;
    u32 makes, user_id_bytes;
    char rp_name[32], user_name[32];
};
Fake g_fake;

fido2::Device g_device;
fido2::DeviceInfo g_info;
fido2::Assertion g_assertion;
fido2::Credential g_credential;

void forget(Mode mode) noexcept {
    clear_bytes(&g_fake, static_cast<u32>(sizeof(g_fake)));
    clear_bytes(&g_assertion, static_cast<u32>(sizeof(g_assertion)));
    clear_bytes(&g_credential, static_cast<u32>(sizeof(g_credential)));
    g_fake.mode = mode;
    g_fake.devices = 1;
    g_fake.retries = 3;
}

void fido_init_fake(i32) noexcept {}

fido2::DeviceInfo* info_new(usize) noexcept {
    g_info.count = g_fake.devices;
    return &g_info;
}
void info_free(fido2::DeviceInfo** list, usize) noexcept {
    *list = nullptr;
}
i32 info_manifest(fido2::DeviceInfo*, usize capacity, usize* found) noexcept {
    ++g_fake.manifests;
    *found = g_fake.devices < capacity ? g_fake.devices : capacity;
    return fido2::kOk;
}
const fido2::DeviceInfo* info_ptr(const fido2::DeviceInfo* list, usize) noexcept {
    return list;
}
const char* info_path(const fido2::DeviceInfo*) noexcept {
    return "/dev/hidraw-fake";
}
fido2::Device* dev_new() noexcept {
    return &g_device;
}
void dev_free(fido2::Device** device) noexcept {
    *device = nullptr;
}
i32 dev_open(fido2::Device* device, const char*) noexcept {
    ++g_fake.opens;
    if (g_fake.open_fails)
        return fido2::kErrorInternal;
    device->open = true;
    return fido2::kOk;
}
i32 dev_close(fido2::Device* device) noexcept {
    ++g_fake.closes;
    device->open = false;
    return fido2::kOk;
}
i32 dev_cancel(fido2::Device*) noexcept {
    ++g_fake.cancels;
    InterlockedExchange(&g_fake.cancelled, 1);
    return fido2::kOk;
}
i32 dev_retries(fido2::Device*, i32* retries) noexcept {
    *retries = g_fake.retries;
    return fido2::kOk;
}

// The answer data: a credential id, authenticator data as the CBOR byte string libfido2 returns (a header and 37 bytes), a
// signature and a user handle.
const u8 kId[9] = {'c', 'r', 'e', 'd', '-', 'i', 'd', '-', '1'};
u8 g_authdata[2 + 37];
u8 g_signature[64];
const u8 kUser[4] = {'u', 's', 'e', 'r'};

i32 dev_get_assert(fido2::Device*, fido2::Assertion* assertion, const char* pin) noexcept {
    ++g_fake.asserts;
    copy_bytes(g_fake.rp, assertion->rp, sizeof(g_fake.rp));
    copy_bytes(g_fake.hash, assertion->hash, 32);
    g_fake.allowed = assertion->credentials;
    g_fake.uv = assertion->uv;
    if (pin && g_fake.pin_count < 8) {
        u32 i = 0;
        for (; pin[i] && i < 7; ++i)
            g_fake.pins[g_fake.pin_count][i] = pin[i];
        g_fake.pins[g_fake.pin_count][i] = 0;
        ++g_fake.pin_count;
    }
    switch (g_fake.mode) {
    case Mode::plain:
        return fido2::kOk;
    case Mode::needs_pin:
        if (!pin)
            return fido2::kErrorPinRequired;
        if (same_text(pin, "1234"))
            return fido2::kOk;
        if (--g_fake.retries <= 0)
            return fido2::kErrorPinBlocked;
        return fido2::kErrorPinInvalid;
    case Mode::no_credentials:
        return fido2::kErrorNoCredentials;
    case Mode::never_touched:
        thread_sleep(10);
        return fido2::kErrorActionTimeout;
    case Mode::uv_without_pin:
        return fido2::kErrorPinNotSet;
    case Mode::gone:
        return fido2::kErrorTransmit;
    case Mode::excluded:
    case Mode::key_full:
    case Mode::denied:
    case Mode::unsupported_es256:
    case Mode::unsupported_all:
        return fido2::kErrorInternal;
    case Mode::blocks:
        InterlockedExchange(&g_fake.in_assert, 1);
        while (InterlockedCompareExchange(&g_fake.cancelled, 0, 0) == 0)
            thread_sleep(2);
        InterlockedExchange(&g_fake.in_assert, 0);
        return fido2::kErrorKeepaliveCancel;
    }
    return fido2::kErrorInternal;
}

fido2::Assertion* assert_new() noexcept {
    clear_bytes(&g_assertion, static_cast<u32>(sizeof(g_assertion)));
    g_assertion.uv = -1;
    return &g_assertion;
}
void assert_free(fido2::Assertion** assertion) noexcept {
    *assertion = nullptr;
}
i32 assert_set_rp(fido2::Assertion* assertion, const char* rp) noexcept {
    u32 i = 0;
    for (; rp[i] && i < 63; ++i)
        assertion->rp[i] = rp[i];
    assertion->rp[i] = 0;
    return fido2::kOk;
}
i32 assert_set_hash(fido2::Assertion* assertion, const u8* hash, usize bytes) noexcept {
    if (bytes != 32)
        return fido2::kErrorInternal;
    copy_bytes(assertion->hash, hash, 32);
    assertion->hash_set = true;
    return fido2::kOk;
}
i32 assert_allow(fido2::Assertion* assertion, const u8* id, usize bytes) noexcept {
    if (!id || bytes != sizeof(kId) || !bytes_equal(id, kId, sizeof(kId)))
        return fido2::kErrorInternal;
    ++assertion->credentials;
    return fido2::kOk;
}
i32 assert_set_uv(fido2::Assertion* assertion, i32 option) noexcept {
    assertion->uv = option;
    return fido2::kOk;
}
usize assert_count(const fido2::Assertion*) noexcept {
    return 1;
}
const u8* assert_id(const fido2::Assertion*, usize) noexcept {
    return kId;
}
usize assert_id_len(const fido2::Assertion*, usize) noexcept {
    return sizeof(kId);
}
const u8* assert_authdata(const fido2::Assertion*, usize) noexcept {
    return g_authdata;
}
usize assert_authdata_len(const fido2::Assertion*, usize) noexcept {
    return sizeof(g_authdata);
}
const u8* assert_sig(const fido2::Assertion*, usize) noexcept {
    return g_signature;
}
usize assert_sig_len(const fido2::Assertion*, usize) noexcept {
    return sizeof(g_signature);
}
const u8* assert_user(const fido2::Assertion*, usize) noexcept {
    return kUser;
}
usize assert_user_len(const fido2::Assertion*, usize) noexcept {
    return sizeof(kUser);
}

// What a creation builds and what the key answers to it: the authenticator data as libfido2 returns it (a CBOR byte string:
// a two-byte header and 75 bytes of rpIdHash, flags with the AT bit, a counter, an AAGUID, the id and a COSE key) and a
// one-entry attestation statement.
u8 g_make_authdata[2 + 75];
const u8 kStatement[6] = {0xA1, 0x63, 'a', 'l', 'g', 0x26};

void build_make_data() noexcept {
    clear_bytes(g_make_authdata, sizeof(g_make_authdata));
    g_make_authdata[0] = 0x58;
    g_make_authdata[1] = 75;
    u8* data = g_make_authdata + 2;
    for (u32 i = 0; i < 32; ++i)
        data[i] = static_cast<u8>(0x10 + i);
    data[32] = 0x41;
    for (u32 i = 37; i < 53; ++i)
        data[i] = 0x77;
    data[54] = sizeof(kId);
    copy_bytes(data + 55, kId, sizeof(kId));
    const u8 cose[11] = {0xA4, 0x01, 0x02, 0x03, 0x26, 0x20, 0x01, 0x21, 0x42, 9, 9};
    copy_bytes(data + 64, cose, sizeof(cose));
}

fido2::Credential* cred_new() noexcept {
    clear_bytes(&g_credential, static_cast<u32>(sizeof(g_credential)));
    g_credential.uv = g_credential.rk = -1;
    return &g_credential;
}
void cred_free(fido2::Credential** credential) noexcept {
    *credential = nullptr;
}
i32 cred_set_type(fido2::Credential* credential, i32 algorithm) noexcept {
    credential->type = algorithm;
    return fido2::kOk;
}
i32 cred_set_hash(fido2::Credential* credential, const u8* hash, usize bytes) noexcept {
    if (bytes != 32)
        return fido2::kErrorInternal;
    copy_bytes(credential->hash, hash, 32);
    return fido2::kOk;
}
i32 cred_set_rp(fido2::Credential* credential, const char* id, const char* name) noexcept {
    u32 i = 0;
    for (; id[i] && i < 63; ++i)
        credential->rp[i] = id[i];
    credential->rp[i] = 0;
    for (i = 0; name && name[i] && i < 31; ++i)
        credential->rp_name[i] = name[i];
    return fido2::kOk;
}
i32 cred_set_user(fido2::Credential* credential, const u8* id, usize id_bytes, const char* name, const char*,
                  const char*) noexcept {
    if (!id || id_bytes == 0 || id_bytes > sizeof(credential->user_id))
        return fido2::kErrorInternal;
    copy_bytes(credential->user_id, id, static_cast<u32>(id_bytes));
    credential->user_id_bytes = static_cast<u32>(id_bytes);
    for (u32 i = 0; name && name[i] && i < 31; ++i)
        credential->user_name[i] = name[i];
    return fido2::kOk;
}
i32 cred_set_rk(fido2::Credential* credential, i32 option) noexcept {
    credential->rk = option;
    return fido2::kOk;
}
i32 cred_set_uv(fido2::Credential* credential, i32 option) noexcept {
    credential->uv = option;
    return fido2::kOk;
}
i32 cred_exclude(fido2::Credential* credential, const u8* id, usize bytes) noexcept {
    if (!id || bytes != sizeof(kId) || !bytes_equal(id, kId, sizeof(kId)))
        return fido2::kErrorInternal;
    ++credential->excluded;
    return fido2::kOk;
}

i32 dev_make_cred(fido2::Device*, fido2::Credential* credential, const char* pin) noexcept {
    ++g_fake.makes;
    copy_bytes(g_fake.rp, credential->rp, sizeof(g_fake.rp));
    copy_bytes(g_fake.rp_name, credential->rp_name, sizeof(g_fake.rp_name));
    copy_bytes(g_fake.hash, credential->hash, 32);
    copy_bytes(g_fake.user_name, credential->user_name, sizeof(g_fake.user_name));
    g_fake.user_id_bytes = credential->user_id_bytes;
    g_fake.type = credential->type;
    g_fake.allowed = credential->excluded;
    g_fake.uv = credential->uv;
    g_fake.rk = credential->rk;
    if (pin && g_fake.pin_count < 8) {
        u32 i = 0;
        for (; pin[i] && i < 7; ++i)
            g_fake.pins[g_fake.pin_count][i] = pin[i];
        g_fake.pins[g_fake.pin_count][i] = 0;
        ++g_fake.pin_count;
    }
    switch (g_fake.mode) {
    case Mode::plain:
        return fido2::kOk;
    case Mode::needs_pin:
        if (!pin)
            return fido2::kErrorPinRequired;
        if (same_text(pin, "1234"))
            return fido2::kOk;
        if (--g_fake.retries <= 0)
            return fido2::kErrorPinBlocked;
        return fido2::kErrorPinInvalid;
    case Mode::excluded:
        return fido2::kErrorCredentialExcluded;
    case Mode::key_full:
        return fido2::kErrorKeyStoreFull;
    case Mode::denied:
        return fido2::kErrorOperationDenied;
    case Mode::unsupported_es256:
        return credential->type == fido2::kCoseEs256 ? fido2::kErrorUnsupportedAlgorithm : fido2::kOk;
    case Mode::unsupported_all:
        return fido2::kErrorUnsupportedAlgorithm;
    case Mode::never_touched:
        thread_sleep(10);
        return fido2::kErrorActionTimeout;
    case Mode::uv_without_pin:
        return fido2::kErrorPinNotSet;
    case Mode::gone:
        return fido2::kErrorTransmit;
    case Mode::blocks:
        InterlockedExchange(&g_fake.in_assert, 1);
        while (InterlockedCompareExchange(&g_fake.cancelled, 0, 0) == 0)
            thread_sleep(2);
        InterlockedExchange(&g_fake.in_assert, 0);
        return fido2::kErrorKeepaliveCancel;
    case Mode::no_credentials:
        break;
    }
    return fido2::kErrorInternal;
}

const u8* cred_id(const fido2::Credential*) noexcept {
    return kId;
}
usize cred_id_len(const fido2::Credential*) noexcept {
    return sizeof(kId);
}
const u8* cred_authdata(const fido2::Credential*) noexcept {
    return g_make_authdata;
}
usize cred_authdata_len(const fido2::Credential*) noexcept {
    return sizeof(g_make_authdata);
}
const u8* cred_attstmt(const fido2::Credential*) noexcept {
    return kStatement;
}
usize cred_attstmt_len(const fido2::Credential*) noexcept {
    return sizeof(kStatement);
}
const char* cred_fmt(const fido2::Credential*) noexcept {
    return "packed";
}

const fido2::Api g_api = {
    fido_init_fake, info_new, info_free, info_manifest, info_ptr, info_path, dev_new, dev_free, dev_open, dev_close,
    dev_cancel, dev_retries, dev_get_assert, assert_new, assert_free, assert_set_rp, assert_set_hash,
    assert_allow, assert_set_uv, assert_count, assert_id, assert_id_len, assert_authdata,
    assert_authdata_len, assert_sig, assert_sig_len, assert_user, assert_user_len,
    cred_new, cred_free, cred_set_type, cred_set_hash, cred_set_rp, cred_set_user, cred_set_rk, cred_set_uv,
    cred_exclude, dev_make_cred, cred_id, cred_id_len, cred_authdata, cred_authdata_len, cred_attstmt,
    cred_attstmt_len, cred_fmt};

// The request -------------------------------------------------------------------------------------------------------------

constexpr char kRequestJson[] =
    R"({"publicKey":{"challenge":"AQIDBA","timeout":600,"rpId":"discord.com","allowCredentials":[{"id":"Y3JlZC1pZC0x",)"
    R"("type":"public-key","transports":["usb"]}],"userVerification":"preferred"}})";

WebAuthnMfaRequestOptions g_request;

bool make_request(u32 timeout_ms, WebAuthnUserVerification verification) noexcept {
    u32 structurals[512]{};
    clear_bytes(&g_request, static_cast<u32>(sizeof(g_request)));
    const WebAuthnMfaResult decoded = webauthn_mfa_decode_request(
        Utf8View{kRequestJson, static_cast<u32>(sizeof(kRequestJson) - 1)}, Slice<u32>{structurals, 512}, 16, &g_request);
    g_request.timeout_ms = timeout_ms;
    g_request.user_verification = verification;
    return decoded.ok() && g_request.credential_count == 1 && g_request.credentials[0].id_bytes == sizeof(kId);
}

// The surroundings of fido2::run -------------------------------------------------------------------------------------------

struct Surroundings {
    u32 pins_asked;
    bool last_wrong;
    i32 last_retries;
    const char* answers[4];   // the PINs to give, in order; null: the user gives up
    u32 given;
    u32 devices_seen;         // fido_dev_cancel target changes
};

bool never(void*) noexcept {
    return false;
}
void note_device(void* context, fido2::Device* device) noexcept {
    if (device)
        ++static_cast<Surroundings*>(context)->devices_seen;
}
bool answer_pin(void* context, bool wrong, i32 retries, char* pin, u32 capacity) noexcept {
    auto& s = *static_cast<Surroundings*>(context);
    ++s.pins_asked;
    s.last_wrong = wrong;
    s.last_retries = retries;
    const char* answer = s.given < 4 ? s.answers[s.given++] : nullptr;
    if (!answer)
        return false;
    u32 i = 0;
    for (; answer[i] && i + 1 < capacity; ++i)
        pin[i] = answer[i];
    pin[i] = 0;
    return true;
}
void short_pause(void*, u32 milliseconds) noexcept {
    thread_sleep(milliseconds < 15 ? milliseconds : 15);
}

fido2::Outcome run(Surroundings* s, fido2::Result* result) noexcept {
    u8 hash[32];
    for (u32 i = 0; i < 32; ++i)
        hash[i] = static_cast<u8>(0x40 + i);
    const fido2::Hooks hooks{s, never, note_device, answer_pin, short_pause};
    return fido2::run(g_api, g_request, hash, hooks, result);
}

void test_assertion() noexcept {
    for (u32 i = 0; i < sizeof(g_authdata); ++i)
        g_authdata[i] = i < 2 ? (i == 0 ? 0x58 : 37) : static_cast<u8>(i);
    for (u32 i = 0; i < sizeof(g_signature); ++i)
        g_signature[i] = static_cast<u8>(0xA0 + i);

    // The request reaches the library as it is, and the answer comes back whole.
    forget(Mode::plain);
    check(make_request(600, WebAuthnUserVerification::preferred), "the request decodes");
    Surroundings s{};
    fido2::Result result{};
    check(run(&s, &result) == fido2::Outcome::assertion, "a key that answers gives the assertion");
    check(same_text(g_fake.rp, "discord.com") && g_fake.allowed == 1 && g_fake.uv == fido2::kOptionOmit &&
              g_fake.hash[0] == 0x40 && g_fake.hash[31] == 0x5F,
          "the relying party, the allowed credential, the client data hash and 'preferred' reach the library");
    check(result.credential_id_bytes == sizeof(kId) && bytes_equal(result.credential_id, kId, sizeof(kId)) &&
              result.authenticator_data_bytes == 37 && result.authenticator_data[0] == 2 &&
              result.authenticator_data[36] == 38 && result.signature_bytes == 64 && result.signature[0] == 0xA0 &&
              result.user_handle_bytes == 4 && result.user_handle[0] == 'u',
          "the id, the authenticator data without its CBOR header, the signature and the user handle come back");
    check(g_fake.opens == 1 && g_fake.closes == 1 && s.pins_asked == 0, "the key was opened once, closed, and no PIN asked");

    // User verification as the request says.
    forget(Mode::plain);
    make_request(600, WebAuthnUserVerification::required);
    run(&s, &result);
    check(g_fake.uv == fido2::kOptionTrue, "'required' asks the key to verify the user");
    forget(Mode::plain);
    make_request(600, WebAuthnUserVerification::discouraged);
    run(&s, &result);
    check(g_fake.uv == fido2::kOptionFalse, "'discouraged' asks for no verification");
}

void test_pin() noexcept {
    // The key wants a PIN: asked once, and given.
    forget(Mode::needs_pin);
    make_request(600, WebAuthnUserVerification::required);
    Surroundings s{};
    s.answers[0] = "1234";
    fido2::Result result{};
    check(run(&s, &result) == fido2::Outcome::assertion && s.pins_asked == 1 && !s.last_wrong && g_fake.pin_count == 1 &&
              same_text(g_fake.pins[0], "1234"),
          "a key that wants a PIN is asked for it once, then answers with it");

    // A wrong PIN is asked for again, with the attempts the key has left.
    forget(Mode::needs_pin);
    clear_bytes(&s, static_cast<u32>(sizeof(s)));
    s.answers[0] = "0000";
    s.answers[1] = "1234";
    check(run(&s, &result) == fido2::Outcome::assertion && s.pins_asked == 2 && s.last_wrong && s.last_retries == 2,
          "a wrong PIN is asked for again, saying so and how many attempts are left");

    // Three wrong ones block it.
    forget(Mode::needs_pin);
    clear_bytes(&s, static_cast<u32>(sizeof(s)));
    s.answers[0] = "0000";
    s.answers[1] = "1111";
    s.answers[2] = "2222";
    s.answers[3] = "3333";
    check(run(&s, &result) == fido2::Outcome::pin_blocked, "a blocked PIN ends the ceremony");

    // Giving up at the PIN prompt.
    forget(Mode::needs_pin);
    clear_bytes(&s, static_cast<u32>(sizeof(s)));
    check(run(&s, &result) == fido2::Outcome::timed_out && s.pins_asked == 1,
          "a PIN prompt nobody answers ends the request");

    // A key that must verify the user and has no PIN.
    forget(Mode::uv_without_pin);
    check(run(&s, &result) == fido2::Outcome::pin_not_set, "a key with no PIN for a verification it must make");
}

void test_keys() noexcept {
    fido2::Result result{};
    Surroundings s{};

    forget(Mode::plain);
    g_fake.devices = 0;
    make_request(150, WebAuthnUserVerification::preferred);
    check(run(&s, &result) == fido2::Outcome::no_device && g_fake.manifests > 2,
          "no key plugged in before the time runs out: no device, after looking again and again");

    forget(Mode::no_credentials);
    make_request(600, WebAuthnUserVerification::preferred);
    check(run(&s, &result) == fido2::Outcome::not_registered, "a key without the credential: not registered");

    forget(Mode::never_touched);
    make_request(150, WebAuthnUserVerification::preferred);
    check(run(&s, &result) == fido2::Outcome::timed_out && g_fake.asserts > 1,
          "a key that is never touched: asked again until the request's time is out, then timed out");

    forget(Mode::plain);
    g_fake.open_fails = true;
    make_request(2000, WebAuthnUserVerification::preferred);
    check(run(&s, &result) == fido2::Outcome::cannot_open && g_fake.opens >= 3,
          "keys that cannot be opened: cannot open, after a few tries");

    forget(Mode::gone);
    make_request(150, WebAuthnUserVerification::preferred);
    check(run(&s, &result) == fido2::Outcome::failed && g_fake.opens > 1,
          "a key that goes away is looked for again, and the failure is told when time runs out");
}

// The ceremony -------------------------------------------------------------------------------------------------------------

WebAuthnCeremonyState wait_state(WebAuthnCeremonyState wanted, u32 milliseconds) noexcept {
    const u64 end = clock_monotonic_milliseconds() + milliseconds;
    while (webauthn_ceremony_state() != wanted && clock_monotonic_milliseconds() < end)
        thread_sleep(2);
    return webauthn_ceremony_state();
}

bool start(u32 timeout_ms) noexcept {
    make_request(timeout_ms, WebAuthnUserVerification::required);
    return webauthn_ceremony_start_with(g_api, g_request, Waker{}, 77);
}

void test_ceremony() noexcept {
    for (u32 i = 0; i < sizeof(g_authdata); ++i)
        g_authdata[i] = i < 2 ? (i == 0 ? 0x58 : 37) : static_cast<u8>(i);

    // Success: the credential is the JSON of the Windows path.
    forget(Mode::plain);
    check(start(2000), "the ceremony starts");
    check(!webauthn_ceremony_start_with(g_api, g_request, Waker{}, 78), "and only one at a time");
    check(wait_state(WebAuthnCeremonyState::done, 3000) == WebAuthnCeremonyState::done, "it ends when the key answers");
    u8 storage[2048];
    SecureBuffer credential{};
    credential.data = storage;
    credential.capacity = sizeof(storage);
    credential.locked = true;
    const WebAuthnCeremonyResult finished = webauthn_ceremony_finish(&credential);
    check(finished.ok() && credential.bytes > 100 &&
              contains(reinterpret_cast<const char*>(storage), credential.bytes, R"("id":"Y3JlZC1pZC0x")") &&
              contains(reinterpret_cast<const char*>(storage), credential.bytes, R"("authenticatorAttachment":"cross-platform")") &&
              contains(reinterpret_cast<const char*>(storage), credential.bytes, R"("type":"public-key")"),
          "its credential is the PublicKeyCredential JSON of the Windows path");
    check(webauthn_ceremony_state() == WebAuthnCeremonyState::idle, "and the ceremony is idle again");
    clear_bytes(storage, sizeof(storage));

    // A PIN: asked, refused once, accepted.
    forget(Mode::needs_pin);
    check(start(5000), "a ceremony for a key that wants a PIN starts");
    check(wait_state(WebAuthnCeremonyState::need_pin, 3000) == WebAuthnCeremonyState::need_pin &&
              !webauthn_ceremony_pin_prompt().wrong,
          "it asks for the PIN");
    const u8 short_pin[2] = {'1', '2'};
    check(!webauthn_ceremony_submit_pin({short_pin, 2}), "a PIN of two characters is not sent");
    const u8 wrong[4] = {'0', '0', '0', '0'};
    check(webauthn_ceremony_submit_pin({wrong, 4}), "a PIN is taken");
    // The prompt comes back after the key refuses it: wait for the wrong flag.
    const u64 end = clock_monotonic_milliseconds() + 3000;
    while (clock_monotonic_milliseconds() < end &&
           !(webauthn_ceremony_state() == WebAuthnCeremonyState::need_pin && webauthn_ceremony_pin_prompt().wrong))
        thread_sleep(2);
    check(webauthn_ceremony_state() == WebAuthnCeremonyState::need_pin && webauthn_ceremony_pin_prompt().wrong &&
              webauthn_ceremony_pin_prompt().retries == 2,
          "a wrong PIN brings the prompt back, saying so, with two attempts left");
    const u8 right[4] = {'1', '2', '3', '4'};
    check(webauthn_ceremony_submit_pin({right, 4}) &&
              wait_state(WebAuthnCeremonyState::done, 3000) == WebAuthnCeremonyState::done,
          "the right PIN finishes it");
    credential.bytes = 0;
    credential.data = storage;
    check(webauthn_ceremony_finish(&credential).ok() && credential.bytes > 100, "with the credential");
    check(same_text(g_fake.pins[g_fake.pin_count - 1], "1234") && g_fake.pin_count == 2, "the PINs reached the key as typed");
    clear_bytes(storage, sizeof(storage));

    // Cancel while the key waits for a touch: fido_dev_cancel goes to the key, the ceremony ends cancelled.
    forget(Mode::blocks);
    check(start(60000), "a ceremony for a key that waits starts");
    const u64 waiting = clock_monotonic_milliseconds() + 3000;
    while (clock_monotonic_milliseconds() < waiting && InterlockedCompareExchange(&g_fake.in_assert, 0, 0) == 0)
        thread_sleep(2);
    check(InterlockedCompareExchange(&g_fake.in_assert, 0, 0) != 0, "the key is waiting for a touch");
    webauthn_ceremony_cancel();
    check(wait_state(WebAuthnCeremonyState::done, 3000) == WebAuthnCeremonyState::done && g_fake.cancels >= 1,
          "a cancel reaches the waiting key and ends the ceremony");
    credential.bytes = 0;
    credential.data = storage;
    check(webauthn_ceremony_finish(&credential).error == WebAuthnCeremonyError::cancelled && credential.bytes == 0,
          "as cancelled, with no credential");

    // A cancel at the PIN prompt.
    forget(Mode::needs_pin);
    start(60000);
    wait_state(WebAuthnCeremonyState::need_pin, 3000);
    webauthn_ceremony_cancel();
    check(wait_state(WebAuthnCeremonyState::done, 3000) == WebAuthnCeremonyState::done &&
              webauthn_ceremony_finish(&credential).error == WebAuthnCeremonyError::cancelled,
          "a cancel at the PIN prompt ends it cancelled");

    // No key, a touch that never comes, a key without the credential.
    forget(Mode::plain);
    g_fake.devices = 0;
    start(150);
    wait_state(WebAuthnCeremonyState::done, 3000);
    check(webauthn_ceremony_finish(&credential).error == WebAuthnCeremonyError::no_device, "no key: no device");
    forget(Mode::never_touched);
    start(150);
    wait_state(WebAuthnCeremonyState::done, 3000);
    check(webauthn_ceremony_finish(&credential).error == WebAuthnCeremonyError::timed_out, "no touch: timed out");
    forget(Mode::no_credentials);
    start(600);
    wait_state(WebAuthnCeremonyState::done, 3000);
    check(webauthn_ceremony_finish(&credential).error == WebAuthnCeremonyError::not_registered, "no credential: not registered");

    // Release while the worker waits joins it.
    forget(Mode::blocks);
    start(60000);
    thread_sleep(30);
    webauthn_ceremony_release();
    check(webauthn_ceremony_state() == WebAuthnCeremonyState::idle, "a release joins the worker and leaves the ceremony idle");
    check(!webauthn_ceremony_submit_pin({right, 4}), "and no PIN is taken when none is asked");
}

// Registering a key ---------------------------------------------------------------------------------------------------------

constexpr char kCreationJson[] =
    R"({"publicKey":{"challenge":"AQIDBA","rp":{"id":"discord.com","name":"Discord"},)"
    R"("user":{"id":"dXNlci0x","name":"qa","displayName":"QA Tester"},)"
    R"("pubKeyCredParams":[{"type":"public-key","alg":-7},{"type":"public-key","alg":-257}],)"
    R"("excludeCredentials":[{"id":"Y3JlZC1pZC0x","type":"public-key","transports":["usb"]}],)"
    R"("authenticatorSelection":{"userVerification":"preferred"},"attestation":"none"}})";

WebAuthnMfaCreationOptions g_creation;

bool make_creation(const char* json, u32 bytes, u32 timeout_ms) noexcept {
    static u32 structurals[1024];
    clear_bytes(&g_creation, static_cast<u32>(sizeof(g_creation)));
    const WebAuthnMfaResult decoded =
        webauthn_mfa_decode_creation(Utf8View{json, bytes}, Slice<u32>{structurals, 1024}, 16, &g_creation);
    g_creation.request.timeout_ms = timeout_ms;
    return decoded.ok() && g_creation.request.credential_count == 1 && g_creation.algorithm_count >= 1;
}

template <u32 N> bool make_creation(const char (&json)[N], u32 timeout_ms) noexcept {
    return make_creation(json, N - 1, timeout_ms);
}

fido2::Outcome make_run(Surroundings* s, fido2::MakeResult* result) noexcept {
    u8 hash[32];
    for (u32 i = 0; i < 32; ++i)
        hash[i] = static_cast<u8>(0x40 + i);
    const fido2::Hooks hooks{s, never, note_device, answer_pin, short_pause};
    return fido2::make(g_api, g_creation, hash, hooks, result);
}

void test_creation() noexcept {
    build_make_data();
    static fido2::MakeResult made;
    Surroundings s{};

    // The creation reaches the library as it is, and the answer comes back whole.
    forget(Mode::plain);
    check(make_creation(kCreationJson, 600), "the creation challenge decodes");
    check(make_run(&s, &made) == fido2::Outcome::created, "a key that answers makes the credential");
    check(same_text(g_fake.rp, "discord.com") && same_text(g_fake.rp_name, "Discord") && g_fake.type == fido2::kCoseEs256 &&
              g_fake.allowed == 1 && g_fake.uv == fido2::kOptionOmit && g_fake.rk == fido2::kOptionOmit &&
              g_fake.hash[0] == 0x40 && g_fake.hash[31] == 0x5F && g_fake.user_id_bytes == 6 &&
              same_text(g_fake.user_name, "qa") && g_fake.makes == 1,
          "the relying party, user, first algorithm, excluded credential, client data hash and options reach the library");
    check(made.credential_id_bytes == sizeof(kId) && bytes_equal(made.credential_id, kId, sizeof(kId)) &&
              made.authenticator_data_bytes == 75 && made.authenticator_data[32] == 0x41 &&
              made.authenticator_data[37] == 0x77 && made.attestation_statement_bytes == sizeof(kStatement) &&
              made.attestation_statement[0] == 0xA1 && same_text(made.format, "packed"),
          "the id, the authenticator data without its CBOR header, the statement and the format come back");
    check(g_fake.opens == 1 && g_fake.closes == 1 && s.pins_asked == 0, "the key was opened once, closed, and no PIN asked");

    // User verification and resident keys as the options say.
    forget(Mode::plain);
    make_creation(kCreationJson, 600);
    g_creation.request.user_verification = WebAuthnUserVerification::required;
    g_creation.resident_key = WebAuthnResidentKey::required;
    make_run(&s, &made);
    check(g_fake.uv == fido2::kOptionTrue && g_fake.rk == fido2::kOptionTrue,
          "'required' verification and a required resident key are asked of the key");
    forget(Mode::plain);
    make_creation(kCreationJson, 600);
    g_creation.request.user_verification = WebAuthnUserVerification::discouraged;
    g_creation.resident_key = WebAuthnResidentKey::preferred;
    make_run(&s, &made);
    check(g_fake.uv == fido2::kOptionFalse && g_fake.rk == fido2::kOptionOmit,
          "'discouraged' verification asks for none and a preferred resident key is an ordinary credential");

    // The server's algorithms in order: the next one when the key refuses the first; none left is unsupported.
    forget(Mode::unsupported_es256);
    make_creation(kCreationJson, 600);
    check(make_run(&s, &made) == fido2::Outcome::created && g_fake.makes == 2 && g_fake.type == fido2::kCoseRs256,
          "a key that refuses ES256 is asked for RS256");
    forget(Mode::unsupported_all);
    make_creation(kCreationJson, 600);
    check(make_run(&s, &made) == fido2::Outcome::unsupported && g_fake.makes == 2,
          "a key that refuses every algorithm: unsupported");
    forget(Mode::plain);
    static constexpr char kOnlyUnknown[] =
        R"({"publicKey":{"challenge":"AQIDBA","rp":{"id":"discord.com"},"user":{"id":"AQID"},)"
        R"("pubKeyCredParams":[{"type":"public-key","alg":-35}],"excludeCredentials":[{"id":"Y3JlZC1pZC0x",)"
        R"("type":"public-key"}]}})";
    make_creation(kOnlyUnknown, 600);
    check(make_run(&s, &made) == fido2::Outcome::unsupported && g_fake.makes == 0,
          "algorithms libfido2 does not offer are never asked of a key");

    // A key that holds an excluded credential, a full key, a refusal on the key.
    forget(Mode::excluded);
    make_creation(kCreationJson, 600);
    check(make_run(&s, &made) == fido2::Outcome::excluded, "a key holding an excluded credential: excluded");
    forget(Mode::key_full);
    make_creation(kCreationJson, 600);
    check(make_run(&s, &made) == fido2::Outcome::key_full, "a key with no room left: key full");
    forget(Mode::denied);
    make_creation(kCreationJson, 600);
    check(make_run(&s, &made) == fido2::Outcome::cancelled, "a request turned down on the key: cancelled");

    // The PIN, as for a sign-in.
    forget(Mode::needs_pin);
    make_creation(kCreationJson, 600);
    g_creation.request.user_verification = WebAuthnUserVerification::required;
    clear_bytes(&s, static_cast<u32>(sizeof(s)));
    s.answers[0] = "0000";
    s.answers[1] = "1234";
    check(make_run(&s, &made) == fido2::Outcome::created && s.pins_asked == 2 && s.last_wrong && s.last_retries == 2 &&
              same_text(g_fake.pins[g_fake.pin_count - 1], "1234"),
          "a key that wants a PIN is asked for it, again after a wrong one, then makes the credential");
    forget(Mode::needs_pin);
    make_creation(kCreationJson, 600);
    clear_bytes(&s, static_cast<u32>(sizeof(s)));
    s.answers[0] = "0000";
    s.answers[1] = "1111";
    s.answers[2] = "2222";
    check(make_run(&s, &made) == fido2::Outcome::pin_blocked, "a blocked PIN ends the registration");
    forget(Mode::uv_without_pin);
    make_creation(kCreationJson, 600);
    check(make_run(&s, &made) == fido2::Outcome::pin_not_set, "a key with no PIN for a verification it must make");

    // No key, a touch that never comes, keys that cannot be opened.
    forget(Mode::plain);
    g_fake.devices = 0;
    make_creation(kCreationJson, 150);
    check(make_run(&s, &made) == fido2::Outcome::no_device && g_fake.manifests > 2, "no key plugged in: no device");
    forget(Mode::never_touched);
    make_creation(kCreationJson, 150);
    check(make_run(&s, &made) == fido2::Outcome::timed_out && g_fake.makes > 1, "a key that is never touched: timed out");
    forget(Mode::plain);
    g_fake.open_fails = true;
    make_creation(kCreationJson, 2000);
    check(make_run(&s, &made) == fido2::Outcome::cannot_open, "keys that cannot be opened: cannot open");
}

bool start_creation(const char* json, u32 bytes, u32 timeout_ms) noexcept {
    make_creation(json, bytes, timeout_ms);
    return webauthn_ceremony_start_creation_with(g_api, g_creation, Waker{}, 79);
}

void test_creation_ceremony() noexcept {
    build_make_data();
    u8 storage[3072];
    SecureBuffer credential{};
    credential.data = storage;
    credential.capacity = sizeof(storage);
    credential.locked = true;

    // A "none" attestation: the credential holds an attestation object without the statement and the AAGUID.
    forget(Mode::plain);
    check(start_creation(kCreationJson, sizeof(kCreationJson) - 1, 2000), "the registration ceremony starts");
    check(!webauthn_ceremony_start_with(g_api, g_request, Waker{}, 80), "and only one ceremony runs at a time");
    check(wait_state(WebAuthnCeremonyState::done, 3000) == WebAuthnCeremonyState::done, "it ends when the key answers");
    const WebAuthnCeremonyResult finished = webauthn_ceremony_finish(&credential);
    const char* json = reinterpret_cast<const char*>(storage);
    // The expected attestation object, from the key's data with its AAGUID cleared.
    u8 authenticator[75];
    copy_bytes(authenticator, g_make_authdata + 2, 75);
    webauthn_mfa_clear_aaguid(authenticator, 75);
    u8 object[300];
    const WebAuthnMfaResult encoded_object = webauthn_mfa_write_attestation_object(
        WebAuthnMfaAttestationObjectView{utf8_literal("none"), {}, {authenticator, 75}}, Slice<u8>{object, sizeof(object)});
    char object_text[512];
    const Base64UrlResult object_base64 =
        base64url_encode(ConstSlice<u8>{object, encoded_object.bytes}, Slice<char>{object_text, sizeof(object_text)});
    u8 client_data[256];
    const WebAuthnMfaResult client = webauthn_mfa_write_creation_client_data(g_creation, Slice<u8>{client_data, 256});
    char client_text[512];
    const Base64UrlResult client_base64 =
        base64url_encode(ConstSlice<u8>{client_data, client.bytes}, Slice<char>{client_text, sizeof(client_text)});
    object_text[object_base64.bytes] = 0;
    client_text[client_base64.bytes] = 0;
    check(finished.ok() && credential.bytes > 100 && contains(json, credential.bytes, R"("id":"Y3JlZC1pZC0x")") &&
              contains(json, credential.bytes, R"("publicKeyAlgorithm":-7,"transports":["usb"])") &&
              contains(json, credential.bytes, R"("authenticatorAttachment":"cross-platform")") &&
              encoded_object.ok() && object_base64.ok() && contains(json, credential.bytes, object_text) &&
              client.ok() && client_base64.ok() && contains(json, credential.bytes, client_text),
          "its credential is the registration JSON: the id, the algorithm of the key, its transport, the attestation "
          "object of format none and the client data");
    check(webauthn_ceremony_state() == WebAuthnCeremonyState::idle, "and the ceremony is idle again");
    clear_bytes(storage, sizeof(storage));

    // A direct attestation keeps the key's format, statement and AAGUID.
    static constexpr char kDirect[] =
        R"({"publicKey":{"challenge":"AQIDBA","rp":{"id":"discord.com","name":"Discord"},"user":{"id":"dXNlci0x"},)"
        R"("excludeCredentials":[{"id":"Y3JlZC1pZC0x","type":"public-key"}],"attestation":"direct"}})";
    forget(Mode::plain);
    check(start_creation(kDirect, sizeof(kDirect) - 1, 2000), "a direct registration starts");
    wait_state(WebAuthnCeremonyState::done, 3000);
    credential.bytes = 0;
    const WebAuthnCeremonyResult direct = webauthn_ceremony_finish(&credential);
    copy_bytes(authenticator, g_make_authdata + 2, 75);
    const WebAuthnMfaResult direct_object = webauthn_mfa_write_attestation_object(
        WebAuthnMfaAttestationObjectView{utf8_literal("packed"), {kStatement, sizeof(kStatement)}, {authenticator, 75}},
        Slice<u8>{object, sizeof(object)});
    const Base64UrlResult direct_base64 =
        base64url_encode(ConstSlice<u8>{object, direct_object.bytes}, Slice<char>{object_text, sizeof(object_text)});
    object_text[direct_base64.bytes] = 0;
    check(direct.ok() && direct_object.ok() && direct_base64.ok() && contains(json, credential.bytes, object_text),
          "a direct attestation keeps the key's format, statement and AAGUID");
    clear_bytes(storage, sizeof(storage));

    // A PIN, then the credential.
    forget(Mode::needs_pin);
    check(start_creation(kCreationJson, sizeof(kCreationJson) - 1, 5000), "a registration for a key that wants a PIN starts");
    check(wait_state(WebAuthnCeremonyState::need_pin, 3000) == WebAuthnCeremonyState::need_pin,
          "it asks for the PIN");
    const u8 right[4] = {'1', '2', '3', '4'};
    check(webauthn_ceremony_submit_pin({right, 4}) &&
              wait_state(WebAuthnCeremonyState::done, 3000) == WebAuthnCeremonyState::done,
          "the right PIN finishes it");
    credential.bytes = 0;
    check(webauthn_ceremony_finish(&credential).ok() && credential.bytes > 100, "with the credential");
    clear_bytes(storage, sizeof(storage));

    // The failures a registration names.
    forget(Mode::excluded);
    start_creation(kCreationJson, sizeof(kCreationJson) - 1, 600);
    wait_state(WebAuthnCeremonyState::done, 3000);
    credential.bytes = 0;
    check(webauthn_ceremony_finish(&credential).error == WebAuthnCeremonyError::excluded && credential.bytes == 0,
          "a key that already holds the credential: excluded, with no credential");
    forget(Mode::unsupported_all);
    start_creation(kCreationJson, sizeof(kCreationJson) - 1, 600);
    wait_state(WebAuthnCeremonyState::done, 3000);
    check(webauthn_ceremony_finish(&credential).error == WebAuthnCeremonyError::unsupported, "no algorithm accepted: unsupported");
    forget(Mode::key_full);
    start_creation(kCreationJson, sizeof(kCreationJson) - 1, 600);
    wait_state(WebAuthnCeremonyState::done, 3000);
    check(webauthn_ceremony_finish(&credential).error == WebAuthnCeremonyError::key_full, "a full key: key full");

    // Cancel while the key waits for a touch.
    forget(Mode::blocks);
    start_creation(kCreationJson, sizeof(kCreationJson) - 1, 60000);
    const u64 waiting = clock_monotonic_milliseconds() + 3000;
    while (clock_monotonic_milliseconds() < waiting && InterlockedCompareExchange(&g_fake.in_assert, 0, 0) == 0)
        thread_sleep(2);
    webauthn_ceremony_cancel();
    check(wait_state(WebAuthnCeremonyState::done, 3000) == WebAuthnCeremonyState::done &&
              webauthn_ceremony_finish(&credential).error == WebAuthnCeremonyError::cancelled,
          "a cancel reaches the waiting key and ends the registration cancelled");

    // A challenge without a user cannot start.
    check(!webauthn_ceremony_start_creation_with(g_api, WebAuthnMfaCreationOptions{}, Waker{}, 81) &&
              webauthn_ceremony_state() == WebAuthnCeremonyState::idle,
          "an empty registration does not start");
}

void test_no_library() noexcept {
    check(fido2::library() == nullptr && !webauthn_platform_available(), "a missing libfido2 is not loaded");
    make_request(600, WebAuthnUserVerification::preferred);
    check(!webauthn_ceremony_start(g_request, Waker{}, 1) && webauthn_ceremony_state() == WebAuthnCeremonyState::idle,
          "and no ceremony starts without it");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_linux_fido2_entry) {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    if (startup.argc > 1 && same_text(startup.argv[1], "--no-library")) {
        test_no_library();
        test_exit(g_failures, "linux_fido2_no_library");
    }
    test_assertion();
    test_pin();
    test_keys();
    test_ceremony();
    test_creation();
    test_creation_ceremony();
    test_exit(g_failures, "linux_fido2");
}
