// The linked account module's start and login owners: a missing profile starts nothing, and a login owner creates
// no worker or transport before its first submit. Tests that fake the module link tests/support/account_module_fake.cpp.
#include "../../support/test_output.hpp"

#include "maicord/app/account/account_host.hpp"
#include "maicord/platform/crypto/secure_buffer.hpp"

namespace maicord {
namespace {

u32 failures;

void expect(bool success, const char* message) noexcept {
    test_check(&failures, success, message);
}

void test_account_module_contract() noexcept {
    const AccountModuleStartResult missing = account_module_start(
        L"Z:\\MaiCord-definitely-missing.profile", L"Z:\\MaiCord-definitely-missing.session", 1);
    expect(!missing.ok() && missing.instance == nullptr && missing.error == AccountModuleError::profile &&
               missing.native_error != 0,
           "account_module_missing_profile_is_dormant");
    void* login_instance = nullptr;
    const AccountLoginStatus login_started =
        account_module_login_start(&login_instance, L"Z:\\MaiCord-login-contract.profile");
    expect(login_started.ok() && login_started.phase == AccountLoginPhase::idle && login_instance &&
               account_module_login_notification_handle(login_instance) == nullptr &&
               account_module_login_stop(&login_instance, 5'000).phase == AccountLoginPhase::stopped &&
               !login_instance,
           "account_module_login_lifecycle_contract");
    void* reset_instance = nullptr;
    const AccountLoginStatus reset_started =
        account_module_login_start_reset(&reset_instance, L"Z:\\MaiCord-reset-contract.profile");
    const bool reset_dormant = reset_started.ok() && reset_started.phase == AccountLoginPhase::idle &&
        reset_instance && !account_module_login_notification_handle(reset_instance);
    SecureBuffer empty_locked{};
    expect(reset_dormant &&
               account_module_login_submit_password(reset_instance, &empty_locked, &empty_locked).error ==
                   AccountLoginError::invalid_input &&
               account_module_login_submit_verification(reset_instance, &empty_locked).error ==
                   AccountLoginError::invalid_input &&
               account_module_login_submit_recovery(reset_instance, &empty_locked).error ==
                   AccountLoginError::invalid_input &&
               account_module_login_start_remote_auth(reset_instance).error == AccountLoginError::invalid_input &&
               !account_module_login_notification_handle(reset_instance),
           "account_module_reset_owner_is_lazy_and_rejects_unrelated_operations_without_worker");
    expect(reset_instance && account_module_login_stop(&reset_instance, 5'000).phase == AccountLoginPhase::stopped &&
               !reset_instance,
           "account_module_reset_dormant_owner_stops_without_credential_or_network");
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_account_module_entry() noexcept {
    maicord::test_account_module_contract();
    maicord::test_exit(maicord::failures, "Account module");
}
