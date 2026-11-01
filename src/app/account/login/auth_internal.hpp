#pragma once
// Shared by auth.cpp and its parts (auth_*.cpp); private to them.
#include "maicord/core/win32_process.hpp"
#include "../../shell/log_fields.hpp"
#include "auth.hpp"
#include "browser_auth.hpp"
#include "captcha_auth.hpp"

#include "../application_account_module.hpp"
#include "../application_account_logout.hpp"
#include "input.hpp"
#include "ui.hpp"
#include "geometry.hpp"
#include "../../shell/state.hpp"
#include "../../shell/overlay_input.hpp"

#include "maicord/app/account/account_paths.hpp"
#include "maicord/app/account/login/mfa_policy.hpp"
#include "maicord/app/account/login/continuations.hpp"
#include "maicord/protocol/auth/login_continuations.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/platform/desktop/webauthn.hpp"
#include "maicord/protocol/auth/remote_auth.hpp"
#include "maicord/protocol/auth/webauthn_mfa.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/app/account/login/user_auth_coordinator.hpp"
#include "maicord/app/account/login/remote_auth_handshake.hpp"

namespace maicord::app_detail {
namespace auth_detail {}
using namespace auth_detail;
namespace auth_detail {

void retain_auth_retry(const AccountLoginStatus& status) noexcept;
AccountLoginStatus stop_login_owner() noexcept;
void invalidate_auth() noexcept;
bool present(LoginStatus status) noexcept;
bool present(LoginStatus status, LoginControl focus) noexcept;
bool present_focus(LoginControl focus) noexcept;
bool present_mfa_retry(LoginStatus code_status) noexcept;
bool request_throttled() noexcept;
bool phone_request_blocked() noexcept;
bool reset_failed() noexcept;

struct VerificationInput {
    SecureBuffer token;
    ~VerificationInput() noexcept { secure_buffer_release(&token); }
};

bool quiesce_recovery_owner() noexcept;
bool adopt_reset_token(SecureBuffer* token) noexcept;
void restore_identifier_bound() noexcept;

} // namespace auth_detail
} // namespace maicord::app_detail
