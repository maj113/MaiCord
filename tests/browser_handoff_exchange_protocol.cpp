#include "maicord/protocol/user_auth.hpp"
#include "maicord/protocol/browser_sign_in.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
u32 handoff_scratch[16 * 1024];
u8 handoff_body[kBrowserSignInMaximumBodyBytes + 1];
char handoff_token[kUserAuthBrowserExchangeMaximumTokenBytes + 1];
bool handoff_contains(ConstSlice<u8> bytes, Utf8View needle) noexcept {
    if (needle.bytes > bytes.count) return false;
    for (u32 i = 0; i <= bytes.count - needle.bytes; ++i)
        if (bytes_equal(bytes.data + i, needle.data, needle.bytes)) return true;
    return false;
}
bool handoff_sentinel() noexcept {
    for (char value : handoff_token) if (value != 'x') return false;
    return true;
}
}
void test_browser_handoff_exchange_protocol(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr auto request = utf8_literal("{\"key\":\"00000000-0000-4000-8000-000000000000\",\"handoff_token\":\"synthetic-handoff\"}");
    check(user_auth_browser_handoff_request_valid(request, {handoff_scratch, 16 * 1024}, 16),
        "browser exchange: exact receiver JSON is admitted");
    const Utf8View bad_requests[]{
        utf8_literal("{}"),
        utf8_literal("{\"key\":\"00000000-0000-4000-8000-000000000000\",\"handoff_token\":\"\"}"),
        utf8_literal("{\"key\":\"00000000-0000-4000-8000-000000000000\",\"handoff_token\":\"a\",\"handoff_token\":\"b\"}"),
        utf8_literal("{\"key\":\"00000000-0000-4000-8000-000000000000\",\"handoff_token\":\"a\",\"handoff_\\u0074oken\":\"b\"}"),
        utf8_literal("{\"key\":\"00000000-0000-4000-8000-000000000000\",\"handoff_token\":\"a\",\"token\":\"foreign\"}"),
        utf8_literal("{\"key\":\"00000000-0000-3000-8000-000000000000\",\"handoff_token\":\"a\"}"),
        utf8_literal("{\"key\":\"00000000-0000-4000-7000-000000000000\",\"handoff_token\":\"a\"}"),
        utf8_literal("{\"key\":\"00000000-0000-4000-A000-000000000000\",\"handoff_token\":\"a\"}"),
        utf8_literal("{\"key\":\"foreign-key\",\"handoff_token\":\"a\"}"),
        utf8_literal("{\"key\":\"00000000-0000-4000-8000-000000000000\",\"handoff_token\":null}"),
    };
    for (const auto value : bad_requests)
        check(!user_auth_browser_handoff_request_valid(value, {handoff_scratch, 16 * 1024}, 16),
            "browser exchange: malformed, duplicate, foreign credential or noncanonical key rejected");
    constexpr auto prefix = utf8_literal("{\"key\":\"00000000-0000-4000-8000-000000000000\",\"handoff_token\":\"");
    copy_bytes(handoff_body, prefix.data, prefix.bytes);
    for (u32 i = 0; i < kBrowserSignInMaximumTokenBytes + 1; ++i) handoff_body[prefix.bytes + i] = 'a';
    handoff_body[prefix.bytes + kBrowserSignInMaximumTokenBytes] = '"';
    handoff_body[prefix.bytes + kBrowserSignInMaximumTokenBytes + 1] = '}';
    check(user_auth_browser_handoff_request_valid({reinterpret_cast<const char*>(handoff_body),
        prefix.bytes + kBrowserSignInMaximumTokenBytes + 2}, {handoff_scratch, 16 * 1024}, 16),
        "browser exchange: exact4096 decoded handoff credential is admitted");
    handoff_body[prefix.bytes + kBrowserSignInMaximumTokenBytes] = 'a';
    handoff_body[prefix.bytes + kBrowserSignInMaximumTokenBytes + 1] = '"';
    handoff_body[prefix.bytes + kBrowserSignInMaximumTokenBytes + 2] = '}';
    check(!user_auth_browser_handoff_request_valid({reinterpret_cast<const char*>(handoff_body),
        prefix.bytes + kBrowserSignInMaximumTokenBytes + 3}, {handoff_scratch, 16 * 1024}, 16) &&
        !user_auth_browser_handoff_request_valid({reinterpret_cast<const char*>(handoff_body),
            kBrowserSignInMaximumBodyBytes + 1}, {handoff_scratch, 16 * 1024}, 16) &&
        !user_auth_browser_handoff_request_valid(request,
            {reinterpret_cast<u32*>(const_cast<char*>(request.data)), 1}, 16),
        "browser exchange: over-limit decoded/wire credentials and scratch alias rejected");

    u8 head[1024]{};
    auto written = user_auth_write_request_head(UserAuthOperation::browser_handoff_exchange, request.bytes,
        {}, {head, sizeof(head)});
    check(written.ok() && handoff_contains({head, written.bytes}, utf8_literal("POST /api/v9/auth/handoff/exchange HTTP/1.1\r\n")) &&
        !handoff_contains({head, written.bytes}, utf8_literal("Authorization:")) &&
        !handoff_contains({head, written.bytes}, utf8_literal("X-Captcha")),
        "browser exchange: dedicated bounded POST omits authorization and CAPTCHA");
    for (u8& value : head) value = 0x5a;
    written = user_auth_write_request_head(UserAuthOperation::browser_handoff_exchange, request.bytes,
        {utf8_literal("solution"), {}, {}}, {head, sizeof(head)});
    bool untouched = true; for (u8 value : head) if (value != 0x5a) untouched = false;
    check(!written.ok() && untouched, "browser exchange: CAPTCHA head rejection leaves destination untouched");

    UserAuthResponseStorage storage{}; storage.token = {handoff_token, kUserAuthBrowserExchangeMaximumTokenBytes};
    UserAuthResponse result{};
    constexpr auto success = utf8_literal("{\"user\":{\"id\":\"123456789012345678\",\"username\":\"synthetic\"},\"token\":\"synthetic-session\"}");
    auto decoded = user_auth_decode_browser_handoff_response(200, success,
        {handoff_scratch, 16 * 1024}, 16, storage, &result);
    check(decoded.ok() && result.kind == UserAuthResponseKind::authenticated && result.has_user_id &&
        result.user_id == 123456789012345678ull && result.token_bytes == 17 &&
        bytes_equal(handoff_token, "synthetic-session", 17),
        "browser exchange: only nested user.id and session token authenticate");
    const Utf8View malformed[]{
        utf8_literal("{\"token\":\"s\"}"), utf8_literal("{\"user_id\":\"1\",\"token\":\"s\"}"),
        utf8_literal("{\"user\":{},\"token\":\"s\"}"), utf8_literal("{\"user\":{\"id\":0},\"token\":\"s\"}"),
        utf8_literal("{\"user\":{\"id\":\"0\"},\"token\":\"s\"}"),
        utf8_literal("{\"user\":{\"id\":\"18446744073709551616\"},\"token\":\"s\"}"),
        utf8_literal("{\"user\":{\"id\":\"1\",\"i\\u0064\":\"2\"},\"token\":\"s\"}"),
        utf8_literal("{\"user\":{\"id\":\"1\",\"username\":\"a\",\"username\":\"b\"},\"token\":\"s\"}"),
        utf8_literal("{\"user\":{\"id\":\"1\"},\"token\":\"s\",\"to\\u006ben\":\"other\"}"),
        utf8_literal("{\"user\":{\"id\":\"1\"},\"token\":\"s\",\"extension\":1,\"extension\":2}"),
        utf8_literal("{\"user\":{\"id\":\"1\"},\"token\":\"s\",\"mfa\":false}"),
        utf8_literal("{\"user\":{\"id\":\"1\"},\"token\":\"s\",\"ticket\":\"challenge\"}"),
        utf8_literal("{\"user\":{\"id\":\"1\"},\"token\":\"s\",\"captcha_sitekey\":null}"),
        utf8_literal("{\"user\":{\"id\":\"1\"},\"token\":\"s\",\"code\":0}"),
        utf8_literal("{\"user\":{\"id\":\"1\"},\"token\":\"\"}"),
        utf8_literal("{\"user\":{\"id\":\"1\"},\"token\":null}"), utf8_literal("{"),
    };
    for (const auto value : malformed) {
        for (char& byte : handoff_token) byte = 'x';
        decoded = user_auth_decode_browser_handoff_response(200, value,
            {handoff_scratch, 16 * 1024}, 16, storage, &result);
        check(!decoded.ok() && result.kind == UserAuthResponseKind::none && handoff_sentinel(),
            "browser exchange: strict failures leave all secret storage untouched");
    }
    for (char& byte : handoff_token) byte = 'x';
    UserAuthResponseStorage tiny{}; tiny.token = {handoff_token, 1};
    decoded = user_auth_decode_browser_handoff_response(200, success,
        {handoff_scratch, 16 * 1024}, 16, tiny, &result);
    check(decoded.error == UserAuthDecodeError::output_too_small && handoff_sentinel(),
        "browser exchange: token capacity preflight is atomic");
    constexpr auto token_prefix = utf8_literal("{\"user\":{\"id\":\"1\"},\"token\":\"");
    copy_bytes(handoff_body, token_prefix.data, token_prefix.bytes);
    for (u32 i = 0; i < kUserAuthBrowserExchangeMaximumTokenBytes; ++i) handoff_body[token_prefix.bytes + i] = 's';
    handoff_body[token_prefix.bytes + kUserAuthBrowserExchangeMaximumTokenBytes] = '"';
    handoff_body[token_prefix.bytes + kUserAuthBrowserExchangeMaximumTokenBytes + 1] = '}';
    decoded = user_auth_decode_browser_handoff_response(200,
        {reinterpret_cast<const char*>(handoff_body), token_prefix.bytes + kUserAuthBrowserExchangeMaximumTokenBytes + 2},
        {handoff_scratch, 16 * 1024}, 16, storage, &result);
    check(decoded.ok() && result.token_bytes == kUserAuthBrowserExchangeMaximumTokenBytes,
        "browser exchange: exact2048 session token bound is admitted");
    handoff_body[token_prefix.bytes + kUserAuthBrowserExchangeMaximumTokenBytes] = 's';
    handoff_body[token_prefix.bytes + kUserAuthBrowserExchangeMaximumTokenBytes + 1] = '"';
    handoff_body[token_prefix.bytes + kUserAuthBrowserExchangeMaximumTokenBytes + 2] = '}';
    for (char& byte : handoff_token) byte = 'x';
    decoded = user_auth_decode_browser_handoff_response(200,
        {reinterpret_cast<const char*>(handoff_body), token_prefix.bytes + kUserAuthBrowserExchangeMaximumTokenBytes + 3},
        {handoff_scratch, 16 * 1024}, 16, storage, &result);
    check(decoded.error == UserAuthDecodeError::value_too_large && handoff_sentinel(),
        "browser exchange: over-limit session token fails before copying");
    decoded = user_auth_decode_browser_handoff_response(400, utf8_literal("{\"code\":50035,\"message\":\"invalid\"}"),
        {handoff_scratch, 16 * 1024}, 16, storage, &result);
    check(decoded.ok() && result.kind == UserAuthResponseKind::rejected && result.has_error_code &&
        result.error_code == 50035 && handoff_sentinel(), "browser exchange: categorical rejection does not copy credentials");
    check(!user_auth_decode_browser_handoff_response(400, success,
        {handoff_scratch, 16 * 1024}, 16, storage, &result).ok() &&
        !user_auth_decode_browser_handoff_response(302, success,
            {handoff_scratch, 16 * 1024}, 16, storage, &result).ok() &&
        !user_auth_decode_browser_handoff_response(400, utf8_literal("{\"captcha_key\":[\"challenge\"]}"),
            {handoff_scratch, 16 * 1024}, 16, storage, &result).ok(),
        "browser exchange: error-token, redirect and CAPTCHA responses cannot authenticate or replay");

    UserAuthState state{}; user_auth_state_initialize(&state);
    const auto begun = user_auth_state_begin_browser_exchange(&state);
    UserAuthResponse challenge{}; challenge.kind = UserAuthResponseKind::captcha_required;
    check(begun.ok() && state.phase == UserAuthPhase::browser_exchange_pending &&
        !user_auth_state_apply(&state, begun.generation, challenge).ok() &&
        !user_auth_state_begin_password(&state).ok(), "browser exchange: pending owner cannot enter CAPTCHA or password flow");
    UserAuthResponse authenticated{}; authenticated.kind = UserAuthResponseKind::authenticated;
    check(!user_auth_state_apply(&state, begun.generation, authenticated).ok(),
        "browser exchange: state rejects missing identity/session receipt");
    authenticated.has_user_id = true; authenticated.user_id = 1; authenticated.token_bytes = 1;
    user_auth_state_cancel(&state);
    check(!user_auth_state_apply(&state, begun.generation, authenticated).ok() && state.phase == UserAuthPhase::idle,
        "browser exchange: cancellation invalidates old generation");
}
}
