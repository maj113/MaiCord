// Offline fixture: the signed-out login page's state (fingerprint and cookie
// jar), how signed-out requests reach it and how sign-in hands it to the
// account. Part of rest_client_fixtures.cpp.
#include "../../support/rest_client_probe.hpp"
#include "../../support/test_output.hpp"
#include "maicord/app/net/signed_out_session.hpp"

namespace maicord {
namespace signed_out_session_test {
u32 failures;
void check(bool pass, const char* label) noexcept {
    test_check(&failures, pass, label);
}

u32 run() noexcept {
    char fingerprint[kRestUserFingerprintMaximumBytes];
    constexpr auto minted = utf8_literal("1436551184213917696.Jz4qD1N8fRk0aSoW3ZpVr0v7hOM");
    check(!signed_out_fingerprint({fingerprint, sizeof(fingerprint)}) &&
              !signed_out_set_fingerprint({}) &&
              signed_out_set_fingerprint(minted) &&
              signed_out_fingerprint({fingerprint, sizeof(fingerprint)}) == minted.bytes &&
              bytes_equal(fingerprint, minted.data, minted.bytes) && !signed_out_fingerprint({fingerprint, 8}),
          "signed out: the page keeps the fingerprint the experiments answer minted");
    check(signed_out_claim_page_load() && !signed_out_claim_page_load(),
          "signed out: the login page's requests go out once, not on every QR refresh");

    // A signed-out request sends and stores into the page's jar, matched
    // against its own static target.
    static RestClientRuntime auth;
    auth.user_auth_mode = true;
    auth.command.auth_target = kUserAuthLoginTarget;
    const CookieRequest request = cookie_request(&auth);
    check(request_cookies(&auth) == signed_out_cookies() && request.same_site &&
              utf8_equal(request.target, kUserAuthLoginTarget) && utf8_equal(request.host, utf8_literal("discord.com")),
          "signed out: user-auth requests use the signed-out jar");
    constexpr char head[] = "HTTP/1.1 200 OK\r\n"
                            "Set-Cookie: __dcfduid=device; Max-Age=31536000; Secure; HttpOnly; Path=/; SameSite=Lax\r\n"
                            "Set-Cookie: __cfruid=edge; Path=/; Domain=discord.com; HttpOnly; Secure; SameSite=None\r\n\r\n";
    check(cookie_jar_store_response(signed_out_cookies(), request, {reinterpret_cast<const u8*>(head), sizeof(head) - 1},
                                    clock_unix_milliseconds()) == 2,
          "signed out: the login answer's cookies are kept");

    static CookieJar account;
    check(!signed_out_adopt_cookies(42, &account) && account.count == 0,
          "signed out: no account adopts the page's cookies before a sign-in");
    signed_out_signed_in(42);
    check(!signed_out_fingerprint({fingerprint, sizeof(fingerprint)}) && signed_out_claim_page_load(),
          "signed out: sign-in drops the fingerprint and the next login page loads afresh");
    check(!signed_out_adopt_cookies(7, &account) && account.count == 0 && signed_out_adopt_cookies(42, &account) &&
              account.count == 2 && !signed_out_adopt_cookies(42, &account),
          "signed out: only the account that signed in adopts the cookies, once, session ones included");
    u8 sent[kCookieHeaderMaximumBytes];
    const u32 bytes = cookie_jar_header(&account, {utf8_literal("discord.com"), utf8_literal("/api/v9/users/@me"), true},
                                        clock_unix_milliseconds(), {sent, sizeof(sent)});
    constexpr char expected[] = "__dcfduid=device; __cfruid=edge";
    check(bytes == sizeof(expected) - 1 && bytes_equal(sent, expected, bytes) && signed_out_cookies()->count == 2,
          "signed out: the account sends the device cookies the login used; the page keeps them too");
    secure_clear_bytes(sent, sizeof(sent));
    return failures;
}
} // namespace signed_out_session_test
} // namespace maicord
