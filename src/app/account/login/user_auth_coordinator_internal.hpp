#pragma once
// Shared by user_auth_coordinator.cpp and its parts (user_auth_coordinator_*.cpp); private to them.
#include "maicord/app/account/login/user_auth_coordinator.hpp"

#include "maicord/app/account/login/mfa_policy.hpp"

namespace maicord {
namespace user_auth_coordinator_detail {}
using namespace user_auth_coordinator_detail;
namespace user_auth_coordinator_detail {

inline constexpr u32 kStructuralCount = 16 * 1024;
inline constexpr u32 kScratchOffset = 0;
inline constexpr u32 kScratchCapacity = kUserAuthCoordinatorRequestBytes;
inline constexpr u32 kPhoneTokenOffset = 28 * 1024;

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

struct UserAuthCoordinatorRuntime {
    SecureBuffer secrets;
    UserAuthState flow;
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

UserAuthCoordinatorRuntime* runtime_of(UserAuthCoordinator* coordinator) noexcept;
const UserAuthCoordinatorRuntime* runtime_of(const UserAuthCoordinator* coordinator) noexcept;
void clear_secret(UserAuthCoordinatorRuntime* runtime, u32 offset, u32 capacity) noexcept;
void clear_retained(UserAuthCoordinatorRuntime* runtime) noexcept;
void clear_request(UserAuthCoordinatorRuntime* runtime) noexcept;
void restore_phone_base(UserAuthCoordinatorRuntime* runtime) noexcept;
void restore_reset_base(UserAuthCoordinatorRuntime* runtime) noexcept;
void preserve_reset_prefix(UserAuthCoordinatorRuntime* runtime) noexcept;
void clear_mfa(UserAuthCoordinatorRuntime* runtime) noexcept;
void clear_captcha(UserAuthCoordinatorRuntime* runtime) noexcept;
void clear_suspended(UserAuthCoordinatorRuntime* runtime) noexcept;
void clear_tail(UserAuthCoordinatorRuntime* runtime, u32 offset, u32 bytes, u32 capacity) noexcept;
UserAuthResponseStorage response_storage(UserAuthCoordinatorRuntime* runtime) noexcept;
UserAuthCoordinatorPhase coordinator_phase(UserAuthPhase phase) noexcept;

struct ConsumeContext {
    UserAuthCoordinator* coordinator;
    UserAuthCoordinatorEvent* event;
};

bool consume_response(void* opaque, const RestClientResponse& transport_response,
                      ConstSlice<u8> locked_body) noexcept;

} // namespace user_auth_coordinator_detail
} // namespace maicord
