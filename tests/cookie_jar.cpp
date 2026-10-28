#include "maicord/protocol/cookie_jar.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace cookie_jar_test {

u32 failures;
void check(bool success, const char* message) noexcept {
    test_check(&failures, success, message);
}

// Synthetic values only.
constexpr u64 kNow = 1'790'000'000'000ull; // 2026-09-21 14:13:20 UTC
constexpr u64 kDay = 24ull * 60 * 60 * 1000;
const CookieRequest kApi{utf8_literal("discord.com"), utf8_literal("/api/v9/users/@me?with=1"), true};
const CookieRequest kGateway{utf8_literal("gateway.discord.gg"), utf8_literal("/?encoding=json&v=9"), false};

CookieJar jar;
u8 header[kCookieHeaderMaximumBytes];

bool header_is(const CookieRequest& request, u64 now, const char* expected) noexcept {
    const u32 bytes = cookie_jar_header(&jar, request, now, {header, sizeof(header)});
    u32 length = 0;
    while (expected[length])
        ++length;
    return bytes == length && bytes_equal(header, expected, length);
}

bool set(const CookieRequest& request, const char* line, u64 now = kNow) noexcept {
    u32 length = 0;
    while (line[length])
        ++length;
    return cookie_jar_set(&jar, request, {line, length}, now);
}

void test_attributes() noexcept {
    cookie_jar_clear(&jar);
    check(set(kApi, "__dcfduid=visitor1; Expires=Sun, 25 Oct 2026 18:16:15 GMT; Secure; HttpOnly; Path=/; "
                    "Domain=.discord.com; SameSite=Lax") &&
              set(kApi, "_cfuvid=session1; HttpOnly; SameSite=None; Secure; Path=/; Domain=discord.com") &&
              header_is(kApi, kNow, "__dcfduid=visitor1; _cfuvid=session1"),
          "cookies: Discord's visitor and Cloudflare cookies are returned in creation order");
    const CookieRequest sub{utf8_literal("cdn.discord.com"), utf8_literal("/"), true};
    const CookieRequest other{utf8_literal("discord.gg"), utf8_literal("/"), true};
    check(header_is(sub, kNow, "__dcfduid=visitor1; _cfuvid=session1") &&
              cookie_jar_header(&jar, other, kNow, {header, sizeof(header)}) == 0,
          "cookies: a Domain cookie reaches subdomains and no other site");
    check(header_is(kApi, 1'792'952'174'999ull, "__dcfduid=visitor1; _cfuvid=session1") &&
              header_is(kApi, 1'792'952'175'000ull, "_cfuvid=session1"),
          "cookies: Expires is parsed to the millisecond and an expired cookie is dropped");
    check(set(kApi, "two=1; expires=25-Oct-26 18:16:15 GMT") && set(kApi, "bad=1; expires=31 Feb 2027 00:00:00") &&
              header_is(kApi, 1'792'952'174'999ull, "two=1; bad=1; _cfuvid=session1") &&
              header_is(kApi, 1'792'952'175'000ull, "bad=1; _cfuvid=session1"),
          "cookies: two-digit years parse; an impossible date leaves a session cookie");
    check(set(kApi, "two=1; Max-Age=60; Expires=Sun, 25 Oct 2026 18:16:15 GMT", 1'792'952'175'000ull) &&
              header_is(kApi, 1'792'952'175'000ull + 59'999, "bad=1; two=1; _cfuvid=session1") &&
              header_is(kApi, 1'792'952'175'000ull + 60'000, "bad=1; _cfuvid=session1"),
          "cookies: Max-Age wins over Expires");
    check(!set(kApi, "bad=gone; Max-Age=0") && header_is(kApi, kNow, "_cfuvid=session1") &&
              !set(kApi, "_cfuvid=gone; Domain=discord.com; Path=/; Expires=Thu, 01 Jan 1970 00:00:01 GMT") &&
              cookie_jar_header(&jar, kApi, kNow, {header, sizeof(header)}) == 0,
          "cookies: Max-Age=0 or a past Expires deletes the stored cookie");

    cookie_jar_clear(&jar);
    check(set(kApi, "__cfruid=route1") && set(kApi, "wide=1; Path=/api") &&
              header_is(kApi, kNow, "__cfruid=route1; wide=1") &&
              header_is({utf8_literal("discord.com"), utf8_literal("/api/v9/users"), true}, kNow,
                        "__cfruid=route1; wide=1") &&
              header_is({utf8_literal("discord.com"), utf8_literal("/api/v9/usersx"), true}, kNow, "wide=1") &&
              header_is({utf8_literal("discord.com"), utf8_literal("/apix"), true}, kNow, "") &&
              cookie_jar_header(&jar, {utf8_literal("cdn.discord.com"), utf8_literal("/api/v9/users/1"), true},
                                kNow, {header, sizeof(header)}) == 0,
          "cookies: the default path is the request directory, longer paths go first, host-only stays on its host");
    check(set(kApi, "__cfruid=route2; Path=/api/v9/users") && header_is(kApi, kNow, "__cfruid=route2; wide=1"),
          "cookies: a cookie with the same name, domain and path replaces the stored one in place");
}

void test_rejections() noexcept {
    cookie_jar_clear(&jar);
    check(!set(kApi, "a=1; Domain=discord.gg") && !set(kApi, "a=1; Domain=com") &&
              !set(kApi, "a=1; Domain=xdiscord.com") && !set(kApi, "a=1; SameSite=None") &&
              !set(kApi, "__Secure-a=1") && !set(kApi, "__Host-a=1; Secure; Domain=discord.com; Path=/") &&
              !set(kApi, "__Host-a=1; Secure; Path=/api") && !set(kApi, "=") && !set(kApi, "a=b\x01") &&
              cookie_jar_header(&jar, kApi, kNow, {header, sizeof(header)}) == 0,
          "cookies: foreign or public-suffix domains, insecure SameSite=None, bad prefixes and controls are refused");
    check(set(kApi, "__Host-a=1; Secure; Path=/") && set(kApi, "nameless") &&
              header_is(kApi, kNow, "nameless; __Host-a=1"),
          "cookies: a valid __Host- cookie and a nameless value are kept as Chromium sends them");
    static char line[kCookieMaximumNameValueBytes + 8];
    line[0] = 'n';
    line[1] = '=';
    for (u32 i = 2; i <= kCookieMaximumNameValueBytes; ++i)
        line[i] = 'v';
    line[kCookieMaximumNameValueBytes + 1] = 0;
    const bool limit = set(kApi, line);
    line[kCookieMaximumNameValueBytes + 1] = 'v';
    line[kCookieMaximumNameValueBytes + 2] = 0;
    check(limit && !set(kApi, line), "cookies: name and value may total 4096 bytes and no more");
    check(set(kApi, "long=1; Max-Age=999999999") && jar.entries[jar.count - 1].expires_ms == kNow + 400 * kDay,
          "cookies: an expiry is clamped to 400 days");
}

void test_cross_site() noexcept {
    cookie_jar_clear(&jar);
    const char head[] = "HTTP/1.1 101 Switching Protocols\r\nConnection: upgrade\r\nupgrade: websocket\r\n"
                        "Set-Cookie: __cf_bm=bot1; HttpOnly; SameSite=None; Secure; Path=/; Domain=discord.gg; "
                        "Expires=Sun, 25 Oct 2026 18:16:15 GMT\r\n"
                        "set-cookie: lax=1; Domain=discord.gg; Path=/\r\n\r\n";
    const u32 stored = cookie_jar_store_response(
        &jar, kGateway, {reinterpret_cast<const u8*>(head), sizeof(head) - 1}, kNow);
    const CookieRequest remote{utf8_literal("remote-auth-gateway.discord.gg"), utf8_literal("/?v=2"), false};
    check(stored == 1 && header_is(kGateway, kNow, "__cf_bm=bot1") && header_is(remote, kNow, "__cf_bm=bot1") &&
              cookie_jar_header(&jar, kApi, kNow, {header, sizeof(header)}) == 0,
          "cookies: a cross-site WebSocket response sets only SameSite=None cookies, sent to the whole site");
    check(set(kApi, "page=1") && header_is(kGateway, kNow, "__cf_bm=bot1"),
          "cookies: same-site cookies of the page stay off cross-site requests");
}

void test_capacity() noexcept {
    cookie_jar_clear(&jar);
    char line[] = "cA=1";
    bool stored = true;
    for (u32 i = 0; i < kCookieJarCapacity; ++i) {
        line[1] = static_cast<char>('A' + i);
        stored &= set(kApi, line, kNow + i);
    }
    // Touch every cookie but the second, then add one more: the second goes.
    jar.entries[1].accessed_ms = kNow - 1;
    line[1] = 'z';
    stored &= set(kApi, line, kNow + 100);
    u32 bytes = cookie_jar_header(&jar, kApi, kNow + 100, {header, sizeof(header)});
    bool second = false;
    for (u32 i = 0; i + 3 < bytes; ++i)
        second |= bytes_equal(header + i, "cB=", 3);
    check(stored && jar.count == kCookieJarCapacity && !second && bytes == kCookieJarCapacity * 6 - 2,
          "cookies: a full jar evicts its least recently used cookie");
    u8 small[16];
    check(cookie_jar_header(&jar, kApi, kNow, {small, sizeof(small)}) == 0,
          "cookies: the header needs room for a full jar");
}

void test_persistence() noexcept {
    cookie_jar_clear(&jar);
    set(kApi, "__sdcfduid=visitor2; Max-Age=31536000; Secure; HttpOnly; Path=/; Domain=discord.com");
    set(kApi, "_cfuvid=session2; Secure; SameSite=None; Path=/; Domain=discord.com");
    set(kGateway, "__cf_bm=bot2; Secure; SameSite=None; Path=/; Domain=discord.gg; Max-Age=1800");
    static u8 record[kCookieJarPersistentMaximumBytes];
    u32 revision = 0;
    const bool persistent = cookie_jar_has_persistent(&jar, &revision);
    const u32 bytes = cookie_jar_encode_persistent(&jar, kNow, {record, sizeof(record)});
    static CookieJar restored;
    const bool decoded = cookie_jar_decode_persistent(&restored, {record, bytes}, kNow + 1);
    u8 copy[kCookieHeaderMaximumBytes];
    const u32 api = cookie_jar_header(&restored, kApi, kNow + 1, {copy, sizeof(copy)});
    const u32 gateway = cookie_jar_header(&restored, kGateway, kNow + 1, {header, sizeof(header)});
    check(persistent && bytes && decoded && restored.count == 2 &&
              api == sizeof("__sdcfduid=visitor2") - 1 && bytes_equal(copy, "__sdcfduid=visitor2", api) &&
              gateway == sizeof("__cf_bm=bot2") - 1 && bytes_equal(header, "__cf_bm=bot2", gateway),
          "cookies: persistent cookies round-trip; session cookies are not saved");
    check(cookie_jar_decode_persistent(&restored, {record, bytes}, kNow + 1800'000) && restored.count == 1,
          "cookies: a record restores only the cookies still unexpired");
    record[4 + 18] ^= 1;
    check(!cookie_jar_decode_persistent(&restored, {record, bytes}, kNow) && restored.count == 0 &&
              !cookie_jar_decode_persistent(&restored, {record, 3}, kNow),
          "cookies: an inconsistent record restores nothing");
    cookie_jar_clear(&jar);
    u32 cleared = 0;
    check(!cookie_jar_has_persistent(&jar, &cleared) && cleared != revision && jar.text_bytes == 0 &&
              bytes_zero(jar.text, sizeof(jar.text)),
          "cookies: clearing erases every value and marks the persistent set changed");
}

} // namespace cookie_jar_test
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_cookie_jar_entry() noexcept {
    using namespace maicord::cookie_jar_test;
    test_attributes();
    test_rejections();
    test_cross_site();
    test_capacity();
    test_persistence();
    maicord::test_exit(failures, "cookie jar");
}
