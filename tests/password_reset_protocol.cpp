#include "test_output.hpp"
#include "maicord/protocol/login_continuations.hpp"
#include "maicord/app/login_continuations.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
bool same_reset(Utf8View a, Utf8View b) noexcept {
    return a.bytes == b.bytes && bytes_equal(a.data, b.data, a.bytes);
}
bool filled_reset(ConstSlice<u8> bytes, u8 value) noexcept {
    for (u32 i = 0; i < bytes.count; ++i) if (bytes.data[i] != value) return false;
    return true;
}
}
void test_password_reset_protocol(void (*check)(bool, const char*) noexcept) noexcept {
    u8 body[1024]{};
    const UserAuthResetPasswordRequest request{utf8_literal("link\"\\secret"), utf8_literal("new\npassword")};
    const auto measured = user_auth_measure_reset_password_request(request);
    const auto written = user_auth_write_reset_password_request(request, {body, sizeof(body)});
    check(measured.ok() && written.ok() && measured.bytes == written.bytes &&
        same_reset({reinterpret_cast<const char*>(body), written.bytes},
            utf8_literal("{\"token\":\"link\\\"\\\\secret\",\"password\":\"new\\npassword\",\"source\":\"/reset\"}")) &&
        body[written.token_prefix_bytes] == ',',
        "reset initial JSON escapes secrets and fixes the observed reset source");
    const u32 prefix = written.token_prefix_bytes;
    const auto tail_measure = user_auth_measure_reset_password_tail(utf8_literal("replacement"));
    const auto tail = user_auth_write_reset_password_tail(utf8_literal("replacement"),
        {body + prefix, tail_measure.bytes});
    check(tail.ok() && tail.bytes == tail_measure.bytes &&
        same_reset({reinterpret_cast<const char*>(body), prefix + tail.bytes},
            utf8_literal("{\"token\":\"link\\\"\\\\secret\",\"password\":\"replacement\",\"source\":\"/reset\"}")),
        "password correction replaces only the retained password tail");
    constexpr UserAuthMfaType methods[] = {UserAuthMfaType::totp, UserAuthMfaType::sms, UserAuthMfaType::backup};
    constexpr Utf8View expected[] = {
        utf8_literal(",\"code\":\"123456\",\"ticket\":\"mfa-ticket\",\"method\":\"totp\"}"),
        utf8_literal(",\"code\":\"123456\",\"ticket\":\"mfa-ticket\",\"method\":\"sms\"}"),
        utf8_literal(",\"code\":\"123456\",\"ticket\":\"mfa-ticket\",\"method\":\"backup\"}")};
    for (u32 i = 0; i < 3; ++i) {
        const auto size = user_auth_measure_reset_mfa_suffix(utf8_literal("123456"), utf8_literal("mfa-ticket"), methods[i]);
        const auto result = user_auth_write_reset_mfa_suffix(utf8_literal("123456"), utf8_literal("mfa-ticket"), methods[i],
            {body, size.bytes});
        check(size.ok() && result.ok() && same_reset({reinterpret_cast<const char*>(body), result.bytes}, expected[i]),
            "all text MFA methods append their own method at the reset endpoint");
    }
    constexpr Utf8View assertion = utf8_literal(
        "{\"id\":\"AQI\",\"rawId\":\"AQI\",\"response\":{\"authenticatorData\":\"AwQ\",\"clientDataJSON\":\"BQY\","
        "\"signature\":\"Bwg\",\"userHandle\":null},\"type\":\"public-key\",\"clientExtensionResults\":{},"
        "\"authenticatorAttachment\":\"platform\"}");
    const auto assertion_size = user_auth_measure_reset_mfa_suffix(assertion, utf8_literal("ticket"), UserAuthMfaType::webauthn);
    const auto assertion_body = user_auth_write_reset_mfa_suffix(assertion, utf8_literal("ticket"), UserAuthMfaType::webauthn,
        {body, sizeof(body)});
    constexpr Utf8View assertion_tail = utf8_literal(",\"method\":\"webauthn\"}");
    check(assertion_size.ok() && assertion_body.ok() && assertion_body.bytes == assertion_size.bytes &&
        same_reset({reinterpret_cast<const char*>(body) + assertion_body.bytes - assertion_tail.bytes, assertion_tail.bytes},
            assertion_tail),
        "WebAuthn reset accepts typed native assertion serialized as a JSON code string");
    constexpr Utf8View invalid_assertions[] = {
        utf8_literal("arbitrary-account-token"),
        utf8_literal("{}"),
        utf8_literal("{\"id\":\"AQI\",\"id\":\"AQI\"}"),
        utf8_literal("{\"id\":\"AQI\",\"rawId\":\"AQM\",\"response\":{\"authenticatorData\":\"AwQ\",\"clientDataJSON\":\"BQY\",\"signature\":\"Bwg\",\"userHandle\":null},\"type\":\"public-key\",\"clientExtensionResults\":{},\"authenticatorAttachment\":null}"),
        utf8_literal("{\"id\":\"AQI\",\"rawId\":\"AQI\",\"response\":{\"authenticatorData\":true,\"clientDataJSON\":\"BQY\",\"signature\":\"Bwg\",\"userHandle\":null},\"type\":\"public-key\",\"clientExtensionResults\":{},\"authenticatorAttachment\":null}")};
    for (const Utf8View invalid : invalid_assertions)
        check(!user_auth_measure_reset_mfa_suffix(invalid, utf8_literal("ticket"), UserAuthMfaType::webauthn).ok(),
            "reset WebAuthn rejects arbitrary tokens, missing fields, duplicates and invalid typed credentials");
    memset(body, 0x5A, sizeof(body));
    const auto short_body = user_auth_write_reset_password_request(request, {body, measured.bytes - 1});
    check(short_body.error == UserAuthWriteError::output_too_small && short_body.bytes == measured.bytes &&
        filled_reset({body, sizeof(body)}, 0x5A), "reset measured capacity failure leaves the entire destination untouched");
    check(user_auth_write_reset_password_request({{reinterpret_cast<const char*>(body), 3}, request.password},
        {body, sizeof(body)}).error == UserAuthWriteError::invalid_input && filled_reset({body, sizeof(body)}, 0x5A),
        "reset initial writer rejects source/output aliasing before writes");
    check(user_auth_write_reset_password_tail({reinterpret_cast<const char*>(body), 3}, {body, sizeof(body)}).error ==
        UserAuthWriteError::invalid_input &&
        user_auth_write_reset_mfa_suffix(utf8_literal("123456"), {reinterpret_cast<const char*>(body), 3},
            UserAuthMfaType::sms, {body, sizeof(body)}).error == UserAuthWriteError::invalid_input,
        "reset password and MFA tails reject aliased inputs");
    const auto exact = user_auth_write_reset_password_request(request, {body, measured.bytes});
    check(exact.ok() && exact.bytes == measured.bytes,
        "reset initial writer accepts its exact measured capacity");
    const auto suffix_size = user_auth_measure_reset_mfa_suffix(utf8_literal("123456"), utf8_literal("ticket"), UserAuthMfaType::totp);
    memset(body, 0x5A, sizeof(body));
    check(user_auth_write_reset_mfa_suffix(utf8_literal("123456"), utf8_literal("ticket"), UserAuthMfaType::totp,
        {body, suffix_size.bytes - 1}).error == UserAuthWriteError::output_too_small &&
        filled_reset({body, sizeof(body)}, 0x5A) &&
        user_auth_write_reset_password_tail(utf8_literal("replacement"), {body, tail_measure.bytes - 1}).error ==
            UserAuthWriteError::output_too_small && filled_reset({body, sizeof(body)}, 0x5A),
        "reset MFA suffix and replacement password tail fail one byte short without modifying storage");
    char maximum_token[kLoginResetMaximumTokenBytes]{}, maximum_password[kUserAuthMaximumPasswordBytes]{};
    memset(maximum_token, 1, sizeof(maximum_token)); memset(maximum_password, 1, sizeof(maximum_password));
    const UserAuthResetPasswordRequest maximum_request{{maximum_token, sizeof(maximum_token)},
        {maximum_password, sizeof(maximum_password)}};
    const auto maximum_measure = user_auth_measure_reset_password_request(maximum_request);
    SecureBuffer maximum_body{};
    const auto maximum_allocation = secure_buffer_initialize(&maximum_body, maximum_measure.bytes);
    check(maximum_measure.ok() && maximum_measure.bytes == 30764 && maximum_measure.token_prefix_bytes == 24587 &&
        maximum_allocation.ok(), "reset u64 measurement accounts for maximum six-byte JSON escaping");
    if (maximum_body.data) {
        check(user_auth_write_reset_password_request(maximum_request,
            {maximum_body.data, maximum_body.capacity}).ok(), "maximum expanded reset request fits its exact measured locked capacity");
        secure_buffer_release(&maximum_body);
    }
    secure_clear_bytes(maximum_token, sizeof(maximum_token)); secure_clear_bytes(maximum_password, sizeof(maximum_password));
    u8 head[1024]{};
    const auto initial_head = user_auth_write_request_head(UserAuthOperation::reset_password, 37, {}, {head, sizeof(head)});
    constexpr Utf8View head_prefix = utf8_literal("POST /api/v9/auth/reset HTTP/1.1\r\n");
    check(initial_head.ok() && same_reset({reinterpret_cast<const char*>(head), head_prefix.bytes}, head_prefix) &&
        same_reset(user_auth_target(UserAuthOperation::reset_mfa), kLoginResetTarget),
        "initial and final reset use the same allowlisted API v9 POST route");
    u32 scratch[256]{};
    char token[64]{}, ticket[64]{}, challenge[128]{};
    UserAuthResponseStorage storage{};
    storage.token = {token, sizeof(token)}; storage.ticket = {ticket, sizeof(ticket)};
    storage.webauthn_challenge = {challenge, sizeof(challenge)};
    UserAuthResponse decoded{};
    constexpr Utf8View mfa = utf8_literal("{\"mfa\":true,\"ticket\":\"reset-ticket\",\"totp\":true,\"sms\":true,\"backup\":true,\"webauthn\":{\"challenge\":\"AQI\"}}");
    check(user_auth_decode_reset_response(200, mfa, {scratch, 256}, 8, storage, &decoded).ok() &&
        decoded.kind == UserAuthResponseKind::mfa_required && decoded.mfa_capabilities == 15 && decoded.ticket_bytes == 12 &&
        decoded.token_bytes == 0, "initial reset adopts validated MFA ticket/capabilities without a session");
    memset(token, 0x5A, sizeof(token)); memset(ticket, 0x5A, sizeof(ticket));
    check(user_auth_decode_reset_response(200, mfa, {scratch, 256}, 8, storage, &decoded, true).error ==
        UserAuthDecodeError::unexpected_response && filled_reset({reinterpret_cast<const u8*>(token), sizeof(token)}, 0x5A) &&
        filled_reset({reinterpret_cast<const u8*>(ticket), sizeof(ticket)}, 0x5A),
        "final reset MFA rejects another MFA result before protected storage writes");
    constexpr Utf8View final_token = utf8_literal("{\"token\":\"session-token\",\"user_id\":\"123\"}");
    check(user_auth_decode_reset_response(200, final_token, {scratch, 256}, 8, storage, &decoded, true).ok() &&
        decoded.kind == UserAuthResponseKind::authenticated && decoded.token_bytes == 13 && decoded.user_id == 123,
        "final reset token enters the ordinary authenticated result contract");
    constexpr Utf8View malformed[] = {
        utf8_literal("{\"token\":\"\"}"), utf8_literal("{\"token\":\"x\",\"mfa\":true,\"ticket\":\"t\"}"),
        utf8_literal("{\"token\":\"a\",\"token\":\"b\"}"), utf8_literal("{\"mfa\":true,\"ticket\":\"\"}"),
        utf8_literal("{\"token\":\"session\"} trailing"), utf8_literal("{\"mfa\":true}"),
        utf8_literal("{\"token\":\"session\",\"unknown\":{\"bad\":[true,]}}")};
    for (const Utf8View invalid : malformed) {
        memset(token, 0x5A, sizeof(token));
        check(!user_auth_decode_reset_response(200, invalid, {scratch, 256}, 8, storage, &decoded).ok() &&
            filled_reset({reinterpret_cast<const u8*>(token), sizeof(token)}, 0x5A),
            "reset rejects empty, contradictory, duplicate and malformed complete responses atomically");
    }
    storage.token.count = 2;
    check(user_auth_decode_reset_response(200, final_token, {scratch, 256}, 8, storage, &decoded, true).error ==
        UserAuthDecodeError::output_too_small && filled_reset({reinterpret_cast<const u8*>(token), sizeof(token)}, 0x5A),
        "reset session capacity failure does not partially adopt a token");
    storage.token.count = sizeof(token);
    storage.ticket = storage.token;
    check(user_auth_decode_reset_response(200, final_token, {scratch, 256}, 8, storage, &decoded).error ==
        UserAuthDecodeError::invalid_input, "reset rejects overlapping secret destinations");
    storage.ticket = {ticket, sizeof(ticket)};
    check(!user_auth_decode_reset_response(400, final_token, {scratch, 256}, 8, storage, &decoded).ok() &&
        user_auth_decode_reset_response(400, utf8_literal("{\"code\":60008,\"message\":\"invalid code\"}"),
            {scratch, 256}, 8, storage, &decoded, true).ok() && decoded.kind == UserAuthResponseKind::rejected &&
        decoded.error_code == 60008, "reset errors are categorical and cannot adopt an auth-shaped error body");
    check(user_auth_decode_reset_response(400, utf8_literal("{\"captcha_key\":[\"required\"]}"),
        {scratch, 256}, 8, storage, &decoded, true).ok() && decoded.kind == UserAuthResponseKind::captcha_required,
        "reset final MFA preserves the shared CAPTCHA challenge classification");
    check(!user_auth_decode_recovery_response(200, final_token, {scratch, 256}, 8, storage, &decoded).ok(),
        "reset token success does not weaken ordinary recovery anti-promotion");
    UserAuthState state{}; user_auth_state_initialize(&state);
    const auto begun = user_auth_state_begin_reset(&state);
    UserAuthResponse response{}; response.kind = UserAuthResponseKind::captcha_required;
    check(begun.ok() && state.phase == UserAuthPhase::reset_pending &&
        user_auth_state_apply(&state, begun.generation, response).ok() &&
        state.captcha_return_phase == UserAuthPhase::reset_pending && user_auth_state_resume_captcha(&state).ok() &&
        state.phase == UserAuthPhase::reset_pending, "reset CAPTCHA resumes the reset operation");
    response.kind = UserAuthResponseKind::mfa_required;
    check(user_auth_state_apply(&state, state.generation, response).ok() &&
        user_auth_state_begin_mfa(&state, UserAuthMfaType::sms).ok(), "reset MFA uses the ordinary protected method state");
    const u64 obsolete = state.generation; user_auth_state_cancel(&state);
    response.kind = UserAuthResponseKind::authenticated; response.token_bytes = 13;
    check(user_auth_state_apply(&state, obsolete, response).error == UserAuthTransitionError::stale_generation,
        "cancelled reset cannot accept a late session");
    SecureBuffer link_token{};
    check(secure_buffer_initialize(&link_token, kLoginResetMaximumTokenBytes).ok(), "reset URI storage is locked");
    if (!link_token.data) return;
    const auto link = login_reset_consume_link(utf8_literal("https://discord.com/reset?token=query#%74oken=a%2Bb+c%F0%9F%98%80"), &link_token);
    check(link.ok() && same_reset({reinterpret_cast<const char*>(link_token.data), link_token.bytes},
        utf8_literal("a+b c\xF0\x9F\x98\x80")), "reset URI uses percent UTF-8 decoding and fragment token precedence");
    constexpr Utf8View rejected_links[] = {
        utf8_literal("http://discord.com/reset?token=x"), utf8_literal("https://discord.com:443/reset?token=x"),
        utf8_literal("https://discord.com@other.invalid/reset?token=x"), utf8_literal("https://discord.com/reset/extra?token=x"),
        utf8_literal("https://discord.com/verify?token=x"), utf8_literal("https://discord.com/reset?token=query#token="),
        utf8_literal("https://discord.com/reset?token=a&%74oken=b#token=fragment"),
        utf8_literal("https://discord.com/reset?token=query#token=x&token=y"),
        utf8_literal("https://discord.com/reset?token[]=a#token=fragment"),
        utf8_literal("https://discord.com/reset?token=query#token%5Bx%5D=a"),
        utf8_literal("https://discord.com/reset?other=%GG#token=fragment"),
        utf8_literal("https://discord.com/reset?token=%C0%AF#token=fragment"),
        utf8_literal("https://discord.com/reset?token=query#other=%ED%A0%80"),
        utf8_literal("https://discord.com/reset?token=%00"), utf8_literal("https://discord.com/reset?token=%F4%90%80%80")};
    for (const Utf8View invalid : rejected_links) {
        secure_buffer_assign(&link_token, {reinterpret_cast<const u8*>("stale"), 5});
        check(!login_reset_consume_link(invalid, &link_token).ok() && !link_token.bytes &&
            filled_reset({link_token.data, link_token.capacity}, 0),
            "reset URI rejects foreign, duplicate, array and malformed parameters in either component and erases output");
    }
    constexpr Utf8View aliased = utf8_literal("https://discord.com/reset?token=synthetic");
    secure_buffer_assign(&link_token, {reinterpret_cast<const u8*>(aliased.data), aliased.bytes});
    check(login_reset_consume_link({reinterpret_cast<const char*>(link_token.data), link_token.bytes}, &link_token).error ==
        LoginResetLinkError::invalid_input && filled_reset({link_token.data, link_token.capacity}, 0),
        "reset URI rejects aliasing and clears valid locked output");
    constexpr Utf8View url_prefix = utf8_literal("https://discord.com/reset?token=");
    char long_link[url_prefix.bytes + kLoginResetMaximumTokenBytes + 1]{};
    copy_bytes(long_link, url_prefix.data, url_prefix.bytes);
    memset(long_link + url_prefix.bytes, 'x', kLoginResetMaximumTokenBytes + 1);
    check(login_reset_consume_link({long_link, sizeof(long_link) - 1}, &link_token).ok() &&
        link_token.bytes == kLoginResetMaximumTokenBytes &&
        login_reset_consume_link({long_link, sizeof(long_link)}, &link_token).error == LoginResetLinkError::token_too_large &&
        filled_reset({link_token.data, link_token.capacity}, 0), "reset URI enforces the exact 4096-byte token boundary");
    secure_clear_bytes(long_link, sizeof(long_link));
    secure_buffer_release(&link_token);
    secure_clear_bytes(token, sizeof(token)); secure_clear_bytes(ticket, sizeof(ticket));
    secure_clear_bytes(challenge, sizeof(challenge)); secure_clear_bytes(body, sizeof(body));
}
} // namespace maicord


namespace maicord {
namespace password_reset_protocol_entry {
u32 failures = 0;
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_password_reset_protocol_entry() noexcept {
    maicord::test_password_reset_protocol(maicord::password_reset_protocol_entry::check);
    constexpr char pass[] = "PASS reset protocol\n";
    constexpr char fail[] = "FAIL reset protocol\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), maicord::password_reset_protocol_entry::failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(maicord::password_reset_protocol_entry::failures ? 1 : 0);
}
