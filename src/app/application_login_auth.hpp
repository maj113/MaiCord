#pragma once

#include "maicord/core/base.hpp"
#include "maicord/protocol/user_auth.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {
enum class LoginRemoteStatus : u8;
}

namespace maicord::app_detail {

void application_login_reset_backup_acknowledgement() noexcept;
void application_login_schedule_remote_restart(LoginRemoteStatus status) noexcept;
bool application_login_tick_backup_acknowledgement(u64 now_ms) noexcept;
bool application_login_auth_cooldown_active() noexcept;
void application_login_retain_auth_retry() noexcept;
u64 application_login_auth_deadline() noexcept;
bool application_login_tick_auth_cooldown(u64 now_ms) noexcept;
u64 application_login_recovery_deadline() noexcept;
bool application_login_tick_recovery(u64 now_ms) noexcept;
bool application_login_begin_recovery() noexcept;
bool application_login_submit_recovery() noexcept;
bool application_login_submit_phone_verify() noexcept;
bool application_login_submit_phone_resend() noexcept;
bool application_login_run_phone_verify(u64 posted_epoch, u32 posted_revision) noexcept;
u64 application_login_phone_deadline() noexcept;
bool application_login_tick_phone(u64 now_ms) noexcept;
bool application_login_begin_reset() noexcept;
bool application_login_submit_reset() noexcept;
u64 application_login_reset_deadline() noexcept;
bool application_login_tick_reset(u64 now_ms) noexcept;
bool application_login_select_mfa(UserAuthMfaType type, bool allow_resend) noexcept;
bool application_login_run_webauthn(u64 posted_epoch = 0) noexcept;
bool application_login_submit_mfa() noexcept;
bool application_login_pump() noexcept;
bool application_login_begin_remote_auth() noexcept;
bool application_login_cancel_blocked() noexcept;
bool application_login_handle_click(LPARAM position) noexcept;
bool application_login_handle_key(WPARAM key, bool repeated = false) noexcept;

} // namespace maicord::app_detail
