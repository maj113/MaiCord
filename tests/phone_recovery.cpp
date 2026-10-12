#include "maicord/protocol/user_auth.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
void test_phone_recovery(void (*check)(bool, const char*) noexcept) noexcept {
    u8 request[4096]{}, saved[4096]{};
    constexpr auto phone = utf8_literal("+1\"\\\xc3\xa9\n");
    constexpr auto expected = utf8_literal("{\"phone\":\"+1\\\"\\\\\xc3\xa9\\n\"}");
    auto written = user_auth_write_recovery_request(phone, {request, sizeof(request)});
    auto base = user_auth_recovery_phone_base({request, sizeof(request)}, written.bytes);
    check(written.ok() && base.ok() && base.bytes == expected.bytes && bytes_equal(request, expected.data, expected.bytes),
        "phone: admitted recovery conversion preserves exact encoded original value");
    copy_bytes(saved, request, sizeof(request));
    const Utf8View invalid[]{utf8_literal(""), utf8_literal("12345"), utf8_literal("1234567"),
        utf8_literal("12a456"), utf8_literal("１２３４５６"), utf8_literal("12345\n")};
    for (auto code : invalid) {
        check(!user_auth_write_phone_verify_request({request, sizeof(request)}, base.bytes, code).ok() &&
            bytes_equal(saved, request, sizeof(request)), "phone: exact six ASCII digits required before mutation");
    }
    check(!user_auth_write_phone_verify_request({request, base.bytes + 15}, base.bytes, utf8_literal("001234")).ok() &&
        bytes_equal(saved, request, sizeof(request)), "phone: complete suffix capacity preflight leaves immutable base unchanged");
    check(!user_auth_write_phone_verify_request({request, sizeof(request)}, base.bytes,
        {reinterpret_cast<const char*>(request + 10), 6}).ok() && bytes_equal(saved, request, sizeof(request)),
        "phone: partially overlapping code is rejected before body writes");
    written = user_auth_write_phone_verify_request({request, sizeof(request)}, base.bytes, utf8_literal("001234"));
    constexpr auto verify = utf8_literal("{\"phone\":\"+1\\\"\\\\\xc3\xa9\\n\",\"code\":\"001234\"}");
    check(written.ok() && written.bytes == base.bytes + 16 && written.bytes == verify.bytes &&
        bytes_equal(request, verify.data, verify.bytes), "phone: verify request contains only exact phone and six digit code");
    char worst[512]; for (auto& value : worst) value = 1;
    written = user_auth_write_recovery_request({worst, sizeof(worst)}, {request, sizeof(request)});
    base = user_auth_recovery_phone_base({request, sizeof(request)}, written.bytes);
    written = user_auth_write_phone_verify_request({request, sizeof(request)}, base.bytes, utf8_literal("000000"));
    check(base.ok() && base.bytes == 3084 && written.ok() && written.bytes == 3100,
        "phone: worst case 512 byte original phone remains below reserved token staging");
    check(utf8_equal(user_auth_target(UserAuthOperation::phone_resend), utf8_literal("/api/v9/phone-verifications/resend")) &&
        utf8_equal(user_auth_target(UserAuthOperation::phone_verify), utf8_literal("/api/v9/phone-verifications/verify")),
        "phone: exact v9 resend and verify targets are distinct");
    written = user_auth_write_request_head(UserAuthOperation::phone_verify, 22, {}, {request, sizeof(request)});
    constexpr auto prefix = utf8_literal("POST /api/v9/phone-verifications/verify HTTP/1.1\r\n");
    bool no_auth = true;
    const auto head = Utf8View{reinterpret_cast<const char*>(request), written.bytes};
    for (u32 i = 0; i < head.bytes; ++i) {
        constexpr auto auth = utf8_literal("Authorization:");
        constexpr auto fingerprint = utf8_literal("X-Fingerprint:");
        if ((head.bytes - i >= auth.bytes && bytes_equal(head.data + i, auth.data, auth.bytes)) ||
            (head.bytes - i >= fingerprint.bytes && bytes_equal(head.data + i, fingerprint.data, fingerprint.bytes))) no_auth = false;
    }
    check(written.ok() && bytes_equal(request, prefix.data, prefix.bytes) && no_auth,
        "phone: native request head emits no fabricated authorization or fingerprint");

    u32 scratch[8192]{};
    char token[4096], challenges[2048], json[8220]{};
    for (auto& value : token) value = 'T';
    for (auto& value : challenges) value = 'C';
    UserAuthResponseStorage storage{}; storage.token = {token, sizeof(token)};
    storage.captcha_service = {challenges, 128}; storage.captcha_sitekey = {challenges + 128, 256};
    storage.captcha_rqdata = {challenges + 384, 512}; storage.captcha_rqtoken = {challenges + 896, 512};
    storage.captcha_session_id = {challenges + 1408, 256}; storage.captcha_user_flow = {challenges + 1664, 128};
    UserAuthResponse response{};
    const auto decode = [&](u16 status, Utf8View body, bool verifying) noexcept {
        return user_auth_decode_phone_response(status, body, {scratch, 8192}, 16, storage, &response, verifying);
    };
    check(decode(204, {}, false).ok() && response.kind == UserAuthResponseKind::phone_code_sent && !response.token_bytes &&
        decode(200, utf8_literal("{}"), false).ok() && response.kind == UserAuthResponseKind::phone_code_sent,
        "phone: empty or structured neutral resend success cannot authenticate");
    const Utf8View rejected[]{utf8_literal("{\"token\":\"reset\",\"user_id\":1}"),
        utf8_literal("{\"token\":\"reset\",\"user\":{\"id\":1}}"), utf8_literal("{\"token\":\"reset\",\"mfa\":false}"),
        utf8_literal("{\"token\":\"reset\",\"ticket\":\"ticket\"}"),
        utf8_literal("{\"token\":\"reset\",\"suspended_user_token\":\"suspended\"}"),
        utf8_literal("{\"token\":\"reset\",\"captcha_key\":[]}"), utf8_literal("{\"token\":\"reset\",\"captcha_sitekey\":\"site\"}"),
        utf8_literal("{\"token\":\"one\",\"to\\u006ben\":\"two\"}"), utf8_literal("{\"token\":\"reset\"} {}"),
        utf8_literal("{\"token\":\"\"}"), utf8_literal("{\"token\":null}"), utf8_literal("{\"token\":1}"), utf8_literal("{}")};
    for (auto body : rejected) {
        for (auto& value : token) value = 'T';
        for (auto& value : challenges) value = 'C';
        const auto result = decode(200, body, true);
        bool unchanged = true;
        for (auto value : token) unchanged &= value == 'T';
        for (auto value : challenges) unchanged &= value == 'C';
        check(!result.ok() && unchanged && response.kind == UserAuthResponseKind::none,
            "phone: conflicting auth fields duplicate escaped keys malformed or missing tokens are atomic rejection");
    }
    check(!decode(200, utf8_literal("{\"token\":\"reset\"}"), false).ok() &&
        !decode(200, utf8_literal("{"), false).ok() && !decode(200, utf8_literal("{\"mfa\":false}"), false).ok(),
        "phone: resend rejects token shaped and malformed nonempty success");
    copy_bytes(json, "{\"token\":\"", 10);
    for (u32 i = 0; i < 4097; ++i) json[10 + i] = 'x';
    copy_bytes(json + 10 + 4097, "\"}", 2);
    check(decode(200, {json, 4109}, true).error == UserAuthDecodeError::value_too_large,
        "phone: decoded reset token beyond 4096 bytes is rejected");
    copy_bytes(json + 10 + 4096, "\"}", 2);
    check(decode(200, {json, 4108}, true).ok() && response.kind == UserAuthResponseKind::phone_verified_reset &&
        response.token_bytes == 4096 && !response.has_user_id && !response.mfa_capabilities && token[4095] == 'x',
        "phone: full 4096 byte reset credential has dedicated non-authenticated classification");
    copy_bytes(json, "{\"token\":\"", 10);
    for (u32 i = 0; i < 4096; ++i) { json[10 + i * 2] = '\\'; json[11 + i * 2] = 'n'; }
    copy_bytes(json + 8202, "\"}", 2);
    check(decode(200, {json, 8204}, true).ok() && response.token_bytes == 4096 && token[4095] == '\n',
        "phone: 4096 token bound applies to unescaped UTF8 rather than encoded response length");
    check(!decode(200, utf8_literal("{\"token\":\"\xc0\xaf\"}"), true).ok(),
        "phone: malformed UTF8 reset token is rejected");
    check(decode(400, utf8_literal("{\"captcha_key\":[\"required\"],\"captcha_rqtoken\":\"opaque\"}"), true).ok() &&
        response.kind == UserAuthResponseKind::captcha_required && response.captcha_rqtoken_bytes == 6,
        "phone: challenge metadata uses shared bounded decoder without token adoption");
    UserAuthState state{}; user_auth_state_initialize(&state); state.phase = UserAuthPhase::phone_pending;
    response = {}; response.kind = UserAuthResponseKind::authenticated; response.token_bytes = 4;
    check(!user_auth_state_apply(&state, state.generation, response).ok() && state.phase == UserAuthPhase::phone_pending,
        "phone: state machine refuses account authentication in phone operation");
    response.kind = UserAuthResponseKind::phone_verified_reset;
    check(user_auth_state_apply(&state, state.generation, response).ok() && state.phase == UserAuthPhase::phone_token_ready,
        "phone: verified response terminates in reset token readiness");
}
} // namespace maicord
