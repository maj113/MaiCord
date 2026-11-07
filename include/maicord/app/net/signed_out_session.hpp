#pragma once

#include "maicord/protocol/http/cookie_jar.hpp"

namespace maicord {

// What the signed-out login page holds before any account signs in, as the
// desktop client's browser profile does: the cookies Discord sets on its
// answers and the experiments fingerprint. Every signed-out (user-auth) request
// sends both. Signing in drops the fingerprint (the client clears it at login
// success) and hands the cookies to that account, whose own jar then carries
// and persists them. One per process; every function is thread-safe. Neither
// value is ever logged.

// The signed-out cookie jar.
CookieJar* signed_out_cookies() noexcept;

// Copies the fingerprint into output; returns its bytes, 0 when none is held
// or output is too small.
u32 signed_out_fingerprint(Slice<char> output) noexcept;
// Keeps the fingerprint an experiments response minted; false when it is
// empty or longer than kRestUserFingerprintMaximumBytes.
bool signed_out_set_fingerprint(Utf8View fingerprint) noexcept;
// The login page's own requests (experiments, location metadata) go out once
// per signed-out period: true for the first caller after start or a sign-in,
// whatever their outcome, so a refreshing QR code never repeats them.
bool signed_out_claim_page_load() noexcept;

// The account profile_id signed in: the fingerprint is dropped and the
// cookies are kept for that account's start.
void signed_out_signed_in(u64 profile_id) noexcept;
// At the start of profile_id's account, after its saved session is restored:
// when it is the account that just signed in, replaces jar's contents with the
// signed-out cookies (session cookies included) and returns true. One shot.
bool signed_out_adopt_cookies(u64 profile_id, CookieJar* jar) noexcept;

} // namespace maicord
