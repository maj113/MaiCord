#pragma once

#include "maicord/app/account/account_module_api.hpp"
#include "maicord/app/account/account_profile_promotion.hpp"
#include "maicord/app/account/login/browser_login.hpp"
#include "maicord/app/account/login/captcha_bridge.hpp"
#include "maicord/app/account/login/remote_auth_client.hpp"
#include "maicord/platform/crypto/random.hpp"
#include "maicord/app/account/login/remote_auth_handshake.hpp"
#include <stddef.h>

// Shared login orchestration, kept local to this translation unit for the account module
// and native handoff tests. This adds no worker or other resource.
namespace maicord {
namespace {

struct AccountLoginRuntime {
    UserAuthCoordinator coordinator;
    RemoteAuthClient remote_auth;
    AccountProfilePromotion promotion;
    BrowserLogin browser;
    CaptchaBridge captcha_bridge;
    CaptchaBridgeStatus captcha_status;
    RemoteAuthClientResult remote_status;
    wchar_t profile_path[kAccountProfileMaximumPathCharacters];
    u64 user_id;
    u64 error_code;
    u32 profile_native_error; // profile fault or first stop diagnostic
    u8 mfa_capabilities;
    UserAuthCoordinatorEventKind last_event;
    RemoteAuthClientEventKind last_remote_event;
    AccountLoginError error;
    bool profile_saved;
    bool remote_mode;
    bool remote_active;
    bool coordinator_started;
    bool reset_mode;
    bool recovery_mode;
    bool browser_mode;
    bool authenticated_pending;
    // The last rejection carried a suspended-user token: Discord suspended the account.
    bool account_suspended;
};

// Indexed by UserAuthCoordinatorPhase. Failed, stopping and the browser exchange map to failed, as does
// anything out of range.
constexpr AccountLoginPhase kLoginPhases[] = {
    AccountLoginPhase::empty,
    AccountLoginPhase::idle,
    AccountLoginPhase::password_pending,
    AccountLoginPhase::mfa_required,
    AccountLoginPhase::mfa_pending,
    AccountLoginPhase::mfa_pending, // sms_send_pending
    AccountLoginPhase::captcha_required,
    AccountLoginPhase::authenticated,
    AccountLoginPhase::rejected,
    AccountLoginPhase::authenticated, // session_promoted
    AccountLoginPhase::failed,
    AccountLoginPhase::failed, // stopping
    AccountLoginPhase::stopped,
    AccountLoginPhase::verification_pending,
    AccountLoginPhase::recovery_pending,
    AccountLoginPhase::recovery_sent,
    AccountLoginPhase::recovery_phone_required,
    AccountLoginPhase::recovery_rejected,
    AccountLoginPhase::recovery_rate_limited,
    AccountLoginPhase::reset_pending,
    AccountLoginPhase::reset_mfa_pending,
    AccountLoginPhase::reset_rejected,
    AccountLoginPhase::reset_rate_limited,
    AccountLoginPhase::phone_verify_pending,
    AccountLoginPhase::phone_resend_pending,
    AccountLoginPhase::phone_rejected,
    AccountLoginPhase::phone_rate_limited,
    AccountLoginPhase::phone_token_ready,
    AccountLoginPhase::phone_token_taken,
    AccountLoginPhase::failed, // browser_exchange_pending
};
constexpr u32 kLoginPhaseCount = sizeof(kLoginPhases) / sizeof(kLoginPhases[0]);
static_assert(kLoginPhaseCount == static_cast<u32>(UserAuthCoordinatorPhase::browser_exchange_pending) + 1 &&
              static_cast<u32>(UserAuthCoordinatorPhase::sms_send_pending) == 5 &&
              static_cast<u32>(UserAuthCoordinatorPhase::session_promoted) == 9 &&
              static_cast<u32>(UserAuthCoordinatorPhase::stopped) == 12 &&
              static_cast<u32>(UserAuthCoordinatorPhase::phone_token_taken) == 28);

AccountLoginPhase login_phase(UserAuthCoordinatorPhase phase) noexcept {
    const u32 index = static_cast<u32>(phase);
    return index < kLoginPhaseCount ? kLoginPhases[index] : AccountLoginPhase::failed;
}

AccountLoginPhase remote_login_phase(RemoteAuthClientPhase phase) noexcept {
    switch (phase) {
    case RemoteAuthClientPhase::starting:
    case RemoteAuthClientPhase::resolving:
    case RemoteAuthClientPhase::connecting:
    case RemoteAuthClientPhase::tls_handshake:
    case RemoteAuthClientPhase::websocket_opening:
    case RemoteAuthClientPhase::awaiting_qr:
        return AccountLoginPhase::remote_starting;
    case RemoteAuthClientPhase::qr_ready:
        return AccountLoginPhase::remote_qr_ready;
    case RemoteAuthClientPhase::mobile_pending:
        return AccountLoginPhase::remote_mobile_pending;
    case RemoteAuthClientPhase::ticket_exchange:
        return AccountLoginPhase::remote_ticket_exchange;
    case RemoteAuthClientPhase::captcha_required:
        return AccountLoginPhase::captcha_required;
    case RemoteAuthClientPhase::authenticated:
        return AccountLoginPhase::authenticated;
    case RemoteAuthClientPhase::cancelled:
        return AccountLoginPhase::rejected;
    case RemoteAuthClientPhase::stopped:
        return AccountLoginPhase::stopped;
    case RemoteAuthClientPhase::failed:
        return AccountLoginPhase::failed;
    case RemoteAuthClientPhase::empty:
    case RemoteAuthClientPhase::cancelling:
    case RemoteAuthClientPhase::stopping:
    default:
        return AccountLoginPhase::remote_starting;
    }
}

AccountLoginStatus login_status(AccountLoginRuntime* runtime) noexcept {
    if (!runtime)
        return account_login_invalid_input();
    const UserAuthCoordinatorResult coordinator = runtime->coordinator_started
                                                      ? user_auth_coordinator_status(&runtime->coordinator)
                                                      : UserAuthCoordinatorResult{};
    AccountLoginStatus status{};
    status.user_id = runtime->user_id;
    status.error_code = runtime->error_code;
    status.generation = coordinator.generation;
    status.request_id = coordinator.request_id;
    status.coordinator_response_bytes = coordinator.response_bytes;
    status.coordinator_decode_error_offset = coordinator.decode_error_offset;
    status.remote_ticket_error_code = runtime->remote_status.ticket_error_code;
    status.native_error = coordinator.native_error;
    status.remote_ticket_response_bytes = runtime->remote_status.ticket_response_bytes;
    status.http_status = coordinator.http_status;
    status.mfa_capabilities = runtime->mfa_capabilities;
    status.account_suspended = runtime->account_suspended;
    status.coordinator_error = coordinator.error;
    status.coordinator_decode_error = coordinator.decode_error;
    status.coordinator_scan_error = coordinator.scan_error;
    status.coordinator_read_error = coordinator.read_error;
    status.coordinator_transport_error = coordinator.transport_error;
    status.coordinator_transition_error = coordinator.transition_error;
    status.coordinator_response_kind = coordinator.response_kind;
    status.coordinator_content_type = coordinator.content_type;
    status.coordinator_content_encoding = coordinator.content_encoding;
    status.coordinator_body_kind = coordinator.body_kind;
    status.retry_at_ms = runtime->remote_mode ? runtime->remote_status.retry_at_ms : coordinator.retry_at_ms;
    status.event = runtime->last_event;
    status.remote_event = runtime->last_remote_event;
    status.remote_error = runtime->remote_status.error;
    status.remote_handshake_error = runtime->remote_status.handshake_error;
    status.remote_handshake_stage = runtime->remote_status.handshake_stage;
    status.remote_protocol_error = runtime->remote_status.protocol_error;
    status.remote_crypto_error = runtime->remote_status.crypto_error;
    status.remote_base64_error = runtime->remote_status.base64_error;
    status.remote_ticket_response_kind = runtime->remote_status.ticket_response_kind;
    status.remote_ticket_decode_error = runtime->remote_status.ticket_decode_error;
    status.remote_ticket_decode_error_offset = runtime->remote_status.ticket_decode_error_offset;
    status.remote_ticket_scan_error = runtime->remote_status.ticket_scan_error;
    status.remote_ticket_read_error = runtime->remote_status.ticket_read_error;
    status.remote_ticket_content_type = runtime->remote_status.ticket_content_type;
    status.remote_ticket_content_encoding = runtime->remote_status.ticket_content_encoding;
    status.remote_ticket_body_kind = runtime->remote_status.ticket_body_kind;
    const CaptchaChallengeSnapshot captcha =
        runtime->remote_mode ? CaptchaChallengeSnapshot{}
                             : user_auth_coordinator_captcha_snapshot(&runtime->coordinator);
    status.captcha_provider =
        runtime->remote_mode ? runtime->remote_status.captcha_provider : captcha.provider;
    status.captcha_invisible =
        runtime->remote_mode ? runtime->remote_status.captcha_invisible : captcha.invisible;
    status.phase = runtime->profile_saved || runtime->authenticated_pending ? AccountLoginPhase::authenticated
                   : runtime->remote_mode          ? remote_login_phase(runtime->remote_status.phase)
                   : !runtime->coordinator_started ? AccountLoginPhase::idle
                                                   : login_phase(coordinator.phase);
    if (runtime->remote_mode) {
        status.native_error = runtime->remote_status.native_error;
        status.http_status = runtime->remote_status.http_status;
    }
    if (runtime->error == AccountLoginError::profile || runtime->error == AccountLoginError::stop) {
        status.native_error = runtime->profile_native_error;
    }
    status.error = runtime->error != AccountLoginError::none ? runtime->error
                   : runtime->remote_mode && runtime->remote_status.error != RemoteAuthClientError::none &&
                           runtime->remote_status.error != RemoteAuthClientError::cancelled
                       ? AccountLoginError::remote_auth
                   : coordinator.error == UserAuthCoordinatorError::none ? AccountLoginError::none
                                                                         : AccountLoginError::coordinator;
    status.profile_saved = runtime->profile_saved;
    if (runtime->browser_mode) {
        const auto browser = browser_login_status(&runtime->browser);
        status.browser_generation = browser.generation;
        status.browser_deadline_ms = browser.deadline_ms;
        if (runtime->browser.authenticated) {
            status.phase = AccountLoginPhase::authenticated;
        } else {
            switch (browser.phase) {
            case BrowserSignInReceiverPhase::starting:
                status.phase = AccountLoginPhase::browser_starting;
                break;
            case BrowserSignInReceiverPhase::waiting:
            case BrowserSignInReceiverPhase::candidate_ready:
                status.phase = AccountLoginPhase::browser_waiting;
                break;
            case BrowserSignInReceiverPhase::exchange_pending:
                status.phase = AccountLoginPhase::browser_exchange_pending;
                break;
            case BrowserSignInReceiverPhase::expired:
                status.phase = AccountLoginPhase::browser_expired;
                break;
            default:
                status.phase = AccountLoginPhase::browser_failed;
                break;
            }
        }
        if (browser.error != BrowserSignInReceiverError::none && runtime->error == AccountLoginError::none)
            status.error = AccountLoginError::browser_auth;
    }
    return status;
}

} // namespace
} // namespace maicord
