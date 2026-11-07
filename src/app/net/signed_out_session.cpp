#include "maicord/app/net/signed_out_session.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/protocol/http/rest.hpp"

namespace maicord {
namespace {

struct SignedOutSession {
    CookieJar cookies;
    SrwLock lock;
    // The account that signed in and has not yet adopted the cookies.
    u64 signed_in_profile;
    u32 fingerprint_bytes;
    // The login page's own requests went out (or were attempted) since the last sign-in.
    bool page_loaded;
    char fingerprint[kRestUserFingerprintMaximumBytes];
};

SignedOutSession g_signed_out;

} // namespace

CookieJar* signed_out_cookies() noexcept {
    return &g_signed_out.cookies;
}

u32 signed_out_fingerprint(Slice<char> output) noexcept {
    AcquireSRWLockShared(g_signed_out.lock.native());
    const u32 bytes = output.data && output.count >= g_signed_out.fingerprint_bytes ? g_signed_out.fingerprint_bytes : 0;
    copy_bytes(output.data, g_signed_out.fingerprint, bytes);
    ReleaseSRWLockShared(g_signed_out.lock.native());
    return bytes;
}

bool signed_out_set_fingerprint(Utf8View fingerprint) noexcept {
    if (!fingerprint.data || !fingerprint.bytes || fingerprint.bytes > kRestUserFingerprintMaximumBytes)
        return false;
    AcquireSRWLockExclusive(g_signed_out.lock.native());
    copy_bytes(g_signed_out.fingerprint, fingerprint.data, fingerprint.bytes);
    g_signed_out.fingerprint_bytes = fingerprint.bytes;
    ReleaseSRWLockExclusive(g_signed_out.lock.native());
    return true;
}

bool signed_out_claim_page_load() noexcept {
    AcquireSRWLockExclusive(g_signed_out.lock.native());
    const bool first = !g_signed_out.page_loaded;
    g_signed_out.page_loaded = true;
    ReleaseSRWLockExclusive(g_signed_out.lock.native());
    return first;
}

void signed_out_signed_in(u64 profile_id) noexcept {
    AcquireSRWLockExclusive(g_signed_out.lock.native());
    secure_clear_bytes(g_signed_out.fingerprint, sizeof(g_signed_out.fingerprint));
    g_signed_out.fingerprint_bytes = 0;
    g_signed_out.page_loaded = false;
    g_signed_out.signed_in_profile = profile_id;
    ReleaseSRWLockExclusive(g_signed_out.lock.native());
}

bool signed_out_adopt_cookies(u64 profile_id, CookieJar* jar) noexcept {
    AcquireSRWLockExclusive(g_signed_out.lock.native());
    const bool adopt = jar && profile_id && g_signed_out.signed_in_profile == profile_id;
    if (adopt)
        g_signed_out.signed_in_profile = 0;
    ReleaseSRWLockExclusive(g_signed_out.lock.native());
    // The browser profile carries on: the signed-out page keeps its cookies too.
    if (adopt)
        cookie_jar_copy(jar, &g_signed_out.cookies);
    return adopt;
}

} // namespace maicord
