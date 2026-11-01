#include "user_auth_coordinator_internal.hpp"

namespace maicord {
namespace {

void retain_decoded(UserAuthCoordinatorRuntime* runtime, const UserAuthResponse& response,
                    UserAuthPhase prior_phase) noexcept {
    if (response.kind == UserAuthResponseKind::authenticated) {
        runtime->token_bytes = response.token_bytes;
        clear_tail(runtime, kTokenOffset, runtime->token_bytes, kTokenCapacity);
        clear_mfa(runtime);
        clear_captcha(runtime);
        clear_suspended(runtime);
        return;
    }
    clear_secret(runtime, kTokenOffset, kTokenCapacity);
    runtime->token_bytes = 0;
    if (response.kind == UserAuthResponseKind::mfa_required) {
        runtime->mfa_capabilities = response.mfa_capabilities;
        runtime->ticket_bytes = response.ticket_bytes;
        runtime->login_instance_bytes = response.login_instance_id_bytes;
        runtime->webauthn_bytes = response.webauthn_challenge_bytes;
        clear_tail(runtime, kTicketOffset, runtime->ticket_bytes, kTicketCapacity);
        clear_tail(runtime, kLoginInstanceOffset, runtime->login_instance_bytes, kLoginInstanceCapacity);
        clear_tail(runtime, kWebauthnOffset, runtime->webauthn_bytes, kWebauthnCapacity);
        clear_captcha(runtime);
        clear_suspended(runtime);
        return;
    }
    if (response.kind == UserAuthResponseKind::captcha_required) {
        if (prior_phase != UserAuthPhase::mfa_pending && !runtime->reset_context_active &&
            runtime->active_operation != UserAuthOperation::mfa_sms_send) {
            clear_mfa(runtime);
        } else {
            if (response.ticket_bytes != 0) {
                runtime->ticket_bytes = response.ticket_bytes;
            }
            if (response.login_instance_id_bytes != 0) {
                runtime->login_instance_bytes = response.login_instance_id_bytes;
            }
            if (response.webauthn_challenge_bytes != 0) {
                runtime->webauthn_bytes = response.webauthn_challenge_bytes;
            }
        }
        runtime->captcha_service_bytes = response.captcha_service_bytes;
        runtime->captcha_sitekey_bytes = response.captcha_sitekey_bytes;
        runtime->captcha_rqdata_bytes = response.captcha_rqdata_bytes;
        runtime->captcha_rqtoken_bytes = response.captcha_rqtoken_bytes;
        runtime->captcha_session_bytes = response.captcha_session_id_bytes;
        runtime->captcha_flow_bytes = response.captcha_user_flow_bytes;
        runtime->captcha_invisible = response.captcha_invisible;
        clear_tail(runtime, kCaptchaServiceOffset, runtime->captcha_service_bytes, kCaptchaServiceCapacity);
        clear_tail(runtime, kCaptchaSitekeyOffset, runtime->captcha_sitekey_bytes, kCaptchaSitekeyCapacity);
        clear_tail(runtime, kCaptchaRqdataOffset, runtime->captcha_rqdata_bytes, kCaptchaRqdataCapacity);
        clear_tail(runtime, kCaptchaRqtokenOffset, runtime->captcha_rqtoken_bytes, kCaptchaRqtokenCapacity);
        clear_tail(runtime, kCaptchaSessionOffset, runtime->captcha_session_bytes, kCaptchaSessionCapacity);
        clear_tail(runtime, kCaptchaFlowOffset, runtime->captcha_flow_bytes, kCaptchaFlowCapacity);
        clear_suspended(runtime);
        return;
    }
    clear_mfa(runtime);
    clear_captcha(runtime);
    runtime->suspended_bytes = response.suspended_user_token_bytes;
    clear_tail(runtime, kSuspendedOffset, runtime->suspended_bytes, kSuspendedCapacity);
}

UserAuthCoordinatorEventKind event_kind(UserAuthResponseKind kind) noexcept {
    switch (kind) {
    case UserAuthResponseKind::authenticated:
        return UserAuthCoordinatorEventKind::authenticated;
    case UserAuthResponseKind::mfa_required:
        return UserAuthCoordinatorEventKind::mfa_required;
    case UserAuthResponseKind::captcha_required:
        return UserAuthCoordinatorEventKind::captcha_required;
    case UserAuthResponseKind::rejected:
        return UserAuthCoordinatorEventKind::rejected;
    case UserAuthResponseKind::recovery_sent: return UserAuthCoordinatorEventKind::recovery_sent;
    case UserAuthResponseKind::recovery_phone_required: return UserAuthCoordinatorEventKind::recovery_phone_required;
    default:
        return UserAuthCoordinatorEventKind::protocol_error;
    }
}

// Ends the flow on a transport, decode or state failure and discards every retained secret.
bool fail_flow(UserAuthCoordinator* coordinator, UserAuthCoordinatorRuntime* runtime,
               UserAuthCoordinatorEvent& event, UserAuthCoordinatorEventKind kind,
               UserAuthCoordinatorError error) noexcept {
    event.kind = kind;
    coordinator->state.error = error;
    coordinator->state.phase = UserAuthCoordinatorPhase::failed;
    clear_request(runtime); clear_retained(runtime); user_auth_state_cancel(&runtime->flow);
    return true;
}

void record_decode(UserAuthCoordinator* coordinator, UserAuthCoordinatorEvent& event,
                   const UserAuthDecodeResult& decode) noexcept {
    event.decode_error = decode.error;
    coordinator->state.decode_error = decode.error;
    coordinator->state.decode_error_offset = decode.error_offset;
    coordinator->state.scan_error = decode.scan_error;
    coordinator->state.read_error = decode.read_error;
}

// Ends a login-flow request on a transport, decode or state failure. Operations
// that ran on retained credentials also drop them.
bool fail_request(UserAuthCoordinator* coordinator, UserAuthCoordinatorRuntime* runtime,
                  UserAuthCoordinatorEvent& event, UserAuthCoordinatorEventKind kind,
                  UserAuthCoordinatorError error) noexcept {
    event.kind = kind;
    coordinator->state.error = error;
    coordinator->state.phase = UserAuthCoordinatorPhase::failed;
    runtime->active_request_id = 0;
    clear_request(runtime);
    if (runtime->active_operation == UserAuthOperation::verification ||
        runtime->active_operation == UserAuthOperation::recovery ||
        runtime->active_operation == UserAuthOperation::browser_handoff_exchange)
        clear_retained(runtime);
    return true;
}

// All reset responses are accepted by request id/generation before this path.
// Classify first so auth-shaped SMS failures cannot overwrite session storage.
bool consume_reset_response(UserAuthCoordinator* coordinator, UserAuthCoordinatorRuntime* runtime,
    UserAuthCoordinatorEvent& event, const RestClientResponse& transport, ConstSlice<u8> body) noexcept {
    const bool sms = runtime->active_operation == UserAuthOperation::mfa_sms_send;
    const bool final = runtime->active_operation == UserAuthOperation::reset_mfa;
    const Utf8View json{reinterpret_cast<const char*>(body.data), body.count};
    UserAuthResponse decoded{};
    UserAuthDecodeResult decode{};
    bool sms_success = false;
    if (transport.error == RestClientError::none) {
        if (sms) {
            decode = user_auth_classify_reset_response(transport.status_code, json,
                {runtime->structurals, kStructuralCount}, 16, &decoded, true);
            // The SMS endpoint has a neutral success schema. Still validate JSON
            // using the recovery classifier, which forbids auth-shaped objects.
            if (transport.status_code >= 200 && transport.status_code < 300) {
                decode = user_auth_decode_recovery_response(transport.status_code, json,
                    {runtime->structurals, kStructuralCount}, 16, response_storage(runtime), &decoded);
                sms_success = decode.ok() && decoded.kind == UserAuthResponseKind::recovery_sent;
            } else if (decode.ok() && decoded.kind == UserAuthResponseKind::captcha_required) {
                if (decoded.ticket_bytes || decoded.login_instance_id_bytes || decoded.webauthn_challenge_bytes)
                    decode.error = UserAuthDecodeError::unexpected_response;
                if (decode.ok()) decode = user_auth_decode_reset_response(transport.status_code, json,
                    {runtime->structurals, kStructuralCount}, 16, response_storage(runtime), &decoded, true);
            } else if (decode.ok() && decoded.kind != UserAuthResponseKind::rejected) {
                decode.error = UserAuthDecodeError::unexpected_response;
            }
        } else {
            decode = user_auth_classify_reset_response(transport.status_code, json,
                {runtime->structurals, kStructuralCount}, 16, &decoded, final);
            if (decode.ok() && final && decoded.kind == UserAuthResponseKind::captcha_required &&
                (decoded.ticket_bytes || decoded.login_instance_id_bytes || decoded.webauthn_challenge_bytes))
                decode.error = UserAuthDecodeError::unexpected_response;
            if (decode.ok() && decoded.kind != UserAuthResponseKind::rejected)
                decode = user_auth_decode_reset_response(transport.status_code, json,
                    {runtime->structurals, kStructuralCount}, 16, response_storage(runtime), &decoded, final);
        }
    }
    runtime->active_request_id = 0;
    record_decode(coordinator, event, decode);
    coordinator->state.response_kind = decoded.kind;
    if (transport.error != RestClientError::none)
        return fail_flow(coordinator, runtime, event, UserAuthCoordinatorEventKind::transport_error,
                         UserAuthCoordinatorError::transport);
    // A 429 is a definitive rejection, including a body the categorical
    // decoder cannot understand. Only a transport-bound deadline permits retry.
    if (transport.status_code == 429 && transport.retry_at_ms != 0) {
        if (final || sms) restore_reset_base(runtime);
        else { preserve_reset_prefix(runtime); clear_retained(runtime); }
        clear_captcha(runtime);
        runtime->continuation_retry_at_ms = transport.retry_at_ms;
        runtime->flow.phase = final || sms ? UserAuthPhase::mfa_required : UserAuthPhase::rejected;
        event.kind = UserAuthCoordinatorEventKind::rejected;
        coordinator->state.error = UserAuthCoordinatorError::none;
        coordinator->state.phase = UserAuthCoordinatorPhase::reset_rate_limited;
        return true;
    }
    if (!decode.ok())
        return fail_flow(coordinator, runtime, event, UserAuthCoordinatorEventKind::protocol_error,
                         UserAuthCoordinatorError::decode);
    event.response = decoded;
    runtime->continuation_retry_at_ms = 0;
    if (sms_success) {
        restore_reset_base(runtime); clear_captcha(runtime);
        runtime->flow.phase = UserAuthPhase::mfa_required;
        event.kind = UserAuthCoordinatorEventKind::sms_code_sent;
        coordinator->state.phase = UserAuthCoordinatorPhase::mfa_required;
        return true;
    }
    if (decoded.kind == UserAuthResponseKind::captcha_required) {
        const UserAuthPhase prior = final || sms ? UserAuthPhase::mfa_pending : UserAuthPhase::reset_pending;
        retain_decoded(runtime, decoded, prior);
        runtime->flow.captcha_return_phase = prior;
        runtime->flow.phase = UserAuthPhase::captcha_required;
        ++runtime->captcha_revision;
        event.kind = UserAuthCoordinatorEventKind::captcha_required;
        coordinator->state.phase = UserAuthCoordinatorPhase::captcha_required;
        return true;
    }
    if (decoded.kind == UserAuthResponseKind::mfa_required && !sms && !final) {
        retain_decoded(runtime, decoded, UserAuthPhase::reset_pending);
        restore_reset_base(runtime);
        runtime->flow.phase = UserAuthPhase::mfa_required;
        event.kind = UserAuthCoordinatorEventKind::mfa_required;
        coordinator->state.phase = UserAuthCoordinatorPhase::mfa_required;
        return true;
    }
    if (decoded.kind == UserAuthResponseKind::authenticated && !sms) {
        retain_decoded(runtime, decoded, runtime->flow.phase);
        clear_request(runtime);
        runtime->flow.phase = UserAuthPhase::authenticated;
        event.kind = UserAuthCoordinatorEventKind::authenticated;
        coordinator->state.phase = UserAuthCoordinatorPhase::authenticated;
        return true;
    }
    if (decoded.kind == UserAuthResponseKind::rejected) {
        const bool retry_initial = !sms && !final && transport.status_code == 400;
        const bool retry_mfa = final && decoded.has_error_code && login_mfa_invalid_error(decoded.error_code);
        const bool retry_sms = sms && transport.status_code == 400;
        if (retry_initial) { preserve_reset_prefix(runtime); clear_retained(runtime); }
        else if (retry_mfa || retry_sms) { restore_reset_base(runtime); clear_captcha(runtime); }
        else { clear_request(runtime); clear_retained(runtime); }
        runtime->flow.phase = retry_mfa || retry_sms ? UserAuthPhase::mfa_required : UserAuthPhase::rejected;
        event.kind = UserAuthCoordinatorEventKind::rejected;
        coordinator->state.phase = retry_mfa || retry_sms ? UserAuthCoordinatorPhase::mfa_required :
            UserAuthCoordinatorPhase::reset_rejected;
        return true;
    }
    return fail_flow(coordinator, runtime, event, UserAuthCoordinatorEventKind::protocol_error,
                     UserAuthCoordinatorError::decode);
}

bool consume_phone_response(UserAuthCoordinator* coordinator, UserAuthCoordinatorRuntime* runtime,
    UserAuthCoordinatorEvent& event, const RestClientResponse& transport,
    ConstSlice<u8> locked_body) noexcept {
    const bool verify = runtime->active_operation == UserAuthOperation::phone_verify;
    if (transport.error != RestClientError::none) {
        runtime->active_request_id = 0;
        return fail_flow(coordinator, runtime, event, UserAuthCoordinatorEventKind::transport_error,
                         UserAuthCoordinatorError::transport);
    }
    UserAuthResponse decoded{};
    auto storage = response_storage(runtime);
    storage.token = {reinterpret_cast<char*>(runtime->secrets.data + kPhoneTokenOffset), kUserAuthPhoneResetTokenBytes};
    const auto decode = user_auth_decode_phone_response(transport.status_code,
        {reinterpret_cast<const char*>(locked_body.data), locked_body.count},
        {runtime->structurals, kStructuralCount}, 16, storage, &decoded, verify);
    record_decode(coordinator, event, decode);
    runtime->active_request_id = 0;
    if (!decode.ok() && transport.status_code != 429)
        return fail_flow(coordinator, runtime, event, UserAuthCoordinatorEventKind::protocol_error,
                         UserAuthCoordinatorError::decode);
    if (transport.status_code == 429 || decoded.kind == UserAuthResponseKind::rejected) {
        event.kind = UserAuthCoordinatorEventKind::phone_rejected;
        event.response = decoded;
        clear_retained(runtime);
        restore_phone_base(runtime);
        runtime->flow.phase = UserAuthPhase::recovery_phone_required;
        runtime->continuation_retry_at_ms = transport.status_code == 429 ? transport.retry_at_ms : 0;
        coordinator->state.error = UserAuthCoordinatorError::none;
        coordinator->state.phase = transport.status_code == 429 ?
            UserAuthCoordinatorPhase::phone_rate_limited : UserAuthCoordinatorPhase::phone_rejected;
        coordinator->state.response_kind = decoded.kind;
        return true;
    }
    const auto applied = user_auth_state_apply(&runtime->flow, runtime->active_generation, decoded);
    if (!applied.ok()) {
        coordinator->state.transition_error = applied.error;
        return fail_flow(coordinator, runtime, event, UserAuthCoordinatorEventKind::protocol_error,
                         UserAuthCoordinatorError::state);
    }
    coordinator->state.response_kind = decoded.kind;
    runtime->continuation_retry_at_ms = 0;
    if (decoded.kind == UserAuthResponseKind::captcha_required) {
        retain_decoded(runtime, decoded, UserAuthPhase::phone_pending);
        event.kind = UserAuthCoordinatorEventKind::captcha_required;
        coordinator->state.phase = UserAuthCoordinatorPhase::captcha_required;
    } else if (decoded.kind == UserAuthResponseKind::phone_verified_reset) {
        // A reset credential lives only in its dedicated request-tail stage.
        clear_retained(runtime);
        runtime->continuation_token_bytes = decoded.token_bytes;
        secure_clear_bytes(runtime->secrets.data + kPhoneTokenOffset + decoded.token_bytes,
            kUserAuthPhoneResetTokenBytes - decoded.token_bytes);
        secure_clear_bytes(runtime->secrets.data, kPhoneTokenOffset);
        runtime->continuation_base_bytes = 0;
        runtime->retained_request_bytes = 0;
        runtime->active_body_offset = 0;
        event.kind = UserAuthCoordinatorEventKind::phone_verified_reset;
        coordinator->state.phase = UserAuthCoordinatorPhase::phone_token_ready;
    } else {
        clear_retained(runtime); restore_phone_base(runtime);
        event.kind = UserAuthCoordinatorEventKind::phone_code_sent;
        coordinator->state.phase = UserAuthCoordinatorPhase::recovery_phone_required;
    }
    event.response = decoded;
    return true;
}

} // namespace
namespace user_auth_coordinator_detail {

bool consume_response(void* opaque, const RestClientResponse& transport_response,
                      ConstSlice<u8> locked_body) noexcept {
    auto* context = static_cast<ConsumeContext*>(opaque);
    UserAuthCoordinator* coordinator = context->coordinator;
    UserAuthCoordinatorRuntime* runtime = runtime_of(coordinator);
    UserAuthCoordinatorEvent& event = *context->event;
    reset_value<UserAuthCoordinatorEvent>(&event);
    // All operations, including same-generation SMS send, require the current
    // accepted request before response metadata or protected storage is touched.
    if (!runtime->active_request_id || transport_response.request_id != runtime->active_request_id ||
        runtime->active_generation != runtime->flow.generation ||
        coordinator->state.phase == UserAuthCoordinatorPhase::stopping ||
        runtime->active_captcha_revision != runtime->captcha_revision) return true;
    event.request_id = transport_response.request_id;
    event.generation = runtime->active_generation;
    // Header timing is metadata on any status; only a received 429 imposes a
    // manual authentication cooldown.
    const u64 retry_at_ms = transport_response.transport_ok() && transport_response.status_code == 429
                                ? transport_response.retry_at_ms : 0;
    event.retry_at_ms = retry_at_ms;
    event.http_status = transport_response.status_code;
    event.transport_error = transport_response.error;

    coordinator->state.request_id = transport_response.request_id;
    coordinator->state.generation = runtime->active_generation;
    coordinator->state.http_status = transport_response.status_code;
    coordinator->state.response_bytes = transport_response.body_bytes;
    coordinator->state.content_type = transport_response.content_type;
    coordinator->state.content_encoding = transport_response.content_encoding;
    coordinator->state.body_kind = transport_response.body_kind;
    coordinator->state.transport_error = transport_response.error;
    coordinator->state.retry_at_ms = retry_at_ms;
    if (runtime->reset_context_active)
        return consume_reset_response(coordinator, runtime, event, transport_response, locked_body);
    if (runtime->phone_context_active)
        return consume_phone_response(coordinator, runtime, event, transport_response, locked_body);

    if (transport_response.error != RestClientError::none)
        return fail_request(coordinator, runtime, event, UserAuthCoordinatorEventKind::transport_error,
                            UserAuthCoordinatorError::transport);

    if (runtime->active_operation == UserAuthOperation::mfa_sms_send &&
        transport_response.status_code >= 200 && transport_response.status_code < 300) {
        event.kind = UserAuthCoordinatorEventKind::sms_code_sent;
        reset_value<UserAuthCoordinatorResult>(&coordinator->state);
        coordinator->state.generation = runtime->flow.generation;
        coordinator->state.request_id = transport_response.request_id;
        coordinator->state.http_status = transport_response.status_code;
        coordinator->state.phase = UserAuthCoordinatorPhase::mfa_required;
        runtime->active_request_id = 0;
        clear_request(runtime);
        return true;
    }

    UserAuthResponse decoded{};
    const auto decode_response = runtime->active_operation == UserAuthOperation::recovery ?
        user_auth_decode_recovery_response : runtime->active_operation == UserAuthOperation::browser_handoff_exchange ?
        user_auth_decode_browser_handoff_response : user_auth_decode_response;
    const Utf8View response_body{reinterpret_cast<const char*>(locked_body.data), locked_body.count};
    bool invalid_mfa_retry = false;
    UserAuthDecodeResult decode{};
    const bool ordinary_mfa = runtime->active_operation == UserAuthOperation::mfa_totp ||
        runtime->active_operation == UserAuthOperation::mfa_sms ||
        runtime->active_operation == UserAuthOperation::mfa_backup ||
        runtime->active_operation == UserAuthOperation::mfa_webauthn;
    if (ordinary_mfa &&
        runtime->flow.phase == UserAuthPhase::mfa_pending && runtime->ticket_bytes &&
        transport_response.status_code == 400) {
        // Classify before copying optional response fields into the retained
        // ticket/challenge partitions. An invalid-code response cannot replace
        // the original method context or identity with response-owned values.
        decode = user_auth_classify_response(transport_response.status_code, response_body,
            {runtime->structurals, kStructuralCount}, 16, &decoded);
        invalid_mfa_retry = decode.ok() && decoded.kind == UserAuthResponseKind::rejected &&
            decoded.has_error_code && login_mfa_invalid_error(decoded.error_code) && !decoded.suspended;
    }
    if (!invalid_mfa_retry)
        decode = decode_response(transport_response.status_code, response_body,
            {runtime->structurals, kStructuralCount}, 16, response_storage(runtime), &decoded);
    record_decode(coordinator, event, decode);
    if (!decode.ok()) {
        if (runtime->active_operation == UserAuthOperation::recovery && transport_response.status_code == 429) {
            event.kind = UserAuthCoordinatorEventKind::recovery_rejected;
            coordinator->state.error = UserAuthCoordinatorError::none;
            coordinator->state.phase = UserAuthCoordinatorPhase::recovery_rate_limited;
            runtime->flow.phase = UserAuthPhase::rejected;
            runtime->active_request_id = 0;
            clear_request(runtime); clear_retained(runtime);
            return true;
        }
        return fail_request(coordinator, runtime, event, UserAuthCoordinatorEventKind::protocol_error,
                            UserAuthCoordinatorError::decode);
    }

    if (invalid_mfa_retry) {
        clear_request(runtime);
        clear_secret(runtime, kTokenOffset, kTokenCapacity);
        runtime->token_bytes = 0;
        clear_captcha(runtime);
        clear_suspended(runtime);
        runtime->active_request_id = 0;
        runtime->flow.phase = UserAuthPhase::mfa_required;
        decoded.mfa_capabilities = runtime->mfa_capabilities;
        decoded.user_id = 0;
        decoded.has_user_id = false;
        event.response = decoded;
        event.kind = UserAuthCoordinatorEventKind::rejected;
        coordinator->state.error = UserAuthCoordinatorError::none;
        coordinator->state.response_kind = UserAuthResponseKind::rejected;
        coordinator->state.phase = UserAuthCoordinatorPhase::mfa_required;
        return true;
    }

    const UserAuthPhase prior_phase = runtime->flow.phase;
    if (runtime->active_operation == UserAuthOperation::mfa_sms_send) {
        event.response = decoded;
        if (decoded.kind == UserAuthResponseKind::captcha_required) {
            retain_decoded(runtime, decoded, UserAuthPhase::mfa_pending);
            runtime->flow.phase = UserAuthPhase::captcha_required;
            runtime->flow.captcha_return_phase = UserAuthPhase::mfa_pending;
            event.kind = UserAuthCoordinatorEventKind::captcha_required;
            coordinator->state.phase = UserAuthCoordinatorPhase::captcha_required;
        } else if (decoded.kind == UserAuthResponseKind::rejected) {
            clear_captcha(runtime);
            runtime->flow.phase = UserAuthPhase::mfa_required;
            event.kind = UserAuthCoordinatorEventKind::rejected;
            coordinator->state.phase = UserAuthCoordinatorPhase::mfa_required;
            clear_request(runtime);
        } else {
            event.kind = UserAuthCoordinatorEventKind::protocol_error;
            coordinator->state.error = UserAuthCoordinatorError::decode;
            coordinator->state.phase = UserAuthCoordinatorPhase::failed;
            clear_request(runtime); clear_retained(runtime);
        }
        runtime->active_request_id = 0;
        return true;
    }

    const UserAuthTransitionResult applied =
        user_auth_state_apply(&runtime->flow, runtime->active_generation, decoded);
    if (!applied.ok()) {
        coordinator->state.transition_error = applied.error;
        coordinator->state.response_kind = decoded.kind;
        return fail_request(coordinator, runtime, event, UserAuthCoordinatorEventKind::protocol_error,
                            UserAuthCoordinatorError::state);
    }
    retain_decoded(runtime, decoded, prior_phase);
    event.response = decoded;
    event.kind = event_kind(decoded.kind);
    reset_value<UserAuthCoordinatorResult>(&coordinator->state);
    coordinator->state.generation = runtime->flow.generation;
    coordinator->state.request_id = transport_response.request_id;
    coordinator->state.http_status = transport_response.status_code;
    coordinator->state.response_bytes = transport_response.body_bytes;
    coordinator->state.response_kind = decoded.kind;
    coordinator->state.content_type = transport_response.content_type;
    coordinator->state.content_encoding = transport_response.content_encoding;
    coordinator->state.body_kind = transport_response.body_kind;
    coordinator->state.phase = coordinator_phase(runtime->flow.phase);
    coordinator->state.retry_at_ms = retry_at_ms;
    if (runtime->active_operation == UserAuthOperation::recovery && decoded.kind != UserAuthResponseKind::captcha_required) {
        clear_retained(runtime);
        if (decoded.kind == UserAuthResponseKind::rejected) {
            event.kind = UserAuthCoordinatorEventKind::recovery_rejected;
            coordinator->state.phase = transport_response.status_code == 429 ?
                UserAuthCoordinatorPhase::recovery_rate_limited : UserAuthCoordinatorPhase::recovery_rejected;
        }
    }
    runtime->active_request_id = 0;
    if (decoded.kind == UserAuthResponseKind::recovery_phone_required) {
        const auto base = user_auth_recovery_phone_base({runtime->secrets.data, kPhoneTokenOffset}, runtime->retained_request_bytes);
        if (!base.ok() || runtime->reset_owner) {
            fail_flow(coordinator, runtime, event, UserAuthCoordinatorEventKind::protocol_error,
                      UserAuthCoordinatorError::request_write);
        } else {
            runtime->phone_context_active = true;
            runtime->continuation_base_bytes = base.bytes;
            runtime->continuation_token_bytes = 0;
            restore_phone_base(runtime);
            secure_clear_bytes(runtime->secrets.data + kPhoneTokenOffset, kUserAuthPhoneResetTokenBytes);
        }
        return true;
    }
    if (decoded.kind != UserAuthResponseKind::captcha_required) {
        clear_request(runtime);
    }
    return true;
}

} // namespace user_auth_coordinator_detail
} // namespace maicord
