#include "maicord/app/account_action_captcha.hpp"
#include "maicord/protocol/rest.hpp"

namespace maicord {
namespace {
CaptchaBridgeStatus refused(const AccountActionCaptcha* captcha, CaptchaBridgeError error) noexcept {
    CaptchaBridgeStatus status = captcha ? captcha->status : CaptchaBridgeStatus{};
    status.error = error;
    return status;
}

void stop_bridge(AccountActionCaptcha* captcha) noexcept {
    if (captcha->bridge.runtime)
        captcha->status = captcha_bridge_stop(&captcha->bridge, 5'000);
}

void erase(AccountActionCaptcha* captcha) noexcept {
    stop_bridge(captcha);
    if (captcha->challenge.secrets.data)
        captcha_challenge_clear(&captcha->challenge);
    secure_buffer_release(&captcha->token);
    captcha->challenge_ready = false;
}

// Builds the replay headers in locked memory and arms them on one lane.
bool arm(AccountActionCaptcha* captcha, RestClient* rest, u64 request_id) noexcept {
    SecureBuffer headers{}, rqtoken{}, session{};
    bool ok = rest && captcha->token.bytes &&
              secure_buffer_initialize(&headers, kRestExtraHeadersMaximumBytes).ok() &&
              secure_buffer_initialize(&rqtoken, kCaptchaChallengeRqtokenBytes).ok() &&
              secure_buffer_initialize(&session, kCaptchaChallengeSessionBytes).ok() &&
              captcha_challenge_copy_replay_headers(&captcha->challenge, &rqtoken, &session).ok();
    u32 used = 0;
    if (ok) {
        used = rest_append_extra_header({headers.data, headers.capacity}, used, utf8_literal("X-Captcha-Key"),
                                        secure_buffer_view(captcha->token));
        if (used != ~0u && rqtoken.bytes)
            used = rest_append_extra_header({headers.data, headers.capacity}, used,
                                            utf8_literal("X-Captcha-Rqtoken"), secure_buffer_view(rqtoken));
        if (used != ~0u && session.bytes)
            used = rest_append_extra_header({headers.data, headers.capacity}, used,
                                            utf8_literal("X-Captcha-Session-Id"), secure_buffer_view(session));
        ok = used != ~0u && rest_client_arm_headers(rest, request_id, {headers.data, used});
    }
    secure_buffer_release(&headers);
    secure_buffer_release(&rqtoken);
    secure_buffer_release(&session);
    return ok;
}
} // namespace

CaptchaBridgeStatus account_action_captcha(AccountActionCaptcha* captcha, RestClient* primary,
                                           RestClient* details,
                                           const AccountActionCaptchaRequest& request) noexcept {
    if (!captcha || request.operation > AccountActionCaptchaOperation::cancel)
        return refused(captcha, CaptchaBridgeError::invalid_input);
    switch (request.operation) {
    case AccountActionCaptchaOperation::start: {
        if (!request.generation || request.timeout_ms < 1'000 || request.timeout_ms > kCaptchaBridgeDefaultTimeoutMs ||
            !request.body.data || !request.body.bytes)
            return refused(captcha, CaptchaBridgeError::invalid_input);
        erase(captcha);
        rest_client_arm_headers(primary, 0, {});
        rest_client_arm_headers(details, 0, {});
        if (!captcha->challenge.secrets.data && !captcha_challenge_initialize(&captcha->challenge).ok())
            return refused(captcha, CaptchaBridgeError::memory);
        static u32 scratch[2'048];
        UserAuthResponse response{};
        if (!captcha_challenge_decode(&captcha->challenge, request.status_code, request.body, {scratch, 2'048}, 32,
                                      &response)
                 .ok())
            return refused(captcha, CaptchaBridgeError::rejected);
        const CaptchaChallengeSnapshot snapshot = captcha_challenge_snapshot(&captcha->challenge);
        SecureBuffer sitekey{}, rqdata{};
        const bool copied = secure_buffer_initialize(&sitekey, kCaptchaChallengeSitekeyBytes).ok() &&
                            secure_buffer_initialize(&rqdata, kCaptchaChallengeRqdataBytes).ok() &&
                            captcha_challenge_copy_secret(&captcha->challenge, CaptchaChallengeSecretKind::sitekey,
                                                          &sitekey)
                                .ok();
        if (copied && !captcha_challenge_copy_secret(&captcha->challenge, CaptchaChallengeSecretKind::rqdata, &rqdata)
                           .ok())
            secure_buffer_clear(&rqdata);
        captcha->status =
            copied ? captcha_bridge_start(&captcha->bridge, request.generation, request.timeout_ms,
                                          {{reinterpret_cast<const char*>(sitekey.data), sitekey.bytes},
                                           {reinterpret_cast<const char*>(rqdata.data), rqdata.bytes},
                                           snapshot.provider,
                                           snapshot.invisible})
                   : CaptchaBridgeStatus{request.generation, 0, 0, 0, CaptchaBridgePhase::failed,
                                         CaptchaBridgeError::invalid_input, false};
        secure_buffer_release(&sitekey);
        secure_buffer_release(&rqdata);
        captcha->challenge_ready = captcha->status.ok();
        if (!captcha->status.ok())
            erase(captcha);
        return captcha->status;
    }
    case AccountActionCaptchaOperation::status:
        if (captcha->bridge.runtime) {
            const CaptchaBridgeStatus bridge = captcha_bridge_status(&captcha->bridge);
            captcha->status = bridge;
            if (bridge.phase == CaptchaBridgePhase::token_ready) {
                const bool taken = secure_buffer_initialize(&captcha->token, kUserAuthMaximumCaptchaKeyBytes).ok() &&
                                   captcha_bridge_take_token(&captcha->bridge, bridge.generation, &captcha->token).ok();
                stop_bridge(captcha);
                captcha->status.generation = bridge.generation;
                captcha->status.phase = taken ? CaptchaBridgePhase::token_ready : CaptchaBridgePhase::failed;
                if (!taken)
                    erase(captcha);
            } else if (bridge.phase == CaptchaBridgePhase::expired || bridge.phase == CaptchaBridgePhase::failed ||
                       bridge.phase == CaptchaBridgePhase::cancelled) {
                stop_bridge(captcha);
                captcha->status.phase = bridge.phase;
            }
        }
        return captcha->status;
    case AccountActionCaptchaOperation::copy_url:
        if (!request.url || !captcha->bridge.runtime ||
            !captcha_bridge_copy_url(&captcha->bridge, request.generation, request.url).ok())
            return refused(captcha, CaptchaBridgeError::state);
        return captcha->status;
    case AccountActionCaptchaOperation::arm: {
        if (!request.request_id || !captcha->challenge_ready || !captcha->token.bytes)
            return refused(captcha, CaptchaBridgeError::state);
        const bool armed = arm(captcha, request.details ? details : primary, request.request_id);
        // A single retry: the solution is never offered twice.
        erase(captcha);
        captcha->status.phase = armed ? CaptchaBridgePhase::complete : CaptchaBridgePhase::failed;
        return armed ? captcha->status : refused(captcha, CaptchaBridgeError::state);
    }
    case AccountActionCaptchaOperation::cancel:
        erase(captcha);
        rest_client_arm_headers(primary, 0, {});
        rest_client_arm_headers(details, 0, {});
        captcha->status = {};
        return captcha->status;
    }
    return refused(captcha, CaptchaBridgeError::invalid_input);
}

void account_action_captcha_release(AccountActionCaptcha* captcha, RestClient* primary,
                                    RestClient* details) noexcept {
    if (!captcha)
        return;
    erase(captcha);
    rest_client_arm_headers(primary, 0, {});
    rest_client_arm_headers(details, 0, {});
    captcha_challenge_release(&captcha->challenge);
    *captcha = {};
}
} // namespace maicord
