#include "maicord/app/login_continuations.hpp"
#include "maicord/protocol/login_continuations.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/crt.hpp"

namespace maicord {
namespace {
bool cleared(const SecureBuffer& value) noexcept {
    if (value.bytes) return false;
    for (u32 i = 0; i < value.capacity; ++i) if (value.data[i]) return false;
    return true;
}
} // namespace

void test_login_continuations(void (*check)(bool, const char*) noexcept) noexcept {
    SecureBuffer token{};
    check(secure_buffer_initialize(&token, kLoginVerificationMaximumTokenBytes).ok(),
          "verification link storage is locked");
    if (!token.data) return;
    check(login_verification_link_candidate(utf8_literal("https://discord.com/verify?token=fake")) &&
              !login_verification_link_candidate(utf8_literal("user@example.invalid")) &&
              !login_verification_link_candidate(utf8_literal("https://discord.com/verify-other?token=fake")),
          "native verification routing recognizes exact supported origin/path only");
    const auto inspect = [&](Utf8View link, Utf8View expected) noexcept {
        const auto result = login_verification_consume_link(link, &token);
        check(result.ok() && result.token_bytes == expected.bytes &&
                  utf8_equal({reinterpret_cast<const char*>(token.data), token.bytes}, expected),
              "verification fragment/query link decodes directly into locked output");
    };
    inspect(utf8_literal("https://discord.com/verify?token=synthetic-query"), utf8_literal("synthetic-query"));
    inspect(utf8_literal("https://discord.com/verify?token=query#token=fragment"), utf8_literal("fragment"));
    inspect(utf8_literal("https://discord.com/verify?token=one&token=two#token=fragment"), utf8_literal("fragment"));
    inspect(utf8_literal("https://discord.com/verify?token=query#other=value"), utf8_literal("query"));
    inspect(utf8_literal("https://discord.com/verify#%74oken=a%2Bb+c%26d%3De"), utf8_literal("a+b c&d=e"));
    inspect(utf8_literal("https://discord.com/verify?token=%F0%9F%98%80"), utf8_literal("\xF0\x9F\x98\x80"));
    constexpr Utf8View rejected[] = {
        utf8_literal("http://discord.com/verify?token=fake"),
        utf8_literal("https://discord.com.evil.invalid/verify?token=fake"),
        utf8_literal("https://discord.com@evil.invalid/verify?token=fake"),
        utf8_literal("https://discord.com:443/verify?token=fake"),
        utf8_literal("https://discord.com/verify/extra?token=fake"),
        utf8_literal("https://discord.com/reset?token=fake"),
        utf8_literal("https://discord.com/verify"),
        utf8_literal("https://discord.com/verify?token=query#token="),
        utf8_literal("https://discord.com/verify?token=query#token"),
        utf8_literal("https://discord.com/verify?token=a&%74oken=b"),
        utf8_literal("https://discord.com/verify?token=query#token=a&token=b"),
        utf8_literal("https://discord.com/verify?token=%"),
        utf8_literal("https://discord.com/verify?token=%G0"),
        utf8_literal("https://discord.com/verify?token=%00"),
        utf8_literal("https://discord.com/verify?token=%0A"),
        utf8_literal("https://discord.com/verify?token=%C0%AF"),
        utf8_literal("https://discord.com/verify?token=%ED%A0%80"),
        utf8_literal("https://discord.com/verify?token=%F0%9F"),
    };
    for (Utf8View link : rejected) {
        secure_buffer_assign(&token, {reinterpret_cast<const u8*>("synthetic-stale"), 15});
        check(!login_verification_consume_link(link, &token).ok() && cleared(token),
              "verification rejects ambiguous/foreign/malformed links and erases stale output");
    }
    constexpr Utf8View overlapping_link = utf8_literal("https://discord.com/verify?token=synthetic");
    secure_buffer_assign(&token, {reinterpret_cast<const u8*>(overlapping_link.data), overlapping_link.bytes});
    check(login_verification_consume_link(
              {reinterpret_cast<const char*>(token.data), token.bytes}, &token).error ==
              LoginVerificationLinkError::invalid_input && cleared(token),
          "verification rejects source/output aliasing and clears its invalid output");
    SecureBuffer small{token.data, 2, 2, true};
    check(login_verification_consume_link(utf8_literal("https://discord.com/verify?token=123"), &small).error ==
              LoginVerificationLinkError::output_too_small && cleared(small),
          "verification capacity failure clears bounded output");
    constexpr Utf8View long_prefix = utf8_literal("https://discord.com/verify?token=");
    char long_link[long_prefix.bytes + kLoginVerificationMaximumTokenBytes + 1]{};
    copy_bytes(long_link, long_prefix.data, long_prefix.bytes);
    memset(long_link + long_prefix.bytes, 'x', kLoginVerificationMaximumTokenBytes + 1);
    check(login_verification_consume_link(
              {long_link, long_prefix.bytes + kLoginVerificationMaximumTokenBytes}, &token).ok() &&
              token.bytes == kLoginVerificationMaximumTokenBytes,
          "verification accepts its exact bounded token limit");
    check(login_verification_consume_link({long_link, sizeof(long_link)}, &token).error ==
              LoginVerificationLinkError::token_too_large && cleared(token),
          "verification rejects and clears over-limit tokens");
    secure_clear_bytes(long_link, sizeof(long_link));
    u8 request[128]{};
    const auto written = login_verification_write_request(utf8_literal("a\"b\\c"), {request, sizeof(request)});
    check(written.ok() && utf8_equal({reinterpret_cast<const char*>(request), written.bytes},
                                     utf8_literal("{\"token\":\"a\\\"b\\\\c\"}")),
          "verification request uses exact observed token-only JSON contract");
    memset(request, 0x5A, sizeof(request));
    const auto undersized = login_verification_write_request(utf8_literal("synthetic"), {request, 2});
    bool preserved = true;
    for (u8 byte : request) if (byte != 0x5A) preserved = false;
    check(undersized.error == UserAuthWriteError::output_too_small && preserved,
          "verification request capacity validation is atomic");
    check(login_verification_write_request({reinterpret_cast<const char*>(request), 3},
                                          {request, sizeof(request)}).error == UserAuthWriteError::invalid_input,
          "verification request rejects aliasing source before writes");
    check(utf8_equal(user_auth_target(UserAuthOperation::verification), utf8_literal("/api/v9/auth/verify")),
          "verification uses allowlisted API target");
    u8 head[1024]{};
    const auto head_written = user_auth_write_request_head(UserAuthOperation::verification, 20, {},
                                                          {head, sizeof(head)});
    constexpr Utf8View head_prefix = utf8_literal("POST /api/v9/auth/verify HTTP/1.1\r\n");
    check(head_written.ok() && utf8_equal({reinterpret_cast<const char*>(head), head_prefix.bytes}, head_prefix),
          "verification transport emits the observed POST route");
    UserAuthState state{};
    user_auth_state_initialize(&state);
    const auto begun = user_auth_state_begin_verification(&state);
    check(begun.ok() && state.phase == UserAuthPhase::verification_pending,
          "verification has its own pending state");
    UserAuthResponse mfa{};
    mfa.kind = UserAuthResponseKind::mfa_required;
    check(user_auth_state_apply(&state, begun.generation, mfa).error == UserAuthTransitionError::invalid_response &&
              state.phase == UserAuthPhase::verification_pending,
          "verification does not invent an MFA continuation contract");
    UserAuthResponse captcha{};
    captcha.kind = UserAuthResponseKind::captcha_required;
    check(user_auth_state_apply(&state, begun.generation, captcha).ok() &&
              state.captcha_return_phase == UserAuthPhase::verification_pending &&
              !user_auth_state_begin_password(&state).ok(),
          "verification CAPTCHA preserves its operation and blocks replacing pending credentials");
    check(user_auth_state_resume_captcha(&state).ok() && state.phase == UserAuthPhase::verification_pending,
          "verification CAPTCHA resumes the original flow");
    const u64 prior_generation = state.generation;
    user_auth_state_cancel(&state);
    UserAuthResponse authenticated{};
    authenticated.kind = UserAuthResponseKind::authenticated;
    authenticated.token_bytes = 23;
    authenticated.has_user_id = true;
    authenticated.user_id = 123;
    check(user_auth_state_apply(&state, prior_generation, authenticated).error == UserAuthTransitionError::stale_generation,
          "cancelled verification cannot install a late token");
    check(user_auth_state_begin_verification(&state).ok(), "verification restarts after cancellation");
    UserAuthResponse incomplete = authenticated;
    incomplete.has_user_id = false;
    check(user_auth_state_apply(&state, state.generation, incomplete).error ==
              UserAuthTransitionError::invalid_response && state.phase == UserAuthPhase::verification_pending,
          "verification requires the observed token and user identity before profile promotion");
    check(
              user_auth_state_apply(&state, state.generation, authenticated).ok() &&
              state.phase == UserAuthPhase::authenticated,
          "verified link success reaches normal credential promotion state");
    u32 scratch[128]{};
    char credential[64]{};
    UserAuthResponseStorage response_storage{};
    response_storage.token = {credential, sizeof(credential)};
    UserAuthResponse decoded{};
    check(user_auth_decode_response(200, utf8_literal("{\"token\":\"synthetic-account-token\",\"user_id\":\"123\"}"),
              {scratch, 128}, 8, response_storage, &decoded).ok() && decoded.kind == UserAuthResponseKind::authenticated &&
              decoded.has_user_id && decoded.user_id == 123 && decoded.token_bytes == 23,
          "observed verification token/user_id response reuses bounded auth decoder");
    secure_clear_bytes(credential, sizeof(credential));
    secure_buffer_release(&token);
}
} // namespace maicord
