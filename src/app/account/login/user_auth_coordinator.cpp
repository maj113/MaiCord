#include "user_auth_coordinator_internal.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/protocol/auth/login_continuations.hpp"
#include "maicord/protocol/auth/browser_sign_in.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/scratch_pages.hpp"

namespace maicord {
namespace {

static_assert(kPhoneTokenOffset + kUserAuthPhoneResetTokenBytes == kScratchCapacity);

static_assert(kSuspendedOffset + kSuspendedCapacity == kUserAuthCoordinatorSecretBytes);

u32 page_round(u64 bytes) noexcept {
    const u64 rounded = (bytes + 4095ull) & ~4095ull;
    return rounded <= ~0u ? static_cast<u32>(rounded) : 0;
}

} // namespace
namespace user_auth_coordinator_detail {

UserAuthCoordinatorRuntime* runtime_of(UserAuthCoordinator* coordinator) noexcept {
    return coordinator ? static_cast<UserAuthCoordinatorRuntime*>(coordinator->runtime) : nullptr;
}

const UserAuthCoordinatorRuntime* runtime_of(const UserAuthCoordinator* coordinator) noexcept {
    return coordinator ? static_cast<const UserAuthCoordinatorRuntime*>(coordinator->runtime) : nullptr;
}

} // namespace user_auth_coordinator_detail
namespace {

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

} // namespace
namespace user_auth_coordinator_detail {

void clear_secret(UserAuthCoordinatorRuntime* runtime, u32 offset, u32 capacity) noexcept {
    secure_clear_bytes(secret(runtime, offset), capacity);
}

void clear_retained(UserAuthCoordinatorRuntime* runtime) noexcept {
    secure_clear_bytes(secret(runtime, kTokenOffset), kUserAuthCoordinatorSecretBytes - kTokenOffset);
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
}

void clear_request(UserAuthCoordinatorRuntime* runtime) noexcept {
    secure_clear_bytes(secret(runtime, kScratchOffset), runtime->request_capacity);
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
    secure_clear_bytes(runtime->secrets.data + end, kPhoneTokenOffset - end);
    runtime->secrets.data[end] = '}';
    runtime->retained_request_bytes = 0;
    runtime->active_body_offset = 0;
}

} // namespace user_auth_coordinator_detail
namespace {

bool overlaps_auth_owner(const UserAuthCoordinator* coordinator, const UserAuthCoordinatorRuntime* runtime,
    const void* data, u32 bytes) noexcept {
    const ConstSlice<u8> span{static_cast<const u8*>(data), bytes};
    return !byte_range_valid(span) || byte_ranges_overlap(span, {runtime->secrets.data, runtime->secrets.capacity}) ||
        byte_ranges_overlap(span, {reinterpret_cast<const u8*>(runtime), sizeof(*runtime)}) ||
        byte_ranges_overlap(span, {reinterpret_cast<const u8*>(coordinator), sizeof(*coordinator)});
}

} // namespace
namespace user_auth_coordinator_detail {

void restore_reset_base(UserAuthCoordinatorRuntime* runtime) noexcept {
    if (runtime->continuation_base_bytes) {
        const u32 end = runtime->continuation_base_bytes - 1;
        secure_clear_bytes(runtime->secrets.data + end, runtime->request_capacity - end);
        runtime->secrets.data[end] = '}';
    }
    runtime->active_body_offset = 0;
    runtime->retained_request_bytes = 0;
}

void preserve_reset_prefix(UserAuthCoordinatorRuntime* runtime) noexcept {
    secure_clear_bytes(runtime->secrets.data + runtime->continuation_token_bytes,
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
        secure_clear_bytes(secret(runtime, offset) + bytes, capacity - bytes);
    }
}

} // namespace user_auth_coordinator_detail
namespace {

Slice<char> char_storage(UserAuthCoordinatorRuntime* runtime, u32 offset, u32 capacity) noexcept {
    return Slice<char>{reinterpret_cast<char*>(secret(runtime, offset)), capacity};
}

} // namespace
namespace user_auth_coordinator_detail {

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

} // namespace user_auth_coordinator_detail
namespace {

SecureBuffer partition(UserAuthCoordinatorRuntime* runtime, u32 offset, u32 capacity, u32 bytes) noexcept {
    return SecureBuffer{secret(runtime, offset), capacity, bytes, true};
}

} // namespace
namespace user_auth_coordinator_detail {

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

} // namespace user_auth_coordinator_detail
namespace {

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

UserAuthCoordinatorResult submit_failed(UserAuthCoordinator* coordinator, RestClientSubmitError error) noexcept {
    UserAuthCoordinatorResult result = fail(coordinator, UserAuthCoordinatorError::transport_submit);
    result.submit_error = error;
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
    reset_value<UserAuthCoordinatorResult>(&coordinator->state);
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

} // namespace

static UserAuthCoordinatorResult start_coordinator(UserAuthCoordinator* coordinator, bool reset) noexcept {
    const u32 bytes = page_round(sizeof(UserAuthCoordinatorRuntime));
    if (!coordinator || bytes == 0 || coordinator->runtime || coordinator->transport.worker.thread) {
        return fail(coordinator, UserAuthCoordinatorError::invalid_input);
    }
    reset_value<UserAuthCoordinator>(coordinator);
    coordinator->runtime = commit_pages(bytes);
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
        secure_clear_bytes(runtime, bytes);
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
        secure_clear_bytes(runtime, bytes);
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
        return submit_failed(coordinator, submitted.error);
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
        return submit_failed(coordinator, submitted.error);
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
        UserAuthPasswordRequest{secure_utf8(*input.login), secure_utf8(*input.password), input.undelete},
        Slice<u8>{secret(runtime, kScratchOffset), runtime->request_capacity});
    if (!written.ok()) {
        secure_clear_bytes(secret(runtime, kScratchOffset), runtime->request_capacity);
        return fail(coordinator, UserAuthCoordinatorError::request_write);
    }
    const u64 request_id = claim_request_id(coordinator);
    const RestClientSubmitResult submitted =
        submit_prepared(coordinator, runtime, next, UserAuthOperation::password, written.bytes,
                        input.captcha_key, request_id);
    if (!submitted.ok()) {
        clear_request(runtime);
        return submit_failed(coordinator, submitted.error);
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
        return submit_failed(coordinator, submitted.error);
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
        return submit_failed(coordinator, submitted.error);
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
        return submit_failed(coordinator, submitted.error);
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
        return submit_failed(coordinator, submitted.error);
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
            Utf8View{reinterpret_cast<const char*>(secret(runtime, kLoginInstanceOffset)),
                     runtime->login_instance_bytes},
            input.type},
        Slice<u8>{secret(runtime, kScratchOffset), runtime->request_capacity});
    if (normalized_code.data) {
        secure_buffer_clear(&normalized_code);
    }
    if (!written.ok()) {
        secure_clear_bytes(secret(runtime, kScratchOffset), runtime->request_capacity);
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
        return submit_failed(coordinator, submitted.error);
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
        return submit_failed(coordinator, submitted.error);
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
        return submit_failed(coordinator, submitted.error);
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
    reset_value<UserAuthCoordinatorEvent>(event);
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
    reset_value<SecureBuffer>(&token);
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
    secure_clear_bytes(runtime, bytes);
    VirtualFree(runtime, 0, MEM_RELEASE);
    UserAuthCoordinatorResult stopped{};
    stopped.phase = UserAuthCoordinatorPhase::stopped;
    if (coordinator->state.http_status == 429) {
        stopped.request_id = coordinator->state.request_id;
        stopped.http_status = 429;
        stopped.retry_at_ms = coordinator->state.retry_at_ms;
    }
    reset_value<UserAuthCoordinator>(coordinator);
    coordinator->state = stopped;
    return stopped;
}

} // namespace maicord
