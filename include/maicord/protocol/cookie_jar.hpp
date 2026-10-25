#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/core/win32_records.hpp"

namespace maicord {

// The cookies Discord's hosts set on the client (Cloudflare and Discord
// visitor IDs, a challenge clearance), kept as Chromium's jar keeps them and
// returned on matching requests. Bounded: the least recently used cookie is
// evicted when a new one does not fit. Every function locks the jar; a zero
// jar is empty. Values are secrets for logging purposes: never log them.
inline constexpr u32 kCookieJarCapacity = 16;
inline constexpr u32 kCookieJarTextBytes = 8 * 1024;
// Chromium's limits: name and value together, and each attribute value.
inline constexpr u32 kCookieMaximumNameValueBytes = 4096;
inline constexpr u32 kCookieMaximumAttributeBytes = 1024;
// Domain and path are stored with one-byte lengths.
inline constexpr u32 kCookieMaximumDomainBytes = 253;
inline constexpr u32 kCookieMaximumPathBytes = 255;
// Chromium clamps an expiry to 400 days from the time it is set.
inline constexpr u64 kCookieMaximumLifetimeMs = 400ull * 24 * 60 * 60 * 1000;
// A Cookie header value holding every stored cookie fits this.
inline constexpr u32 kCookieHeaderMaximumBytes = kCookieJarTextBytes + 3 * kCookieJarCapacity;
// cookie_jar_encode_persistent output for a full jar fits this.
inline constexpr u32 kCookieJarPersistentMaximumBytes = 4 + kCookieJarCapacity * 24 + kCookieJarTextBytes;

enum CookieFlag : u8 {
    cookie_secure = 1u << 0,
    cookie_http_only = 1u << 1,
    cookie_host_only = 1u << 2,
};

enum class CookieSameSite : u8 {
    unspecified,
    none,
    lax,
    strict,
};

// text[offset...] holds the name, value, domain (lowercase, no leading dot)
// and path. expires_ms is 0 for a session cookie; times are Unix milliseconds.
struct CookieEntry {
    u64 expires_ms;
    u64 created_ms;
    u64 accessed_ms;
    u16 offset;
    u16 name_bytes;
    u16 value_bytes;
    u8 domain_bytes;
    u8 path_bytes;
    u8 flags;
    CookieSameSite same_site;
};

struct CookieJar {
    SrwLock lock;
    u32 count;
    u32 text_bytes;
    // Bumped whenever a persistent cookie is added, changed or removed.
    u32 persistent_revision;
    CookieEntry entries[kCookieJarCapacity];
    char text[kCookieJarTextBytes];
};

// The request a cookie is set by or sent on: host and the request target
// (its path, before any query). same_site is false for a request the
// https://discord.com page makes to another site (the Gateway on discord.gg):
// as in Chromium, only SameSite=None cookies cross that boundary. Every
// request is secure (HTTPS or WSS).
struct CookieRequest {
    Utf8View host;
    Utf8View target;
    bool same_site;
};

// Applies one Set-Cookie value (RFC 6265bis as Chromium parses it). False
// when the cookie is rejected or deletes a stored one.
bool cookie_jar_set(CookieJar* jar, const CookieRequest& request, Utf8View set_cookie, u64 now_ms) noexcept;
// Applies every Set-Cookie field of a complete response head; returns how
// many cookies were stored.
u32 cookie_jar_store_response(CookieJar* jar, const CookieRequest& request, ConstSlice<u8> head,
                              u64 now_ms) noexcept;
// Writes the Cookie header value for the request ("a=1; b=2", longer paths
// first, then older cookies first) and marks the cookies used. Returns the
// bytes, 0 when no cookie matches or output is smaller than
// kCookieHeaderMaximumBytes. Expired cookies are dropped.
u32 cookie_jar_header(CookieJar* jar, const CookieRequest& request, u64 now_ms, Slice<u8> output) noexcept;
// Erases every cookie.
void cookie_jar_clear(CookieJar* jar) noexcept;

// The unexpired persistent cookies as a self-contained record (session
// cookies stay in memory only); 0 when output is smaller than
// kCookieJarPersistentMaximumBytes. decode replaces the jar's contents and is
// strict: a malformed record leaves the jar empty and returns false.
u32 cookie_jar_encode_persistent(CookieJar* jar, u64 now_ms, Slice<u8> output) noexcept;
bool cookie_jar_decode_persistent(CookieJar* jar, ConstSlice<u8> input, u64 now_ms) noexcept;
// Whether any persistent cookie is stored, and the revision it was last changed at.
bool cookie_jar_has_persistent(CookieJar* jar, u32* revision) noexcept;

} // namespace maicord
