#include "maicord/core/win32_process.hpp"
#include "maicord/app/user_auth_coordinator.hpp"

#include "maicord/app/login_mfa_policy.hpp"
#include "maicord/protocol/login_continuations.hpp"
#include "maicord/protocol/browser_sign_in.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kStructuralCount = 16 * 1024;
inline constexpr u32 kScratchOffset = 0;
inline constexpr u32 kScratchCapacity = kUserAuthCoordinatorRequestBytes;
inline constexpr u32 kPhoneTokenOffset = 28 * 1024;
static_assert(kPhoneTokenOffset + kUserAuthPhoneResetTokenBytes == kScratchCapacity);
inline constexpr u32 kTokenOffset = kScratchOffset + kScratchCapacity;
inline constexpr u32 kTokenCapacity = kUserAuthCoordinatorMaximumTokenBytes;
inline constexpr u32 kTicketOffset = kTokenOffset + kTokenCapacity;
inline constexpr u32 kTicketCapacity = kUserAuthMaximumTicketBytes;
inline constexpr u32 kLoginInstanceOffset = kTicketOffset + kTicketCapacity;
inline constexpr u32 kLoginInstanceCapacity = kUserAuthMaximumLoginInstanceBytes;
inline constexpr u32 kWebauthnOffset = kLoginInstanceOffset + kLoginInstanceCapacity;
inline constexpr u32 kWebauthnCapacity = kUserAuthMaximumWebauthnChallengeBytes;
inline constexpr u32 kCaptchaServiceOffset = kWebauthnOffset + kWebauthnCapacity;
inline constexpr u32 kCaptchaServiceCapacity = 128;
inline constexpr u32 kCaptchaSitekeyOffset = kCaptchaServiceOffset + kCaptchaServiceCapacity;
inline constexpr u32 kCaptchaSitekeyCapacity = 256;
inline constexpr u32 kCaptchaRqdataOffset = kCaptchaSitekeyOffset + kCaptchaSitekeyCapacity;
inline constexpr u32 kCaptchaRqdataCapacity = 4 * 1024;
inline constexpr u32 kCaptchaRqtokenOffset = kCaptchaRqdataOffset + kCaptchaRqdataCapacity;
inline constexpr u32 kCaptchaRqtokenCapacity = kUserAuthMaximumCaptchaTokenBytes;
inline constexpr u32 kCaptchaSessionOffset = kCaptchaRqtokenOffset + kCaptchaRqtokenCapacity;
inline constexpr u32 kCaptchaSessionCapacity = kUserAuthMaximumCaptchaSessionBytes;
inline constexpr u32 kCaptchaFlowOffset = kCaptchaSessionOffset + kCaptchaSessionCapacity;
inline constexpr u32 kCaptchaFlowCapacity = 128;
inline constexpr u32 kSuspendedOffset = kCaptchaFlowOffset + kCaptchaFlowCapacity;
inline constexpr u32 kSuspendedCapacity = 1024;
static_assert(kSuspendedOffset + kSuspendedCapacity == kUserAuthCoordinatorSecretBytes);

struct UserAuthCoordinatorRuntime {
    SecureBuffer secrets;
    UserAuthState flow;
    UserAuthResponse last_response;
    u64 active_generation;
    u64 active_request_id;
    u32 retained_request_bytes;
    u32 request_capacity;
    u32 active_body_offset;
    // Reset and phone contexts are mutually exclusive. In reset mode these
    // are the initial body/prefix; in phone mode the fixed body/stage.
    u32 continuation_base_bytes;
    u32 continuation_token_bytes;
    u64 captcha_revision;
    u64 active_captcha_revision;
    u64 continuation_retry_at_ms;
    u8 mfa_capabilities;
    bool reset_owner;
    bool reset_context_active;
    bool reset_prefix_retry;
    u32 token_bytes;
    u32 ticket_bytes;
    u32 login_instance_bytes;
    u32 webauthn_bytes;
    u32 captcha_service_bytes;
    u32 captcha_sitekey_bytes;
    u32 captcha_rqdata_bytes;
    u32 captcha_rqtoken_bytes;
    u32 captcha_session_bytes;
    u32 captcha_flow_bytes;
    u32 suspended_bytes;
    UserAuthOperation active_operation;
    bool transport_stopped;
    bool phone_context_active;
    bool captcha_invisible;
    u32 structurals[kStructuralCount];
};

u32 page_round(u64 bytes) noexcept {
    const u64 rounded = (bytes + 4095ull) & ~4095ull;
    return rounded <= ~0u ? static_cast<u32>(rounded) : 0;
}

UserAuthCoordinatorRuntime* runtime_of(UserAuthCoordinator* coordinator) noexcept {
    return coordinator ? static_cast<UserAuthCoordinatorRuntime*>(coordinator->runtime) : nullptr;
}

const UserAuthCoordinatorRuntime* runtime_of(const UserAuthCoordinator* coordinator) noexcept {
    return coordinator ? static_cast<const UserAuthCoordinatorRuntime*>(coordinator->runtime) : nullptr;
}

bool locked_input(const SecureBuffer* value, u32 maximum) noexcept {
    return value && value->data && value->locked && value->bytes != 0 && value->bytes <= value->capacity &&
           value->bytes <= maximum;
}

Utf8View secure_utf8(const SecureBuffer& value) noexcept {
    return Utf8View{reinterpret_cast<const char*>(value.data), value.bytes};
}

u8* secret(UserAuthCoordinatorRuntime* runtime, u32 offset) noexcept {
    return runtime->secrets.data + offset +
        (offset >= kTokenOffset ? runtime->request_capacity - kScratchCapacity : 0);
}

const u8* secret(const UserAuthCoordinatorRuntime* runtime, u32 offset) noexcept {
    return runtime->secrets.data + offset +
        (offset >= kTokenOffset ? runtime->request_capacity - kScratchCapacity : 0);
}

void clear_secret(UserAuthCoordinatorRuntime* runtime, u32 offset, u32 capacity) noexcept {
    SecureZeroMemory(secret(runtime, offset), capacity);
}

void clear_retained(UserAuthCoordinatorRuntime* runtime) noexcept {
    SecureZeroMemory(secret(runtime, kTokenOffset), kUserAuthCoordinatorSecretBytes - kTokenOffset);
    runtime->token_bytes = 0;
    runtime->ticket_bytes = 0;
    runtime->mfa_capabilities = 0;
    runtime->login_instance_bytes = 0;
    runtime->webauthn_bytes = 0;
    runtime->captcha_service_bytes = 0;
    runtime->captcha_sitekey_bytes = 0;
    runtime->captcha_rqdata_bytes = 0;
    runtime->captcha_rqtoken_bytes = 0;
    runtime->captcha_session_bytes = 0;
    runtime->captcha_flow_bytes = 0;
    runtime->captcha_invisible = false;
    runtime->suspended_bytes = 0;
    runtime->last_response = UserAuthResponse{};
}

void clear_request(UserAuthCoordinatorRuntime* runtime) noexcept {
    SecureZeroMemory(secret(runtime, kScratchOffset), runtime->request_capacity);
    runtime->retained_request_bytes = 0;
    runtime->active_body_offset = 0;
    runtime->continuation_base_bytes = 0;
    runtime->continuation_token_bytes = 0;
    runtime->reset_context_active = false;
    runtime->reset_prefix_retry = false;
    runtime->continuation_retry_at_ms = 0;
    runtime->phone_context_active = false;
}

void restore_phone_base(UserAuthCoordinatorRuntime* runtime) noexcept {
    if (!runtime->continuation_base_bytes || runtime->continuation_base_bytes > kPhoneTokenOffset) return;
    const u32 end = runtime->continuation_base_bytes - 1;
    SecureZeroMemory(runtime->secrets.data + end, kPhoneTokenOffset - end);
    runtime->secrets.data[end] = '}';
    runtime->retained_request_bytes = 0;
    runtime->active_body_offset = 0;
}

bool overlaps_auth_owner(const UserAuthCoordinator* coordinator, const UserAuthCoordinatorRuntime* runtime,
    const void* data, u32 bytes) noexcept {
    const ConstSlice<u8> span{static_cast<const u8*>(data), bytes};
    return !byte_range_valid(span) || byte_ranges_overlap(span, {runtime->secrets.data, runtime->secrets.capacity}) ||
        byte_ranges_overlap(span, {reinterpret_cast<const u8*>(runtime), sizeof(*runtime)}) ||
        byte_ranges_overlap(span, {reinterpret_cast<const u8*>(coordinator), sizeof(*coordinator)});
}

void restore_reset_base(UserAuthCoordinatorRuntime* runtime) noexcept {
    if (runtime->continuation_base_bytes) {
        const u32 end = runtime->continuation_base_bytes - 1;
        SecureZeroMemory(runtime->secrets.data + end, runtime->request_capacity - end);
        runtime->secrets.data[end] = '}';
    }
    runtime->active_body_offset = 0;
    runtime->retained_request_bytes = 0;
}

void preserve_reset_prefix(UserAuthCoordinatorRuntime* runtime) noexcept {
    SecureZeroMemory(runtime->secrets.data + runtime->continuation_token_bytes,
                     runtime->request_capacity - runtime->continuation_token_bytes);
    runtime->continuation_base_bytes = 0;
    runtime->retained_request_bytes = 0;
    runtime->active_body_offset = 0;
    runtime->reset_prefix_retry = true;
}

void clear_mfa(UserAuthCoordinatorRuntime* runtime) noexcept {
    clear_secret(runtime, kTicketOffset, kTicketCapacity);
    clear_secret(runtime, kLoginInstanceOffset, kLoginInstanceCapacity);
    clear_secret(runtime, kWebauthnOffset, kWebauthnCapacity);
    runtime->ticket_bytes = 0;
    runtime->mfa_capabilities = 0;
    runtime->login_instance_bytes = 0;
    runtime->webauthn_bytes = 0;
}

void clear_captcha(UserAuthCoordinatorRuntime* runtime) noexcept {
    ++runtime->captcha_revision;
    clear_secret(runtime, kCaptchaServiceOffset, kCaptchaServiceCapacity);
    clear_secret(runtime, kCaptchaSitekeyOffset, kCaptchaSitekeyCapacity);
    clear_secret(runtime, kCaptchaRqdataOffset, kCaptchaRqdataCapacity);
    clear_secret(runtime, kCaptchaRqtokenOffset, kCaptchaRqtokenCapacity);
    clear_secret(runtime, kCaptchaSessionOffset, kCaptchaSessionCapacity);
    clear_secret(runtime, kCaptchaFlowOffset, kCaptchaFlowCapacity);
    runtime->captcha_service_bytes = 0;
    runtime->captcha_sitekey_bytes = 0;
    runtime->captcha_rqdata_bytes = 0;
    runtime->captcha_rqtoken_bytes = 0;
    runtime->captcha_session_bytes = 0;
    runtime->captcha_flow_bytes = 0;
    runtime->captcha_invisible = false;
}

void clear_suspended(UserAuthCoordinatorRuntime* runtime) noexcept {
    clear_secret(runtime, kSuspendedOffset, kSuspendedCapacity);
    runtime->suspended_bytes = 0;
}

void clear_tail(UserAuthCoordinatorRuntime* runtime, u32 offset, u32 bytes, u32 capacity) noexcept {
    if (bytes < capacity) {
        SecureZeroMemory(secret(runtime, offset) + bytes, capacity - bytes);
    }
}

Slice<char> char_storage(UserAuthCoordinatorRuntime* runtime, u32 offset, u32 capacity) noexcept {
    return Slice<char>{reinterpret_cast<char*>(secret(runtime, offset)), capacity};
}

UserAuthResponseStorage response_storage(UserAuthCoordinatorRuntime* runtime) noexcept {
    return UserAuthResponseStorage{char_storage(runtime, kTokenOffset, kTokenCapacity),
                                   char_storage(runtime, kTicketOffset, kTicketCapacity),
                                   char_storage(runtime, kLoginInstanceOffset, kLoginInstanceCapacity),
                                   char_storage(runtime, kWebauthnOffset, kWebauthnCapacity),
                                   char_storage(runtime, kCaptchaServiceOffset, kCaptchaServiceCapacity),
                                   char_storage(runtime, kCaptchaSitekeyOffset, kCaptchaSitekeyCapacity),
                                   char_storage(runtime, kCaptchaRqdataOffset, kCaptchaRqdataCapacity),
                                   char_storage(runtime, kCaptchaRqtokenOffset, kCaptchaRqtokenCapacity),
                                   char_storage(runtime, kCaptchaSessionOffset, kCaptchaSessionCapacity),
                                   char_storage(runtime, kCaptchaFlowOffset, kCaptchaFlowCapacity),
                                   char_storage(runtime, kSuspendedOffset, kSuspendedCapacity)};
}

SecureBuffer partition(UserAuthCoordinatorRuntime* runtime, u32 offset, u32 capacity, u32 bytes) noexcept {
    return SecureBuffer{secret(runtime, offset), capacity, bytes, true};
}

UserAuthCoordinatorPhase coordinator_phase(UserAuthPhase phase) noexcept {
    switch (phase) {
    case UserAuthPhase::reset_pending:
        return UserAuthCoordinatorPhase::reset_pending;
    case UserAuthPhase::idle:
        return UserAuthCoordinatorPhase::idle;
    case UserAuthPhase::password_pending:
        return UserAuthCoordinatorPhase::password_pending;
    case UserAuthPhase::verification_pending:
        return UserAuthCoordinatorPhase::verification_pending;
    case UserAuthPhase::browser_exchange_pending:
        return UserAuthCoordinatorPhase::browser_exchange_pending;
    case UserAuthPhase::recovery_pending: return UserAuthCoordinatorPhase::recovery_pending;
    case UserAuthPhase::recovery_sent: return UserAuthCoordinatorPhase::recovery_sent;
    case UserAuthPhase::recovery_phone_required: return UserAuthCoordinatorPhase::recovery_phone_required;
    case UserAuthPhase::phone_pending: return UserAuthCoordinatorPhase::phone_verify_pending;
    case UserAuthPhase::phone_token_ready: return UserAuthCoordinatorPhase::phone_token_ready;
    case UserAuthPhase::phone_token_taken: return UserAuthCoordinatorPhase::phone_token_taken;
    case UserAuthPhase::mfa_required:
        return UserAuthCoordinatorPhase::mfa_required;
    case UserAuthPhase::mfa_pending:
        return UserAuthCoordinatorPhase::mfa_pending;
    case UserAuthPhase::captcha_required:
        return UserAuthCoordinatorPhase::captcha_required;
    case UserAuthPhase::authenticated:
        return UserAuthCoordinatorPhase::authenticated;
    case UserAuthPhase::rejected:
        return UserAuthCoordinatorPhase::rejected;
    default:
        return UserAuthCoordinatorPhase::failed;
    }
}

UserAuthCoordinatorResult fail(UserAuthCoordinator* coordinator, UserAuthCoordinatorError error,
                               u32 native_error = 0) noexcept {
    UserAuthCoordinatorResult result = coordinator ? coordinator->state : UserAuthCoordinatorResult{};
    result.error = error;
    result.native_error = native_error;
    if (coordinator && error != UserAuthCoordinatorError::invalid_input &&
        error != UserAuthCoordinatorError::state && error != UserAuthCoordinatorError::request_write &&
        error != UserAuthCoordinatorError::transport_submit) {
        result.phase = UserAuthCoordinatorPhase::failed;
        coordinator->state = result;
    }
    return result;
}

u64 claim_request_id(UserAuthCoordinator* coordinator) noexcept {
    const u64 request_id = coordinator->next_request_id == 0 ? 1 : coordinator->next_request_id;
    coordinator->next_request_id = request_id == ~0ull ? 1 : request_id + 1;
    return request_id;
}

bool captcha_retry(const UserAuthState& state, UserAuthPhase return_phase) noexcept {
    return state.phase == UserAuthPhase::captcha_required && state.captcha_return_phase == return_phase;
}

RestClientSubmitResult submit_prepared(UserAuthCoordinator* coordinator,
                                       UserAuthCoordinatorRuntime* runtime, UserAuthState next_state,
                                       UserAuthOperation operation, u32 body_bytes,
                                       const SecureBuffer* captcha_key, u64 request_id, u32 body_offset = 0) noexcept {
    const u32 capacity = runtime->phone_context_active ? kPhoneTokenOffset : runtime->request_capacity;
    if (body_offset > capacity || body_bytes > capacity - body_offset)
        return {RestClientSubmitError::body_capacity};
    SecureBuffer body{runtime->secrets.data + body_offset, runtime->request_capacity - body_offset, body_bytes, true};
    SecureBuffer rqtoken =
        partition(runtime, kCaptchaRqtokenOffset, kCaptchaRqtokenCapacity, runtime->captcha_rqtoken_bytes);
    SecureBuffer session =
        partition(runtime, kCaptchaSessionOffset, kCaptchaSessionCapacity, runtime->captcha_session_bytes);
    const bool retry = captcha_key != nullptr;
    const RestClientSubmitResult submitted = rest_client_submit_user_auth(
        &coordinator->transport,
        RestUserAuthRequest{&body, captcha_key, retry && rqtoken.bytes != 0 ? &rqtoken : nullptr,
                            retry && session.bytes != 0 ? &session : nullptr, request_id,
                            kRestClientDefaultTimeoutMs, operation});
    if (!submitted.ok()) {
        return submitted;
    }
    runtime->retained_request_bytes = body_bytes;
    runtime->active_body_offset = body_offset;
    runtime->active_captcha_revision = runtime->captcha_revision;
    runtime->flow = next_state;
    runtime->active_generation = next_state.generation;
    runtime->active_request_id = request_id;
    runtime->active_operation = operation;
    coordinator->state = UserAuthCoordinatorResult{};
    coordinator->state.generation = next_state.generation;
    coordinator->state.request_id = request_id;
    coordinator->state.phase = coordinator_phase(next_state.phase);
    if (operation == UserAuthOperation::reset_mfa)
        coordinator->state.phase = UserAuthCoordinatorPhase::reset_mfa_pending;
    if (operation == UserAuthOperation::phone_resend)
        coordinator->state.phase = UserAuthCoordinatorPhase::phone_resend_pending;
    if (operation == UserAuthOperation::mfa_sms_send) {
        coordinator->state.phase = UserAuthCoordinatorPhase::sms_send_pending;
    }
    return submitted;
}

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

struct ConsumeContext {
    UserAuthCoordinator* coordinator;
    UserAuthCoordinatorEvent* event;
};

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
    event.decode_error = decode.error;
    coordinator->state.decode_error = decode.error;
    coordinator->state.decode_error_offset = decode.error_offset;
    coordinator->state.scan_error = decode.scan_error;
    coordinator->state.read_error = decode.read_error;
    coordinator->state.response_kind = decoded.kind;
    if (transport.error != RestClientError::none) {
        event.kind = UserAuthCoordinatorEventKind::transport_error;
        coordinator->state.error = UserAuthCoordinatorError::transport;
        coordinator->state.phase = UserAuthCoordinatorPhase::failed;
        clear_request(runtime); clear_retained(runtime); user_auth_state_cancel(&runtime->flow);
        return true;
    }
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
    if (!decode.ok()) {
        event.kind = UserAuthCoordinatorEventKind::protocol_error;
        coordinator->state.error = UserAuthCoordinatorError::decode;
        coordinator->state.phase = UserAuthCoordinatorPhase::failed;
        clear_request(runtime); clear_retained(runtime); user_auth_state_cancel(&runtime->flow);
        return true;
    }
    event.response = decoded;
    runtime->last_response = decoded;
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
    event.kind = UserAuthCoordinatorEventKind::protocol_error;
    coordinator->state.error = UserAuthCoordinatorError::decode;
    coordinator->state.phase = UserAuthCoordinatorPhase::failed;
    clear_request(runtime); clear_retained(runtime); user_auth_state_cancel(&runtime->flow);
    return true;
}

bool consume_phone_response(UserAuthCoordinator* coordinator, UserAuthCoordinatorRuntime* runtime,
    UserAuthCoordinatorEvent& event, const RestClientResponse& transport,
    ConstSlice<u8> locked_body) noexcept {
    const bool verify = runtime->active_operation == UserAuthOperation::phone_verify;
    if (transport.error != RestClientError::none) {
        runtime->active_request_id = 0;
        event.kind = UserAuthCoordinatorEventKind::transport_error;
        coordinator->state.error = UserAuthCoordinatorError::transport;
        coordinator->state.phase = UserAuthCoordinatorPhase::failed;
        clear_request(runtime); clear_retained(runtime); user_auth_state_cancel(&runtime->flow);
        return true;
    }
    UserAuthResponse decoded{};
    auto storage = response_storage(runtime);
    storage.token = {reinterpret_cast<char*>(runtime->secrets.data + kPhoneTokenOffset), kUserAuthPhoneResetTokenBytes};
    const auto decode = user_auth_decode_phone_response(transport.status_code,
        {reinterpret_cast<const char*>(locked_body.data), locked_body.count},
        {runtime->structurals, kStructuralCount}, 16, storage, &decoded, verify);
    event.decode_error = decode.error;
    coordinator->state.decode_error = decode.error;
    coordinator->state.decode_error_offset = decode.error_offset;
    coordinator->state.scan_error = decode.scan_error;
    coordinator->state.read_error = decode.read_error;
    runtime->active_request_id = 0;
    if (!decode.ok() && transport.status_code != 429) {
        event.kind = UserAuthCoordinatorEventKind::protocol_error;
        coordinator->state.error = UserAuthCoordinatorError::decode;
        coordinator->state.phase = UserAuthCoordinatorPhase::failed;
        clear_request(runtime); clear_retained(runtime); user_auth_state_cancel(&runtime->flow);
        return true;
    }
    if (transport.status_code == 429 || decoded.kind == UserAuthResponseKind::rejected) {
        event.kind = UserAuthCoordinatorEventKind::phone_rejected;
        event.response = decoded;
        runtime->last_response = decoded;
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
        event.kind = UserAuthCoordinatorEventKind::protocol_error;
        coordinator->state.error = UserAuthCoordinatorError::state;
        coordinator->state.transition_error = applied.error;
        coordinator->state.phase = UserAuthCoordinatorPhase::failed;
        clear_request(runtime); clear_retained(runtime); user_auth_state_cancel(&runtime->flow);
        return true;
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
        SecureZeroMemory(runtime->secrets.data + kPhoneTokenOffset + decoded.token_bytes,
            kUserAuthPhoneResetTokenBytes - decoded.token_bytes);
        SecureZeroMemory(runtime->secrets.data, kPhoneTokenOffset);
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
    runtime->last_response = decoded;
    event.response = decoded;
    return true;
}

bool consume_response(void* opaque, const RestClientResponse& transport_response,
                      ConstSlice<u8> locked_body) noexcept {
    auto* context = static_cast<ConsumeContext*>(opaque);
    UserAuthCoordinator* coordinator = context->coordinator;
    UserAuthCoordinatorRuntime* runtime = runtime_of(coordinator);
    UserAuthCoordinatorEvent& event = *context->event;
    event = UserAuthCoordinatorEvent{};
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

    if (transport_response.request_id != runtime->active_request_id ||
        transport_response.error != RestClientError::none) {
        event.kind = UserAuthCoordinatorEventKind::transport_error;
        coordinator->state.error = UserAuthCoordinatorError::transport;
        coordinator->state.phase = UserAuthCoordinatorPhase::failed;
        runtime->active_request_id = 0;
        clear_request(runtime);
        if (runtime->active_operation == UserAuthOperation::verification || runtime->active_operation == UserAuthOperation::recovery ||
            runtime->active_operation == UserAuthOperation::browser_handoff_exchange) clear_retained(runtime);
        return true;
    }

    if (runtime->active_operation == UserAuthOperation::mfa_sms_send &&
        transport_response.status_code >= 200 && transport_response.status_code < 300) {
        event.kind = UserAuthCoordinatorEventKind::sms_code_sent;
        coordinator->state = UserAuthCoordinatorResult{};
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
    event.decode_error = decode.error;
    coordinator->state.decode_error = decode.error;
    coordinator->state.decode_error_offset = decode.error_offset;
    coordinator->state.scan_error = decode.scan_error;
    coordinator->state.read_error = decode.read_error;
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
        event.kind = UserAuthCoordinatorEventKind::protocol_error;
        coordinator->state.error = UserAuthCoordinatorError::decode;
        coordinator->state.phase = UserAuthCoordinatorPhase::failed;
        runtime->active_request_id = 0;
        clear_request(runtime);
        if (runtime->active_operation == UserAuthOperation::verification || runtime->active_operation == UserAuthOperation::recovery ||
            runtime->active_operation == UserAuthOperation::browser_handoff_exchange) clear_retained(runtime);
        return true;
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
        runtime->last_response = decoded;
        event.response = decoded;
        event.kind = UserAuthCoordinatorEventKind::rejected;
        coordinator->state.error = UserAuthCoordinatorError::none;
        coordinator->state.response_kind = UserAuthResponseKind::rejected;
        coordinator->state.phase = UserAuthCoordinatorPhase::mfa_required;
        return true;
    }

    const UserAuthPhase prior_phase = runtime->flow.phase;
    if (runtime->active_operation == UserAuthOperation::mfa_sms_send) {
        runtime->last_response = decoded;
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
        event.kind = UserAuthCoordinatorEventKind::protocol_error;
        coordinator->state.error = UserAuthCoordinatorError::state;
        coordinator->state.transition_error = applied.error;
        coordinator->state.response_kind = decoded.kind;
        coordinator->state.phase = UserAuthCoordinatorPhase::failed;
        runtime->active_request_id = 0;
        clear_request(runtime);
        if (runtime->active_operation == UserAuthOperation::verification || runtime->active_operation == UserAuthOperation::recovery ||
            runtime->active_operation == UserAuthOperation::browser_handoff_exchange) clear_retained(runtime);
        return true;
    }
    retain_decoded(runtime, decoded, prior_phase);
    runtime->last_response = decoded;
    event.response = decoded;
    event.kind = event_kind(decoded.kind);
    coordinator->state = UserAuthCoordinatorResult{};
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
            event.kind = UserAuthCoordinatorEventKind::protocol_error;
            coordinator->state.error = UserAuthCoordinatorError::request_write;
            coordinator->state.phase = UserAuthCoordinatorPhase::failed;
            clear_request(runtime); clear_retained(runtime); user_auth_state_cancel(&runtime->flow);
        } else {
            runtime->phone_context_active = true;
            runtime->continuation_base_bytes = base.bytes;
            runtime->continuation_token_bytes = 0;
            restore_phone_base(runtime);
            SecureZeroMemory(runtime->secrets.data + kPhoneTokenOffset, kUserAuthPhoneResetTokenBytes);
        }
        return true;
    }
    if (decoded.kind != UserAuthResponseKind::captcha_required) {
        clear_request(runtime);
    }
    return true;
}

} // namespace

static UserAuthCoordinatorResult start_coordinator(UserAuthCoordinator* coordinator, bool reset) noexcept {
    const u32 bytes = page_round(sizeof(UserAuthCoordinatorRuntime));
    if (!coordinator || bytes == 0 || coordinator->runtime || coordinator->transport.worker.thread) {
        return fail(coordinator, UserAuthCoordinatorError::invalid_input);
    }
    *coordinator = UserAuthCoordinator{};
    coordinator->runtime = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!coordinator->runtime) {
        return fail(coordinator, UserAuthCoordinatorError::memory, thread_last_error());
    }
    UserAuthCoordinatorRuntime* runtime = runtime_of(coordinator);
    runtime->request_capacity = reset ? kUserAuthResetCoordinatorRequestBytes : kUserAuthCoordinatorRequestBytes;
    runtime->reset_owner = reset;
    const SecureBufferResult secret_result =
        secure_buffer_initialize(&runtime->secrets, reset ? kUserAuthResetCoordinatorSecretBytes : kUserAuthCoordinatorSecretBytes);
    if (!secret_result.ok()) {
        const u32 native_error = secret_result.native_error;
        SecureZeroMemory(runtime, bytes);
        VirtualFree(runtime, 0, MEM_RELEASE);
        coordinator->runtime = nullptr;
        return fail(coordinator, UserAuthCoordinatorError::memory, native_error);
    }
    user_auth_state_initialize(&runtime->flow);
    const RestClientResult started =
        reset ? rest_client_start_user_auth_capacity(&coordinator->transport,
            kUserAuthMaximumResponseBytes, runtime->request_capacity) :
        rest_client_start_user_auth(&coordinator->transport, kUserAuthMaximumResponseBytes);
    if (!started.ok()) {
        secure_buffer_release(&runtime->secrets);
        SecureZeroMemory(runtime, bytes);
        VirtualFree(runtime, 0, MEM_RELEASE);
        coordinator->runtime = nullptr;
        return fail(coordinator, UserAuthCoordinatorError::transport_start, started.native_error);
    }
    coordinator->next_request_id = 1;
    coordinator->state.phase = UserAuthCoordinatorPhase::idle;
    return coordinator->state;
}

UserAuthCoordinatorResult user_auth_coordinator_start(UserAuthCoordinator* coordinator) noexcept {
    return start_coordinator(coordinator, false);
}
UserAuthCoordinatorResult user_auth_coordinator_start_reset(UserAuthCoordinator* coordinator) noexcept {
    return start_coordinator(coordinator, true);
}

UserAuthCoordinatorResult user_auth_coordinator_submit_reset(UserAuthCoordinator* coordinator,
    const SecureBuffer* token, const SecureBuffer* password) noexcept {
    auto* runtime = runtime_of(coordinator);
    if (!runtime || !runtime->reset_owner || runtime->transport_stopped || runtime->active_request_id ||
        !locked_input(password, kUserAuthMaximumPasswordBytes))
        return fail(coordinator, UserAuthCoordinatorError::invalid_input);
    if (coordinator->state.phase == UserAuthCoordinatorPhase::failed ||
        coordinator->state.phase == UserAuthCoordinatorPhase::stopping ||
        (runtime->continuation_retry_at_ms && GetTickCount64() < runtime->continuation_retry_at_ms))
        return fail(coordinator, UserAuthCoordinatorError::state);
    const bool retry = token == nullptr;
    if (retry ? !runtime->reset_prefix_retry || !runtime->continuation_token_bytes :
        runtime->reset_context_active || runtime->flow.phase != UserAuthPhase::idle ||
        !locked_input(token, kLoginResetMaximumTokenBytes))
        return fail(coordinator, UserAuthCoordinatorError::invalid_input);
    UserAuthState next = runtime->flow;
    if (!user_auth_state_begin_reset(&next).ok()) return fail(coordinator, UserAuthCoordinatorError::state);
    u32 bytes = 0, prefix = runtime->continuation_token_bytes;
    if (retry) {
        const auto measured = user_auth_measure_reset_password_tail(secure_utf8(*password));
        if (!measured.ok() || measured.bytes > runtime->request_capacity - prefix)
            return fail(coordinator, UserAuthCoordinatorError::request_write);
        const auto written = user_auth_write_reset_password_tail(secure_utf8(*password),
            {runtime->secrets.data + prefix, runtime->request_capacity - prefix});
        if (!written.ok()) return fail(coordinator, UserAuthCoordinatorError::request_write);
        bytes = prefix + written.bytes;
    } else {
        const UserAuthResetPasswordRequest request{secure_utf8(*token), secure_utf8(*password)};
        const auto measured = user_auth_measure_reset_password_request(request);
        if (!measured.ok() || measured.bytes > runtime->request_capacity)
            return fail(coordinator, UserAuthCoordinatorError::request_write);
        const auto written = user_auth_write_reset_password_request(request,
            {secret(runtime, 0), runtime->request_capacity});
        if (!written.ok()) return fail(coordinator, UserAuthCoordinatorError::request_write);
        bytes = written.bytes; prefix = written.token_prefix_bytes;
    }
    const auto submitted = submit_prepared(coordinator, runtime, next, UserAuthOperation::reset_password,
        bytes, nullptr, claim_request_id(coordinator));
    if (!submitted.ok()) {
        if (retry) preserve_reset_prefix(runtime); else clear_request(runtime);
        auto result = fail(coordinator, UserAuthCoordinatorError::transport_submit);
        result.submit_error = submitted.error; return result;
    }
    runtime->continuation_base_bytes = bytes;
    runtime->continuation_token_bytes = prefix;
    runtime->reset_context_active = true;
    runtime->reset_prefix_retry = false;
    runtime->continuation_retry_at_ms = 0;
    clear_retained(runtime);
    return coordinator->state;
}

static UserAuthCoordinatorResult submit_reset_mfa(UserAuthCoordinator* coordinator,
    const UserAuthMfaInput& input) noexcept {
    auto* runtime = runtime_of(coordinator);
    if (!runtime || runtime->transport_stopped || runtime->active_request_id ||
        runtime->flow.phase != UserAuthPhase::mfa_required || !runtime->continuation_base_bytes ||
        !runtime->ticket_bytes || !(runtime->mfa_capabilities & login_mfa_capability(input.type)) ||
        input.captcha_key || !locked_input(input.code, user_auth_mfa_data_maximum(input.type)) ||
        (runtime->continuation_retry_at_ms && GetTickCount64() < runtime->continuation_retry_at_ms))
        return fail(coordinator, UserAuthCoordinatorError::state);
    UserAuthState next = runtime->flow;
    if (!user_auth_state_begin_mfa(&next, input.type).ok()) return fail(coordinator, UserAuthCoordinatorError::state);
    SecureBuffer normalized{};
    const SecureBuffer* code = input.code;
    if (input.type != UserAuthMfaType::webauthn) {
        normalized = partition(runtime, kTokenOffset, kLoginMfaNormalizedCodeBytes, 0);
        if (!login_mfa_prepare_code(input.type, secure_buffer_view(*code), &normalized).ok()) {
            secure_buffer_clear(&normalized); return fail(coordinator, UserAuthCoordinatorError::invalid_input);
        }
        code = &normalized;
    }
    const Utf8View ticket{reinterpret_cast<const char*>(secret(runtime, kTicketOffset)), runtime->ticket_bytes};
    const auto measured = user_auth_measure_reset_mfa_suffix(secure_utf8(*code), ticket, input.type);
    const u32 offset = runtime->continuation_base_bytes - 1;
    if (!measured.ok() || measured.bytes > runtime->request_capacity - offset) {
        if (normalized.data) secure_buffer_clear(&normalized);
        return fail(coordinator, UserAuthCoordinatorError::request_write);
    }
    const auto written = user_auth_write_reset_mfa_suffix(secure_utf8(*code), ticket, input.type,
        {runtime->secrets.data + offset, runtime->request_capacity - offset});
    if (normalized.data) secure_buffer_clear(&normalized);
    if (!written.ok()) return fail(coordinator, UserAuthCoordinatorError::request_write);
    const auto submitted = submit_prepared(coordinator, runtime, next, UserAuthOperation::reset_mfa,
        offset + written.bytes, nullptr, claim_request_id(coordinator));
    if (!submitted.ok()) {
        restore_reset_base(runtime);
        auto result = fail(coordinator, UserAuthCoordinatorError::transport_submit);
        result.submit_error = submitted.error; return result;
    }
    runtime->continuation_retry_at_ms = 0;
    clear_captcha(runtime);
    runtime->active_captcha_revision = runtime->captcha_revision;
    return coordinator->state;
}

UserAuthCoordinatorResult user_auth_coordinator_submit_password(
    UserAuthCoordinator* coordinator, const UserAuthPasswordInput& input) noexcept {
    UserAuthCoordinatorRuntime* runtime = runtime_of(coordinator);
    if (!runtime || runtime->reset_owner || runtime->transport_stopped || !locked_input(input.login, kUserAuthMaximumLoginBytes) ||
        !locked_input(input.password, kUserAuthMaximumPasswordBytes) || runtime->active_request_id != 0) {
        return fail(coordinator, UserAuthCoordinatorError::invalid_input);
    }
    const bool retry = captcha_retry(runtime->flow, UserAuthPhase::password_pending);
    if (retry != (input.captcha_key != nullptr) ||
        (input.captcha_key && !locked_input(input.captcha_key, kUserAuthMaximumCaptchaKeyBytes))) {
        return fail(coordinator, UserAuthCoordinatorError::state);
    }
    UserAuthState next = runtime->flow;
    const UserAuthTransitionResult transition =
        retry ? user_auth_state_resume_captcha(&next) : user_auth_state_begin_password(&next);
    if (!transition.ok()) {
        return fail(coordinator, UserAuthCoordinatorError::state);
    }
    const UserAuthWriteResult written = user_auth_write_password_request(
        UserAuthPasswordRequest{secure_utf8(*input.login), secure_utf8(*input.password),
                                kUserAuthDefaultLoginSource, input.undelete},
        Slice<u8>{secret(runtime, kScratchOffset), runtime->request_capacity});
    if (!written.ok()) {
        SecureZeroMemory(secret(runtime, kScratchOffset), runtime->request_capacity);
        return fail(coordinator, UserAuthCoordinatorError::request_write);
    }
    const u64 request_id = claim_request_id(coordinator);
    const RestClientSubmitResult submitted =
        submit_prepared(coordinator, runtime, next, UserAuthOperation::password, written.bytes,
                        input.captcha_key, request_id);
    if (!submitted.ok()) {
        clear_request(runtime);
        UserAuthCoordinatorResult result = fail(coordinator, UserAuthCoordinatorError::transport_submit);
        result.submit_error = submitted.error;
        return result;
    }
    if (!retry) {
        clear_retained(runtime);
    } else {
        clear_captcha(runtime);
        runtime->active_captcha_revision = runtime->captcha_revision;
    }
    return coordinator->state;
}

UserAuthCoordinatorResult user_auth_coordinator_submit_recovery(
    UserAuthCoordinator* coordinator, const SecureBuffer* login) noexcept {
    auto* runtime = runtime_of(coordinator);
    if (!runtime || runtime->reset_owner || runtime->transport_stopped || runtime->active_request_id ||
        !locked_input(login, kUserAuthMaximumLoginBytes))
        return fail(coordinator, UserAuthCoordinatorError::invalid_input);
    UserAuthState next = runtime->flow;
    if (!user_auth_state_begin_recovery(&next).ok()) return fail(coordinator, UserAuthCoordinatorError::state);
    const auto written = user_auth_write_recovery_request(secure_utf8(*login),
        {secret(runtime, kScratchOffset), runtime->request_capacity});
    if (!written.ok()) { clear_request(runtime); return fail(coordinator, UserAuthCoordinatorError::request_write); }
    const auto submitted = submit_prepared(coordinator, runtime, next, UserAuthOperation::recovery,
        written.bytes, nullptr, claim_request_id(coordinator));
    if (!submitted.ok()) {
        clear_request(runtime);
        auto result = fail(coordinator, UserAuthCoordinatorError::transport_submit);
        result.submit_error = submitted.error;
        return result;
    }
    clear_retained(runtime);
    return coordinator->state;
}

static UserAuthCoordinatorResult submit_phone(UserAuthCoordinator* coordinator, const SecureBuffer* code,
    bool verify) noexcept {
    auto* runtime = runtime_of(coordinator);
    if (!runtime || runtime->reset_owner || runtime->transport_stopped || !runtime->phone_context_active ||
        runtime->active_request_id || runtime->flow.phase != UserAuthPhase::recovery_phone_required ||
        !runtime->continuation_base_bytes || runtime->continuation_base_bytes > kPhoneTokenOffset ||
        runtime->continuation_token_bytes || (runtime->continuation_retry_at_ms && GetTickCount64() < runtime->continuation_retry_at_ms))
        return fail(coordinator, UserAuthCoordinatorError::state);
    if (verify && (!locked_input(code, kUserAuthPhoneCodeBytes) ||
        overlaps_auth_owner(coordinator, runtime, code, sizeof(*code)) ||
        overlaps_auth_owner(coordinator, runtime, code->data, code->capacity) ||
        !user_auth_phone_code_valid(secure_utf8(*code))))
        return fail(coordinator, UserAuthCoordinatorError::invalid_input);
    const u32 base = runtime->continuation_base_bytes;
    u32 bytes = base;
    if (verify) {
        const auto written = user_auth_write_phone_verify_request({runtime->secrets.data, kPhoneTokenOffset}, base, secure_utf8(*code));
        if (!written.ok()) return fail(coordinator, UserAuthCoordinatorError::request_write);
        bytes = written.bytes;
    }
    UserAuthState next = runtime->flow;
    user_auth_state_cancel(&next);
    next.phase = UserAuthPhase::phone_pending;
    const auto submitted = submit_prepared(coordinator, runtime, next,
        verify ? UserAuthOperation::phone_verify : UserAuthOperation::phone_resend, bytes, nullptr, claim_request_id(coordinator));
    if (!submitted.ok()) {
        restore_phone_base(runtime);
        auto result = fail(coordinator, UserAuthCoordinatorError::transport_submit);
        result.submit_error = submitted.error;
        return result;
    }
    runtime->continuation_retry_at_ms = 0;
    clear_retained(runtime);
    return coordinator->state;
}
UserAuthCoordinatorResult user_auth_coordinator_submit_phone_verify(UserAuthCoordinator* coordinator,
    const SecureBuffer* code) noexcept { return submit_phone(coordinator, code, true); }
UserAuthCoordinatorResult user_auth_coordinator_submit_phone_resend(UserAuthCoordinator* coordinator) noexcept {
    return submit_phone(coordinator, nullptr, false);
}
SecureBufferResult user_auth_coordinator_take_phone_reset_token(UserAuthCoordinator* coordinator,
    u64 expected_generation, SecureBuffer* output) noexcept {
    auto* runtime = runtime_of(coordinator);
    if (!runtime || !output || overlaps_auth_owner(coordinator, runtime, output, sizeof(*output)) ||
        !output->data || !output->locked || output->bytes || output->capacity != kUserAuthPhoneResetTokenBytes ||
        overlaps_auth_owner(coordinator, runtime, output->data, output->capacity) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(output), sizeof(*output)}, {output->data, output->capacity}) ||
        !runtime->phone_context_active || runtime->reset_owner || runtime->active_request_id ||
        coordinator->state.phase != UserAuthCoordinatorPhase::phone_token_ready ||
        runtime->flow.phase != UserAuthPhase::phone_token_ready || expected_generation != runtime->flow.generation ||
        !runtime->continuation_token_bytes || runtime->continuation_token_bytes > kUserAuthPhoneResetTokenBytes)
        return {0, ERROR_INVALID_PARAMETER, SecureBufferError::invalid_input};
    const auto result = secure_buffer_assign(output, {runtime->secrets.data + kPhoneTokenOffset, runtime->continuation_token_bytes});
    if (!result.ok()) return result;
    clear_request(runtime); clear_retained(runtime);
    user_auth_state_cancel(&runtime->flow);
    runtime->flow.phase = UserAuthPhase::phone_token_taken;
    coordinator->state.generation = runtime->flow.generation;
    coordinator->state.request_id = 0;
    coordinator->state.phase = UserAuthCoordinatorPhase::phone_token_taken;
    return result;
}

UserAuthCoordinatorResult user_auth_coordinator_submit_verification(
    UserAuthCoordinator* coordinator, const SecureBuffer* link_token) noexcept {
    UserAuthCoordinatorRuntime* runtime = runtime_of(coordinator);
    if (!runtime || runtime->reset_owner || runtime->transport_stopped ||
        !locked_input(link_token, kLoginVerificationMaximumTokenBytes) ||
        runtime->active_request_id != 0) {
        return fail(coordinator, UserAuthCoordinatorError::invalid_input);
    }
    UserAuthState next = runtime->flow;
    const UserAuthTransitionResult transition = user_auth_state_begin_verification(&next);
    if (!transition.ok()) return fail(coordinator, UserAuthCoordinatorError::state);
    const UserAuthWriteResult written = login_verification_write_request(
        secure_utf8(*link_token), {secret(runtime, kScratchOffset), runtime->request_capacity});
    if (!written.ok()) {
        clear_request(runtime);
        return fail(coordinator, UserAuthCoordinatorError::request_write);
    }
    const RestClientSubmitResult submitted = submit_prepared(
        coordinator, runtime, next, UserAuthOperation::verification, written.bytes,
        nullptr, claim_request_id(coordinator));
    if (!submitted.ok()) {
        clear_request(runtime);
        UserAuthCoordinatorResult result = fail(coordinator, UserAuthCoordinatorError::transport_submit);
        result.submit_error = submitted.error;
        return result;
    }
    clear_retained(runtime);
    return coordinator->state;
}

UserAuthCoordinatorResult user_auth_coordinator_submit_browser_handoff_exchange(
    UserAuthCoordinator* coordinator, const SecureBuffer* exchange_request) noexcept {
    auto* runtime = runtime_of(coordinator);
    if (!runtime || runtime->transport_stopped ||
        !locked_input(exchange_request, kBrowserSignInMaximumBodyBytes) ||
        overlaps_auth_owner(coordinator, runtime, exchange_request, sizeof(*exchange_request)) ||
        overlaps_auth_owner(coordinator, runtime, exchange_request->data, exchange_request->capacity))
        return fail(coordinator, UserAuthCoordinatorError::invalid_input);
    if (runtime->reset_owner || runtime->reset_context_active || runtime->phone_context_active ||
        runtime->active_request_id || (runtime->flow.phase != UserAuthPhase::idle &&
            runtime->flow.phase != UserAuthPhase::rejected) ||
        coordinator->state.phase == UserAuthCoordinatorPhase::failed ||
        coordinator->state.phase == UserAuthCoordinatorPhase::stopping)
        return fail(coordinator, UserAuthCoordinatorError::state);
    if (!user_auth_browser_handoff_request_valid(secure_utf8(*exchange_request),
        {runtime->structurals, kStructuralCount}, 16))
        return fail(coordinator, UserAuthCoordinatorError::invalid_input);
    UserAuthState next = runtime->flow;
    if (!user_auth_state_begin_browser_exchange(&next).ok())
        return fail(coordinator, UserAuthCoordinatorError::state);
    copy_bytes(secret(runtime, kScratchOffset), exchange_request->data, exchange_request->bytes);
    const auto submitted = submit_prepared(coordinator, runtime, next,
        UserAuthOperation::browser_handoff_exchange, exchange_request->bytes, nullptr, claim_request_id(coordinator));
    // Enqueueing the request synchronously copies the protected body. Neither
    // success nor failure retains a credential for a later implicit replay.
    clear_request(runtime);
    if (!submitted.ok()) {
        auto result = fail(coordinator, UserAuthCoordinatorError::transport_submit);
        result.submit_error = submitted.error;
        return result;
    }
    clear_retained(runtime);
    return coordinator->state;
}

UserAuthCoordinatorResult user_auth_coordinator_submit_mfa(UserAuthCoordinator* coordinator,
                                                           const UserAuthMfaInput& input) noexcept {
    UserAuthCoordinatorRuntime* runtime = runtime_of(coordinator);
    if (runtime && runtime->reset_context_active) return submit_reset_mfa(coordinator, input);
    const u32 data_maximum = user_auth_mfa_data_maximum(input.type);
    if (!runtime || runtime->transport_stopped || data_maximum == 0 ||
        !locked_input(input.code, data_maximum) || runtime->ticket_bytes == 0 ||
        runtime->login_instance_bytes == 0 || runtime->active_request_id != 0) {
        return fail(coordinator, UserAuthCoordinatorError::invalid_input);
    }
    const bool retry = captcha_retry(runtime->flow, UserAuthPhase::mfa_pending);
    if (retry != (input.captcha_key != nullptr) ||
        (input.captcha_key && !locked_input(input.captcha_key, kUserAuthMaximumCaptchaKeyBytes))) {
        return fail(coordinator, UserAuthCoordinatorError::state);
    }
    UserAuthState next = runtime->flow;
    UserAuthTransitionResult transition{};
    if (retry) {
        if (next.mfa_type != input.type) {
            return fail(coordinator, UserAuthCoordinatorError::state);
        }
        transition = user_auth_state_resume_captcha(&next);
    } else {
        transition = user_auth_state_begin_mfa(&next, input.type);
    }
    if (!transition.ok()) {
        return fail(coordinator, UserAuthCoordinatorError::state);
    }
    SecureBuffer normalized_code{};
    const SecureBuffer* prepared_code = input.code;
    if (input.type != UserAuthMfaType::webauthn) {
        normalized_code = partition(runtime, kTokenOffset, kLoginMfaNormalizedCodeBytes, 0);
        const LoginMfaCodeResult prepared =
            login_mfa_prepare_code(input.type, secure_buffer_view(*input.code), &normalized_code);
        if (!prepared.ok()) {
            secure_buffer_clear(&normalized_code);
            return fail(coordinator, UserAuthCoordinatorError::invalid_input);
        }
        prepared_code = &normalized_code;
    }
    const UserAuthWriteResult written = user_auth_write_mfa_request(
        UserAuthMfaRequest{
            secure_utf8(*prepared_code),
            Utf8View{reinterpret_cast<const char*>(secret(runtime, kTicketOffset)), runtime->ticket_bytes},
            kUserAuthDefaultLoginSource,
            Utf8View{reinterpret_cast<const char*>(secret(runtime, kLoginInstanceOffset)),
                     runtime->login_instance_bytes},
            input.type},
        Slice<u8>{secret(runtime, kScratchOffset), runtime->request_capacity});
    if (normalized_code.data) {
        secure_buffer_clear(&normalized_code);
    }
    if (!written.ok()) {
        SecureZeroMemory(secret(runtime, kScratchOffset), runtime->request_capacity);
        return fail(coordinator, UserAuthCoordinatorError::request_write);
    }
    const UserAuthOperation operation = input.type == UserAuthMfaType::totp  ? UserAuthOperation::mfa_totp
                                        : input.type == UserAuthMfaType::sms ? UserAuthOperation::mfa_sms
                                        : input.type == UserAuthMfaType::backup
                                            ? UserAuthOperation::mfa_backup
                                            : UserAuthOperation::mfa_webauthn;
    const u64 request_id = claim_request_id(coordinator);
    const RestClientSubmitResult submitted = submit_prepared(
        coordinator, runtime, next, operation, written.bytes, input.captcha_key, request_id);
    if (!submitted.ok()) {
        clear_request(runtime);
        UserAuthCoordinatorResult result = fail(coordinator, UserAuthCoordinatorError::transport_submit);
        result.submit_error = submitted.error;
        return result;
    }
    if (retry) {
        clear_captcha(runtime);
        runtime->active_captcha_revision = runtime->captcha_revision;
    }
    return coordinator->state;
}

UserAuthCoordinatorResult user_auth_coordinator_submit_sms_send(UserAuthCoordinator* coordinator) noexcept {
    auto* runtime = runtime_of(coordinator);
    if (!runtime || runtime->transport_stopped || runtime->flow.phase != UserAuthPhase::mfa_required ||
        !runtime->ticket_bytes || runtime->active_request_id ||
        (runtime->reset_context_active && (!runtime->continuation_base_bytes ||
            !(runtime->mfa_capabilities & user_auth_mfa_capability_sms))) ||
        (runtime->continuation_retry_at_ms && GetTickCount64() < runtime->continuation_retry_at_ms))
        return fail(coordinator, UserAuthCoordinatorError::state);
    const u32 offset = runtime->reset_context_active ? runtime->continuation_base_bytes : 0;
    const auto written = user_auth_write_mfa_sms_send_request(
        {{reinterpret_cast<const char*>(secret(runtime, kTicketOffset)), runtime->ticket_bytes}},
        {runtime->secrets.data + offset, runtime->request_capacity - offset});
    if (!written.ok()) return fail(coordinator, UserAuthCoordinatorError::request_write);
    const auto submitted = submit_prepared(coordinator, runtime, runtime->flow,
        UserAuthOperation::mfa_sms_send, written.bytes, nullptr, claim_request_id(coordinator), offset);
    if (!submitted.ok()) {
        if (runtime->reset_context_active) restore_reset_base(runtime); else clear_request(runtime);
        auto result = fail(coordinator, UserAuthCoordinatorError::transport_submit);
        result.submit_error = submitted.error; return result;
    }
    runtime->continuation_retry_at_ms = 0;
    return coordinator->state;
}

UserAuthCoordinatorResult user_auth_coordinator_submit_captcha(UserAuthCoordinator* coordinator,
                                                               const SecureBuffer* solution) noexcept {
    UserAuthCoordinatorRuntime* runtime = runtime_of(coordinator);
    if (!runtime || runtime->transport_stopped || runtime->active_request_id != 0 ||
        runtime->flow.phase != UserAuthPhase::captcha_required || runtime->retained_request_bytes == 0 ||
        runtime->active_operation == UserAuthOperation::browser_handoff_exchange ||
        runtime->active_body_offset > runtime->request_capacity ||
        runtime->retained_request_bytes > runtime->request_capacity - runtime->active_body_offset ||
        !locked_input(solution, kUserAuthMaximumCaptchaKeyBytes) ||
        (runtime->phone_context_active && (runtime->active_body_offset != 0 ||
            runtime->retained_request_bytes > kPhoneTokenOffset ||
            overlaps_auth_owner(coordinator, runtime, solution, sizeof(*solution)) ||
            overlaps_auth_owner(coordinator, runtime, solution->data, solution->capacity)))) {
        return fail(coordinator, UserAuthCoordinatorError::state);
    }
    UserAuthState next = runtime->flow;
    const UserAuthTransitionResult resumed = user_auth_state_resume_captcha(&next);
    if (!resumed.ok()) {
        return fail(coordinator, UserAuthCoordinatorError::state);
    }
    const u64 request_id = claim_request_id(coordinator);
    const RestClientSubmitResult submitted =
        submit_prepared(coordinator, runtime, next, runtime->active_operation,
                        runtime->retained_request_bytes, solution, request_id, runtime->active_body_offset);
    if (!submitted.ok()) {
        UserAuthCoordinatorResult result = fail(coordinator, UserAuthCoordinatorError::transport_submit);
        result.submit_error = submitted.error;
        return result;
    }
    clear_captcha(runtime);
    runtime->active_captcha_revision = runtime->captcha_revision;
    return coordinator->state;
}

UserAuthCoordinatorResult user_auth_coordinator_pump(UserAuthCoordinator* coordinator,
                                                     UserAuthCoordinatorEvent* event) noexcept {
    UserAuthCoordinatorRuntime* runtime = runtime_of(coordinator);
    if (!runtime || !event || runtime->transport_stopped) {
        return fail(coordinator, UserAuthCoordinatorError::invalid_input);
    }
    *event = UserAuthCoordinatorEvent{};
    if (coordinator->state.phase == UserAuthCoordinatorPhase::stopping ||
        coordinator->state.phase == UserAuthCoordinatorPhase::failed) return coordinator->state;
    const RestClientResult transport = rest_client_status(&coordinator->transport);
    if (transport.phase != RestClientPhase::response_ready) {
        // A fatal worker error is recorded before shutdown changes its phase
        // to stopping. MFA/CAPTCHA can still own secrets without an active ID.
        const bool waiting = runtime->flow.phase == UserAuthPhase::captcha_required ||
            runtime->flow.phase == UserAuthPhase::mfa_required || runtime->phone_context_active ||
            runtime->flow.phase == UserAuthPhase::phone_token_taken;
        if ((transport.phase == RestClientPhase::failed || transport.error != RestClientError::none) &&
            runtime->flow.phase != UserAuthPhase::authenticated &&
            (runtime->active_request_id != 0 || waiting)) {
            event->request_id = runtime->active_request_id;
            event->generation = runtime->active_generation;
            event->transport_error = transport.error;
            event->kind = UserAuthCoordinatorEventKind::transport_error;
            coordinator->state.error = UserAuthCoordinatorError::transport;
            coordinator->state.native_error = transport.native_error;
            coordinator->state.transport_error = transport.error;
            coordinator->state.phase = UserAuthCoordinatorPhase::failed;
            runtime->active_request_id = 0;
            clear_request(runtime);
            user_auth_state_cancel(&runtime->flow);
            clear_retained(runtime);
        }
        return coordinator->state;
    }
    ConsumeContext context{coordinator, event};
    RestClientResponse response{};
    const RestClientTakeResult consumed = rest_client_consume_user_auth_response(
        &coordinator->transport, &response, consume_response, &context);
    if (!consumed.ok()) {
        if (runtime->phone_context_active || runtime->flow.phase == UserAuthPhase::phone_token_taken) {
            runtime->active_request_id = 0;
            clear_request(runtime); clear_retained(runtime); user_auth_state_cancel(&runtime->flow);
        }
        return fail(coordinator, UserAuthCoordinatorError::transport);
    }
    return coordinator->state;
}

SecureBufferResult user_auth_coordinator_copy_secret(const UserAuthCoordinator* coordinator,
                                                     UserAuthCoordinatorSecretKind kind,
                                                     SecureBuffer* output) noexcept {
    const UserAuthCoordinatorRuntime* runtime = runtime_of(coordinator);
    u32 offset = 0;
    u32 bytes = 0;
    if (!runtime || !output) {
        return SecureBufferResult{0, ERROR_INVALID_PARAMETER, SecureBufferError::invalid_input};
    }
    switch (kind) {
    case UserAuthCoordinatorSecretKind::webauthn_challenge:
        offset = kWebauthnOffset;
        bytes = runtime->webauthn_bytes;
        break;
    case UserAuthCoordinatorSecretKind::captcha_service:
        offset = kCaptchaServiceOffset;
        bytes = runtime->captcha_service_bytes;
        break;
    case UserAuthCoordinatorSecretKind::captcha_sitekey:
        offset = kCaptchaSitekeyOffset;
        bytes = runtime->captcha_sitekey_bytes;
        break;
    case UserAuthCoordinatorSecretKind::captcha_rqdata:
        offset = kCaptchaRqdataOffset;
        bytes = runtime->captcha_rqdata_bytes;
        break;
    case UserAuthCoordinatorSecretKind::captcha_rqtoken:
        offset = kCaptchaRqtokenOffset;
        bytes = runtime->captcha_rqtoken_bytes;
        break;
    case UserAuthCoordinatorSecretKind::captcha_session_id:
        offset = kCaptchaSessionOffset;
        bytes = runtime->captcha_session_bytes;
        break;
    case UserAuthCoordinatorSecretKind::captcha_user_flow:
        offset = kCaptchaFlowOffset;
        bytes = runtime->captcha_flow_bytes;
        break;
    case UserAuthCoordinatorSecretKind::suspended_user_token:
        offset = kSuspendedOffset;
        bytes = runtime->suspended_bytes;
        break;
    default:
        return SecureBufferResult{0, ERROR_INVALID_PARAMETER, SecureBufferError::invalid_input};
    }
    if (bytes == 0) {
        return SecureBufferResult{0, ERROR_NOT_FOUND, SecureBufferError::invalid_input};
    }
    return secure_buffer_assign(output, ConstSlice<u8>{secret(runtime, offset), bytes});
}

CaptchaChallengeSnapshot user_auth_coordinator_captcha_snapshot(
    const UserAuthCoordinator* coordinator) noexcept {
    const UserAuthCoordinatorRuntime* runtime = runtime_of(coordinator);
    if (!runtime || !runtime->secrets.data || !runtime->secrets.locked ||
        runtime->flow.phase != UserAuthPhase::captcha_required ||
        coordinator->state.phase != UserAuthCoordinatorPhase::captcha_required) {
        return {};
    }
    return CaptchaChallengeSnapshot{
        runtime->captcha_service_bytes, runtime->captcha_sitekey_bytes,
        runtime->captcha_rqdata_bytes, runtime->captcha_rqtoken_bytes,
        runtime->captcha_session_bytes, runtime->captcha_flow_bytes,
        captcha_provider_from_service({secret(runtime, kCaptchaServiceOffset), runtime->captcha_service_bytes}),
        runtime->captcha_invisible, true};
}

UserAuthCoordinatorResult user_auth_coordinator_take_profile(UserAuthCoordinator* coordinator,
                                                             u64 profile_id, AccountProfile* profile,
                                                             u32 stop_timeout_ms) noexcept {
    UserAuthCoordinatorRuntime* runtime = runtime_of(coordinator);
    if (!runtime || !profile || profile->credential.data || profile_id == 0 ||
        runtime->flow.phase != UserAuthPhase::authenticated || runtime->token_bytes == 0) {
        return fail(coordinator, UserAuthCoordinatorError::invalid_input);
    }
    if (!runtime->transport_stopped) {
        const RestClientResult stopped =
            rest_client_stop(&coordinator->transport, stop_timeout_ms);
        if (coordinator->transport.runtime || coordinator->transport.worker.thread) {
            coordinator->state.error = UserAuthCoordinatorError::stop;
            coordinator->state.native_error = stopped.native_error;
            coordinator->state.phase = UserAuthCoordinatorPhase::stopping;
            return coordinator->state;
        }
        runtime->transport_stopped = true;
    }
    SecureBuffer token{};
    const SecureBufferResult initialized =
        secure_buffer_initialize(&token, kAccountProfileMaximumCredentialBytes);
    if (!initialized.ok()) {
        return fail(coordinator, UserAuthCoordinatorError::memory, initialized.native_error);
    }
    const SecureBufferResult assigned =
        secure_buffer_assign(&token, ConstSlice<u8>{secret(runtime, kTokenOffset), runtime->token_bytes});
    if (!assigned.ok()) {
        secure_buffer_release(&token);
        return fail(coordinator, UserAuthCoordinatorError::profile, assigned.native_error);
    }
    profile->credential = token;
    token = SecureBuffer{};
    profile->metadata = AccountProfileMetadata{profile_id};
    clear_secret(runtime, kTokenOffset, kTokenCapacity);
    runtime->token_bytes = 0;
    coordinator->state.error = UserAuthCoordinatorError::none;
    coordinator->state.phase = UserAuthCoordinatorPhase::session_promoted;
    return coordinator->state;
}

HANDLE user_auth_coordinator_notification_handle(const UserAuthCoordinator* coordinator) noexcept {
    const UserAuthCoordinatorRuntime* runtime = runtime_of(coordinator);
    return runtime && !runtime->transport_stopped && coordinator->state.phase != UserAuthCoordinatorPhase::failed &&
        coordinator->state.phase != UserAuthCoordinatorPhase::stopping
               ? rest_client_notification_handle(&coordinator->transport)
               : nullptr;
}

UserAuthCoordinatorResult user_auth_coordinator_status(const UserAuthCoordinator* coordinator) noexcept {
    if (coordinator) {
        return coordinator->state;
    }
    UserAuthCoordinatorResult invalid{};
    invalid.native_error = ERROR_INVALID_PARAMETER;
    invalid.error = UserAuthCoordinatorError::invalid_input;
    invalid.phase = UserAuthCoordinatorPhase::empty;
    return invalid;
}

UserAuthCoordinatorResult user_auth_coordinator_stop(UserAuthCoordinator* coordinator,
                                                     u32 timeout_ms) noexcept {
    UserAuthCoordinatorRuntime* runtime = runtime_of(coordinator);
    if (!runtime) {
        return fail(coordinator, UserAuthCoordinatorError::invalid_input);
    }
    // Keep the exact admitted request receipt across a failed join. Stop never
    // consumes authentication data, even when a successful response races it.
    const u64 stopping_request_id =
        runtime->active_request_id ? runtime->active_request_id : coordinator->state.request_id;
    coordinator->state.phase = UserAuthCoordinatorPhase::stopping;
    user_auth_state_cancel(&runtime->flow);
    runtime->active_request_id = 0;
    ++runtime->captcha_revision;
    clear_request(runtime);
    clear_retained(runtime);
    if (!runtime->transport_stopped) {
        RestClientResponse receipt{};
        const RestClientResult stopped =
            rest_client_stop_user_auth(&coordinator->transport, timeout_ms, &receipt);
        if (stopping_request_id && receipt.request_id == stopping_request_id && receipt.transport_ok() &&
            receipt.status_code == 429) {
            coordinator->state.request_id = stopping_request_id;
            coordinator->state.http_status = 429;
            coordinator->state.retry_at_ms = maximum(coordinator->state.retry_at_ms, receipt.retry_at_ms);
        }
        // stop can report a prior worker error after joining and releasing.
        // Keep ownership only while a transport allocation or thread remains.
        if (coordinator->transport.runtime || coordinator->transport.worker.thread) {
            coordinator->state.error = UserAuthCoordinatorError::stop;
            coordinator->state.native_error = stopped.native_error;
            coordinator->state.phase = UserAuthCoordinatorPhase::stopping;
            return coordinator->state;
        }
        runtime->transport_stopped = true;
    }
    const u32 bytes = page_round(sizeof(UserAuthCoordinatorRuntime));
    secure_buffer_release(&runtime->secrets);
    SecureZeroMemory(runtime, bytes);
    VirtualFree(runtime, 0, MEM_RELEASE);
    UserAuthCoordinatorResult stopped{};
    stopped.phase = UserAuthCoordinatorPhase::stopped;
    if (coordinator->state.http_status == 429) {
        stopped.request_id = coordinator->state.request_id;
        stopped.http_status = 429;
        stopped.retry_at_ms = coordinator->state.retry_at_ms;
    }
    *coordinator = UserAuthCoordinator{};
    coordinator->state = stopped;
    return stopped;
}

u32 user_auth_coordinator_runtime_bytes() noexcept {
    const u32 owner = page_round(sizeof(UserAuthCoordinatorRuntime));
    const u32 transport = rest_client_user_auth_runtime_bytes(kUserAuthMaximumResponseBytes);
    const u64 total = static_cast<u64>(owner) + kUserAuthCoordinatorSecretBytes + transport;
    return owner != 0 && transport != 0 && total <= ~0u ? static_cast<u32>(total) : 0;
}

} // namespace maicord
