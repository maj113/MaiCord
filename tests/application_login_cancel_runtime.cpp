#include "application_state.hpp"
#include "application_login_auth.hpp"
#include "application_browser_auth.hpp"
#include "application_login_ui.hpp"
#include "application_login_input.hpp"
#include "application_account_module.hpp"
#include "application_account_logout.hpp"
#include "application_scoped_read.hpp"
#include "application_scoped_read_menu.hpp"
#include "maicord/platform/webauthn.hpp"
#include "maicord/app/account_paths.hpp"
#include "maicord/app/login_continuations.hpp"
#include "maicord/app/login_mfa_policy.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/login_continuations.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/platform/browser_sign_in_launcher.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

// Dedicated native controller harness. UI/module-loading boundaries are fake;
// application_login_auth, account_host and SecureEdit are production code.
namespace maicord::login_cancel_test {
using namespace app_detail;
struct Audit {
    SecureBuffer challenge;
    HANDLE event;
    u32 stops, pumps, challenge_copies, notifications, refreshes, releases, attachments, provider_calls;
    bool fail_stop, fail_release, cleared_before_stop, secrets_cleared_before_stop;
    bool allow_load;
    u32 recovery_submits;
    AccountLoginStatus recovery_response;
    u32 reset_submits, mfa_submits, ceremony_scenario;
    bool reset_null_retry;
    u32 phone_verify_submits, phone_resend_submits, phone_takes, phone_token_bytes, malformed_take;
    bool fail_take, joined_error, attach_success;
    u32 browser_starts, browser_copies, browser_launches;
    bool fail_browser_launch, browser_cancel_on_launch;
    AccountLoginStatus browser_response;
    bool scripted_mfa;
    bool logout_blocked, logout_host_owned, logout_on_browser_copy;
    u32 login_starts, module_loads, path_resolves, password_submits, remote_starts;
    u16 stop_http_status;
    u64 stop_retry_at_ms;
} audit{};
u32 failures = 0;
void check(bool condition, const char* label) noexcept {
    test_check(&failures, condition, label);
}
bool zeroed(const SecureBuffer& buffer) noexcept {
    if (buffer.bytes) return false;
    for (u32 i = 0; i < buffer.capacity; ++i) if (buffer.data[i]) return false;
    return true;
}
bool editors_clear() noexcept {
    return zeroed(g_app.login.login.storage) && zeroed(g_app.login.password.storage) &&
        zeroed(g_app.login.mfa.storage) && !g_app.login.login.anchor && !g_app.login.login.caret &&
        !g_app.login.password.anchor && !g_app.login.password.caret &&
        !g_app.login.mfa.anchor && !g_app.login.mfa.caret;
}
bool secrets_clear() noexcept {
    return zeroed(g_app.login.password.storage) && zeroed(g_app.login.mfa.storage) &&
        !g_app.login.password.anchor && !g_app.login.password.caret && !g_app.login.mfa.anchor && !g_app.login.mfa.caret;
}
AccountLoginStatus WINAPI stop(void** instance, u32 timeout) noexcept {
    ++audit.stops;
    audit.cleared_before_stop = editors_clear();
    audit.secrets_cleared_before_stop = secrets_clear();
    AccountLoginStatus result{};
    if (!instance || *instance != &audit || !timeout || audit.fail_stop) {
        result.error = AccountLoginError::stop;
        result.phase = AccountLoginPhase::failed;
        return result;
    }
    secure_buffer_clear(&audit.challenge);
    *instance = nullptr;
    result.phase = AccountLoginPhase::stopped;
    result.http_status = audit.stop_http_status;
    result.retry_at_ms = audit.stop_retry_at_ms;
    if (audit.joined_error) result.error = AccountLoginError::coordinator;
    return result;
}
AccountLoginStatus WINAPI pump(void*) noexcept {
    ++audit.pumps;
    if (audit.browser_starts) return audit.browser_response;
    if (audit.scripted_mfa || audit.recovery_submits || audit.reset_submits || audit.phone_verify_submits || audit.phone_resend_submits)
        return audit.recovery_response;
    AccountLoginStatus result{};
    result.phase = AccountLoginPhase::authenticated;
    result.profile_saved = true; // Queued success must never be adopted after Cancel.
    return result;
}
AccountLoginStatus WINAPI start(void** instance, const wchar_t*) noexcept {
    ++audit.login_starts;
    *instance = &audit;
    AccountLoginStatus result{}; result.phase = AccountLoginPhase::idle;
    return result;
}
AccountLoginStatus WINAPI password(void* instance, const SecureBuffer* identifier,
                                   const SecureBuffer* secret) noexcept {
    AccountLoginStatus result{};
    if (instance != &audit || !identifier || !identifier->locked || !identifier->bytes || !secret ||
        !secret->locked || !secret->bytes) {
        result.error = AccountLoginError::invalid_input;
        return result;
    }
    ++audit.password_submits;
    result.phase = AccountLoginPhase::password_pending;
    return result;
}
AccountLoginStatus WINAPI remote_start(void* instance) noexcept {
    AccountLoginStatus result{};
    if (instance != &audit) {
        result.error = AccountLoginError::invalid_input;
        return result;
    }
    ++audit.remote_starts;
    result.phase = AccountLoginPhase::remote_starting; return result;
}
AccountLoginStatus WINAPI recovery(void* instance, const SecureBuffer* identifier) noexcept {
    AccountLoginStatus result{};
    if (instance != &audit || !identifier || !identifier->locked) {
        result.error = AccountLoginError::invalid_input; return result;
    }
    const auto written = user_auth_write_recovery_request(
        {reinterpret_cast<const char*>(identifier->data), identifier->bytes},
        {audit.challenge.data, audit.challenge.capacity});
    if (!written.ok()) { result.error = AccountLoginError::invalid_input; return result; }
    audit.challenge.bytes = written.bytes;
    ++audit.recovery_submits;
    result.phase = AccountLoginPhase::recovery_pending; result.request_id = audit.recovery_submits;
    result.generation = audit.recovery_submits;
    audit.recovery_response = result;
    return result;
}
HANDLE WINAPI notification(void*) noexcept { ++audit.notifications; return audit.event; }
AccountLoginStatus WINAPI browser_start(void* instance, u64 generation, u32 timeout) noexcept {
    AccountLoginStatus result{};
    if (instance != &audit || !generation || timeout != 120000 || !editors_clear()) {
        result.error = AccountLoginError::invalid_input; return result;
    }
    ++audit.browser_starts;
    result.phase = AccountLoginPhase::browser_starting; result.browser_generation = generation;
    result.browser_deadline_ms = clock_monotonic_milliseconds() + timeout;
    audit.browser_response = result;
    return result;
}
SecureBufferResult WINAPI browser_copy(void*, u64 generation, SecureBuffer* output) noexcept {
    ++audit.browser_copies;
    if (audit.logout_on_browser_copy) audit.logout_blocked = true;
    if (generation != audit.browser_response.browser_generation || audit.browser_response.phase != AccountLoginPhase::browser_waiting)
        return {0, 0, SecureBufferError::invalid_input};
    constexpr auto url = utf8_literal(
        "https://discord.com/"
        "login?redirect_to=%2Fhandoff%3Frpc%3D6463%26key%3D00000000-0000-4000-8000-000000000000");
    return secure_buffer_assign(output, {reinterpret_cast<const u8*>(url.data), url.bytes});
}
AccountLoginStatus WINAPI phone_verify(void* instance, const SecureBuffer* code) noexcept {
    AccountLoginStatus result{};
    if (instance != &audit || !code || !code->locked || !login_phone_code_valid(
        {reinterpret_cast<const char*>(code->data), code->bytes})) {
        result.error = AccountLoginError::invalid_input; return result;
    }
    ++audit.phone_verify_submits;
    result.phase = AccountLoginPhase::phone_verify_pending; result.generation = 700;
    result.request_id = audit.phone_verify_submits; audit.recovery_response = result;
    return result;
}
AccountLoginStatus WINAPI phone_resend(void* instance) noexcept {
    AccountLoginStatus result{};
    if (instance != &audit) { result.error = AccountLoginError::invalid_input; return result; }
    ++audit.phone_resend_submits;
    result.phase = AccountLoginPhase::phone_resend_pending; result.generation = 700;
    result.request_id = audit.phone_resend_submits; audit.recovery_response = result;
    return result;
}
SecureBufferResult WINAPI phone_take(void* instance, u64 generation, SecureBuffer* output) noexcept {
    ++audit.phone_takes;
    if (instance != &audit || generation != 700 || audit.fail_take || !output || !output->locked || output->bytes ||
        output->capacity < kLoginResetMaximumTokenBytes || audit.phone_token_bytes > audit.challenge.bytes)
        return {0, 0, SecureBufferError::invalid_input};
    const auto copied = secure_buffer_assign(output, {audit.challenge.data, audit.phone_token_bytes});
    if (copied.ok()) {
        secure_buffer_clear(&audit.challenge);
        if (audit.malformed_take == 1) output->bytes = 0;
        if (audit.malformed_take == 2) output->bytes = output->capacity + 1;
    }
    return copied;
}
AccountLoginStatus WINAPI reset(void* instance, const SecureBuffer* token, const SecureBuffer* password) noexcept {
    AccountLoginStatus result{};
    if (instance != &audit || !password || !password->locked || (!token && !audit.reset_submits)) {
        result.error = AccountLoginError::invalid_input; return result;
    }
    if (token) {
        const auto written = user_auth_write_reset_password_request(
            {{reinterpret_cast<const char*>(token->data), token->bytes},
             {reinterpret_cast<const char*>(password->data), password->bytes}},
            {audit.challenge.data, audit.challenge.capacity});
        if (!written.ok()) { result.error = AccountLoginError::invalid_input; return result; }
        audit.challenge.bytes = written.bytes;
    } else audit.reset_null_retry = true;
    ++audit.reset_submits;
    result.phase = AccountLoginPhase::reset_pending; result.request_id = audit.reset_submits;
    result.generation = audit.reset_submits; audit.recovery_response = result;
    return result;
}
AccountLoginStatus WINAPI mfa(void* instance, const SecureBuffer* credential, UserAuthMfaType) noexcept {
    AccountLoginStatus result{};
    if (instance != &audit || !credential || !credential->locked || !credential->bytes) {
        result.error = AccountLoginError::invalid_input; return result;
    }
    ++audit.mfa_submits;
    result.phase = g_app.login.reset_mode ? AccountLoginPhase::reset_mfa_pending : AccountLoginPhase::mfa_pending;
    return result;
}
SecureBufferResult WINAPI copy_challenge(void*, SecureBuffer* output) noexcept {
    ++audit.challenge_copies;
    if (audit.ceremony_scenario) {
        constexpr auto json = utf8_literal(R"({"publicKey":{"challenge":"AQIDBA","rpId":"discord.com","allowCredentials":[],"userVerification":"preferred"}})");
        return secure_buffer_assign(output, {reinterpret_cast<const u8*>(json.data), json.bytes});
    }
    return {0, 0, SecureBufferError::invalid_input};
}
bool seed(bool remote) noexcept {
    audit.stops = audit.pumps = audit.challenge_copies = audit.notifications = 0;
    audit.refreshes = audit.releases = audit.attachments = audit.provider_calls = 0;
    audit.fail_stop = audit.fail_release = audit.cleared_before_stop = audit.secrets_cleared_before_stop = false;
    audit.allow_load = false; audit.recovery_submits = 0; audit.recovery_response = {};
    audit.reset_submits = audit.mfa_submits = audit.ceremony_scenario = 0; audit.reset_null_retry = false;
    audit.phone_verify_submits = audit.phone_resend_submits = audit.phone_takes = audit.phone_token_bytes = audit.malformed_take = 0;
    audit.fail_take = audit.joined_error = audit.attach_success = false;
    audit.browser_starts = audit.browser_copies = audit.browser_launches = 0;
    audit.fail_browser_launch = audit.browser_cancel_on_launch = false; audit.browser_response = {};
    audit.scripted_mfa = false;
    audit.logout_blocked = audit.logout_host_owned = audit.logout_on_browser_copy = false;
    audit.login_starts = audit.module_loads = audit.path_resolves = 0;
    audit.password_submits = audit.remote_starts = 0;
    audit.stop_http_status = 0;
    audit.stop_retry_at_ms = 0;
    g_app.login.auth_retry_at_ms = 0;
    g_app.login.auth_retry_time_unknown = false;
    g_app.login.browser_launch_epoch = 0;
    g_app.login.browser_return_to_qr = false;
    g_app.account = {};
    g_app.account.module = reinterpret_cast<HMODULE>(1); // Never passed to an OS loader.
    g_app.account.login_instance = &audit;
    g_app.account.api.login_stop = stop;
    g_app.account.api.login_start = start;
    g_app.account.api.login_submit_password = password;
    g_app.account.api.login_start_remote_auth = remote_start;
    g_app.account.api.login_start_reset = start;
    g_app.account.api.login_start_browser = browser_start;
    g_app.account.api.login_copy_browser_url = browser_copy;
    g_app.account.api.login_submit_reset = reset;
    g_app.account.api.login_submit_mfa = mfa;
    g_app.account.api.login_submit_recovery = recovery;
    g_app.account.api.login_submit_phone_verify = phone_verify;
    g_app.account.api.login_submit_phone_resend = phone_resend;
    g_app.account.api.login_take_phone_reset_token = phone_take;
    g_app.account.api.login_pump = pump;
    g_app.account.api.login_notification_handle = notification;
    g_app.account.api.login_copy_webauthn_challenge = copy_challenge;
    g_app.login.visible = g_app.login.initialized = true;
    g_app.login.reset_mode = g_app.login.reset_retry = false; g_app.login.webauthn_post_epoch = 0;
    g_app.login.status = remote ? LoginStatus::idle : LoginStatus::captcha_required;
    g_app.login.remote_status = remote ? LoginRemoteStatus::captcha_required : LoginRemoteStatus::idle;
    g_app.login.remote_active = remote;
    g_app.login.focus = LoginControl::back;
    g_app.login.pointer_selection = LoginControl::login;
    g_app.login.qr_ready = remote;
    g_app.login.remote_user[0] = 'x'; g_app.login.remote_user_bytes = 1;
    g_app.login.remote_restart_at_ms = 100;
    g_app.login.remote_retry_attempt = 3;
    g_app.login.sms_requested = g_app.login.sms_sending = g_app.login.sms_failed = true;
    g_app.login.mfa_capabilities = login_mfa_capability_all;
    g_app.login.mfa_type = UserAuthMfaType::webauthn;
    g_app.login.webauthn_attempted = true;
    g_app.login.backup_acknowledged = true;
    g_app.login.backup_acknowledgement_seconds = 7;
    g_app.login.backup_acknowledgement_at_ms = 1;
    secure_edit_clear(&g_app.login.login);
    secure_edit_clear(&g_app.login.password);
    secure_edit_clear(&g_app.login.mfa);
    constexpr char synthetic[] = "synthetic-test-only";
    const Utf8View value{synthetic, sizeof(synthetic) - 1};
    return secure_edit_insert_utf8(&g_app.login.login, value) == SecureEditResult::changed &&
        secure_edit_insert_utf8(&g_app.login.password, value) == SecureEditResult::changed &&
        secure_edit_insert_utf8(&g_app.login.mfa, value) == SecureEditResult::changed &&
        secure_buffer_assign(&audit.challenge,
            {reinterpret_cast<const u8*>(synthetic), sizeof(synthetic) - 1}).ok();
}
bool restored(bool browser_qr_restart = false) noexcept {
    return editors_clear() && g_app.login.status == LoginStatus::idle &&
        g_app.login.focus == LoginControl::login && !g_app.account.module && !g_app.account.login_instance &&
        !g_app.login.remote_active && !g_app.login.qr_ready &&
        (browser_qr_restart ? g_app.login.remote_restart_at_ms != 0 : !g_app.login.remote_restart_at_ms) &&
        (browser_qr_restart ? g_app.login.remote_retry_attempt == 1 : !g_app.login.remote_retry_attempt) && !g_app.login.remote_user_bytes &&
        g_app.login.remote_status == (browser_qr_restart ? LoginRemoteStatus::refreshing : LoginRemoteStatus::idle) && !g_app.login.sms_requested &&
        !g_app.login.sms_sending && !g_app.login.sms_failed && !g_app.login.mfa_capabilities &&
        !g_app.login.webauthn_attempted && !g_app.login.backup_acknowledged &&
        !g_app.login.backup_acknowledgement_seconds && !g_app.login.backup_acknowledgement_at_ms &&
        !g_app.login.reset_mode && !g_app.login.reset_retry &&
        g_app.login.pointer_selection == LoginControl::none;
}
// Leaving a CAPTCHA keeps only the typed identifier and focuses the password.
bool restored_kept() noexcept {
    constexpr u32 identifier = sizeof("synthetic-test-only") - 1;
    const bool kept = secrets_clear() && g_app.login.login.storage.bytes == identifier &&
        g_app.login.focus == LoginControl::password;
    secure_edit_clear(&g_app.login.login);
    g_app.login.focus = LoginControl::login;
    return kept && restored();
}
void run_phone() noexcept {
    check(seed(false), "phone native: initial retained owner seeds");
    g_app.login.status = LoginStatus::recovery_phone_required; g_app.login.focus = LoginControl::phone_code;
    secure_edit_clear(&g_app.login.login); secure_edit_clear(&g_app.login.password); secure_edit_clear(&g_app.login.mfa);
    g_app.login.mfa.password = false;
    check(application_login_handle_key(VK_RETURN) && g_app.login.status == LoginStatus::recovery_phone_input_invalid &&
        !audit.phone_verify_submits && !audit.recovery_submits && !audit.reset_submits,
        "phone native: short code Enter never falls through recovery/password/reset submission");
    check(secure_edit_insert_utf8(&g_app.login.mfa, utf8_literal("123456")) == SecureEditResult::changed,
        "phone native: six digits occupy existing locked MFA editor");
    check(secure_edit_set_codepoint(&g_app.login.mfa, 2, false) == SecureEditResult::changed &&
        application_login_handle_key(VK_DELETE) && g_app.login.mfa.storage.bytes == 6 &&
        g_app.login.mfa.storage.data[2] == ' ' && g_app.login.mfa.storage.data[3] == '4' &&
        application_login_handle_key(VK_RETURN) && !audit.phone_verify_submits,
          "phone native: positional Delete preserves later cells and Enter rejects a six-byte document "
          "with a hole");
    check(secure_edit_set_codepoint(&g_app.login.mfa, 3, false) != SecureEditResult::invalid_input &&
        application_login_handle_key(VK_BACK) && g_app.login.mfa.storage.data[3] == ' ' &&
        g_app.login.mfa.storage.data[4] == '5', "phone native: occupied-cell Backspace clears that cell without shifting later digits");
    secure_edit_clear(&g_app.login.mfa);
    check(secure_edit_insert_utf8(&g_app.login.mfa, utf8_literal("123456")) == SecureEditResult::changed,
        "phone native: complete wire code restores after sparse key admission checks");
    const u64 epoch = g_app.login.application_auth_epoch;
    const u32 revision = g_app.login.mfa.revision;
    check(application_login_run_phone_verify(epoch + 1, revision) && application_login_run_phone_verify(epoch, revision + 1) &&
        !audit.phone_verify_submits && g_app.login.mfa.storage.bytes == 6,
        "phone native: stale TSF epoch/revision callbacks leave code and requests untouched");
    check(application_login_run_phone_verify(epoch, revision) && audit.phone_verify_submits == 1 &&
        g_app.login.status == LoginStatus::recovery_phone_verify_pending && zeroed(g_app.login.mfa.storage) &&
        !audit.recovery_submits && !audit.reset_submits && !audit.attachments,
        "phone native: admitted completion submits only verify and erases app code before pending");
    check(application_login_run_phone_verify(epoch, revision) && application_login_submit_phone_verify() &&
        audit.phone_verify_submits == 1, "phone native: duplicate posted completion cannot resubmit pending code");
    audit.recovery_response.phase = AccountLoginPhase::phone_rejected;
    audit.recovery_response.event = UserAuthCoordinatorEventKind::phone_rejected;
    check(application_login_pump() && g_app.login.status == LoginStatus::recovery_phone_code_rejected &&
        g_app.login.focus == LoginControl::phone_code && !audit.attachments,
        "phone native: rejected code returns empty focused cells without profile attachment");
    g_app.login.focus = LoginControl::phone_resend;
    check(application_login_handle_key(VK_SPACE) && audit.phone_resend_submits == 1 && g_app.login.sms_sending &&
        g_app.login.status == LoginStatus::recovery_phone_resend_pending,
        "phone native: explicit Resend Space uses its dedicated operation");
    audit.recovery_response.phase = AccountLoginPhase::phone_rate_limited;
    audit.recovery_response.http_status = 429;
    audit.recovery_response.event = UserAuthCoordinatorEventKind::phone_rejected;
    audit.recovery_response.retry_at_ms = clock_monotonic_milliseconds() + 21'600'000;
    check(application_login_pump() && g_app.login.status == LoginStatus::recovery_phone_resend_rate_limited,
        "phone native: resend throttle retains original operation kind");
    const u64 due = application_login_phone_deadline();
    check(due && application_login_tick_phone(due - 1) && audit.phone_resend_submits == 1 &&
        g_app.login.status == LoginStatus::recovery_phone_resend_rate_limited,
        "phone native: authoritative cooldown does not send or restore editing early");
    g_app.account.stop_failed = true;
    check(!application_login_phone_deadline(), "phone native: quarantined owner schedules no cooldown wake");
    g_app.account.stop_failed = false;
    check(application_login_tick_auth_cooldown(due) && application_login_tick_phone(due) && !application_login_phone_deadline() &&
        g_app.login.status == LoginStatus::recovery_phone_required && audit.phone_resend_submits == 1,
        "phone native: exact expiry restores editing once without automatic resend");
    check(application_login_submit_phone_resend() && audit.phone_resend_submits == 2,
        "phone native: only a later explicit action resends after expiry");
    audit.recovery_response.phase = AccountLoginPhase::recovery_phone_required;
    audit.recovery_response.event = UserAuthCoordinatorEventKind::phone_code_sent;
    audit.recovery_response.retry_at_ms = 0;
    check(application_login_pump() && g_app.login.status == LoginStatus::recovery_phone_resent &&
        !g_app.login.sms_sending && !audit.attachments, "phone native: resent feedback remains code entry, never authenticated");
    check(application_login_cancel_blocked() && restored(), "phone native: editable Back destroys original owner");

    const u32 token_lengths[]{513, kLoginResetMaximumTokenBytes};
    for (u32 length : token_lengths) {
        check(seed(false), "phone reset handoff: owned fixture seeds");
        g_app.login.status = LoginStatus::recovery_phone_verify_pending;
        secure_edit_clear(&g_app.login.login); secure_edit_clear(&g_app.login.password); secure_edit_clear(&g_app.login.mfa);
        audit.phone_verify_submits = 1; audit.phone_token_bytes = length;
        for (u32 i = 0; i < length; ++i) audit.challenge.data[i] = 'x';
        audit.challenge.bytes = length;
        audit.recovery_response = {}; audit.recovery_response.generation = 700;
        audit.recovery_response.phase = AccountLoginPhase::phone_token_ready;
        // Token-ready remains consumable after its one-shot event was observed.
        audit.joined_error = length == 513;
        check(application_login_pump() && audit.phone_takes == 1 && audit.stops == 1 && audit.releases == 1 &&
            !g_app.account.login_instance && !g_app.account.module && g_app.login.reset_mode &&
            g_app.login.status == LoginStatus::reset_editing && g_app.login.focus == LoginControl::password &&
            g_app.login.login.password && g_app.login.login.storage.locked && g_app.login.login.storage.bytes == length &&
            g_app.login.login.storage.capacity == kLoginResetMaximumTokenBytes && !audit.attachments &&
            zeroed(audit.challenge),
              "phone reset handoff: 513/4096 protected token transfers only after actual owner/module "
              "release");
        bool exact = true;
        for (u32 i = 0; i < length; ++i) exact = exact && g_app.login.login.storage.data[i] == 'x';
        for (u32 i = length; i < g_app.login.login.storage.capacity; ++i) exact = exact && !g_app.login.login.storage.data[i];
        check(exact, "phone reset handoff: exact token and erased destination tail survive ownership transfer");
        auto* full_token_page = g_app.login.login.storage.data;
        Win32MemoryInformation token_page{};
        check(VirtualQuery(full_token_page, token_page.native(), sizeof(token_page)) == sizeof(token_page) &&
            token_page.State == MEM_COMMIT && token_page.RegionSize == kLoginResetMaximumTokenBytes,
            "phone reset handoff: actual token owner commits only the original single-page footprint");
        check(application_login_cancel_blocked() && restored() &&
            g_app.login.login.storage.capacity == kUserAuthMaximumLoginBytes,
            "phone reset handoff: Back wipes full token range before restoring ordinary512 admission");
        bool full_page_erased = full_token_page == g_app.login.login.storage.data;
        for (u32 i = 0; i < kLoginResetMaximumTokenBytes; ++i) full_page_erased = full_page_erased && !full_token_page[i];
        check(full_page_erased, "phone reset handoff: former suffix512..4095 is erased after logical bound shrinks");
    }
    check(seed(false), "phone reset completion: owned fixture seeds");
    g_app.login.status = LoginStatus::recovery_phone_verify_pending;
    secure_edit_clear(&g_app.login.login); secure_edit_clear(&g_app.login.password); secure_edit_clear(&g_app.login.mfa);
    audit.phone_verify_submits = 1; audit.phone_token_bytes = 513;
    for (u32 i = 0; i < 513; ++i) audit.challenge.data[i] = 'x'; audit.challenge.bytes = 513;
    audit.recovery_response = {}; audit.recovery_response.generation = 700;
    audit.recovery_response.phase = AccountLoginPhase::phone_token_ready;
    check(application_login_pump() && g_app.login.reset_mode, "phone reset completion: phone adopts protected reset edit");
    audit.allow_load = true;
    check(secure_edit_insert_utf8(&g_app.login.password, utf8_literal("synthetic-password")) == SecureEditResult::changed &&
        application_login_submit_reset() && audit.reset_submits == 1 && !audit.attachments,
        "phone reset completion: explicit password starts the separate reset owner after phone release");
    audit.recovery_response = {}; audit.recovery_response.phase = AccountLoginPhase::authenticated;
    audit.recovery_response.event = UserAuthCoordinatorEventKind::authenticated; audit.recovery_response.profile_saved = true;
    audit.attach_success = true;
    check(application_login_pump() && audit.attachments == 1 && g_app.account.instance && !g_app.login.reset_mode &&
        !g_app.login.login.password && g_app.login.login.storage.capacity == kUserAuthMaximumLoginBytes && editors_clear(),
          "phone reset completion: actual session admission restores ordinary512 unmasked identifier "
          "before returning to login");
    g_app.account.instance = nullptr; g_app.login.visible = true; g_app.login.status = LoginStatus::session_expired;
    check(secure_edit_insert_utf8(&g_app.login.login, utf8_literal("example@example.com")) == SecureEditResult::changed &&
        !g_app.login.login.password && !login_verification_link_candidate(
            {reinterpret_cast<const char*>(g_app.login.login.storage.data), g_app.login.login.storage.bytes}),
        "phone reset completion: reused editor after session expiry accepts an ordinary identifier");
    for (u32 scenario = 0; scenario < 5; ++scenario) {
        check(seed(false), "phone reset failure: owned fixture seeds");
        g_app.login.status = LoginStatus::recovery_phone_verify_pending;
        secure_edit_clear(&g_app.login.login); secure_edit_clear(&g_app.login.password); secure_edit_clear(&g_app.login.mfa);
        audit.phone_verify_submits = 1; audit.phone_token_bytes = 513;
        for (u32 i = 0; i < 513; ++i) audit.challenge.data[i] = 'x'; audit.challenge.bytes = 513;
        audit.recovery_response = {}; audit.recovery_response.generation = 700;
        audit.recovery_response.phase = AccountLoginPhase::phone_token_ready;
        audit.fail_take = scenario == 0; audit.fail_stop = scenario == 1; audit.fail_release = scenario == 2;
        audit.malformed_take = scenario >= 3 ? scenario - 2 : 0;
        check(application_login_pump() && !g_app.login.reset_mode && editors_clear() && !audit.attachments &&
            (g_app.login.status == LoginStatus::cancelling || g_app.login.status == LoginStatus::recovery_phone_failed),
              "phone reset failure: take/join/module/empty/oversized result cannot publish token or reset "
              "owner");
        audit.fail_stop = audit.fail_release = false;
        check(application_login_cancel_blocked() && restored(), "phone reset failure: explicit Back retries retained cleanup safely");
    }
}

void run_mfa_retry() noexcept {
    constexpr UserAuthMfaType methods[]{UserAuthMfaType::totp, UserAuthMfaType::sms, UserAuthMfaType::backup};
    constexpr Utf8View answers[]{utf8_literal("123456"), utf8_literal("654321"), utf8_literal("abcdefgh")};
    for (u32 i = 0; i < 3; ++i) {
        check(seed(false), "ordinary MFA controller: retained owner fixture seeds");
        g_app.login.status = LoginStatus::submitting; g_app.login.focus = LoginControl::none;
        g_app.login.mfa_type = methods[i]; g_app.login.sms_sending = false;
        g_app.login.remote_restart_at_ms = 0;
        audit.scripted_mfa = true;
        audit.recovery_response = {};
        audit.recovery_response.phase = AccountLoginPhase::mfa_required;
        audit.recovery_response.event = UserAuthCoordinatorEventKind::rejected;
        audit.recovery_response.error_code = kLoginMfaInvalidCode;
        audit.recovery_response.mfa_capabilities = login_mfa_capability_all;
        check(application_login_pump() && g_app.login.status == LoginStatus::mfa_code_rejected &&
            g_app.login.focus == LoginControl::password && zeroed(g_app.login.mfa.storage) &&
            g_app.account.login_instance == &audit && g_app.login.mfa_capabilities == login_mfa_capability_all &&
            !audit.stops && !audit.releases && !g_app.login.remote_restart_at_ms,
              "ordinary MFA controller: reusable invalid answer keeps sole owner and methods while "
              "clearing attempted editor");
        check(secure_edit_insert_utf8(&g_app.login.mfa, answers[i]) == SecureEditResult::changed &&
            application_login_handle_key(VK_RETURN) && audit.mfa_submits == 1 &&
            g_app.login.status == LoginStatus::submitting && zeroed(g_app.login.mfa.storage) &&
            !audit.stops && !audit.recovery_submits && !audit.reset_submits,
              "ordinary MFA controller: Enter after invalid code submits a new MFA answer without password "
              "reset recovery or stop");
        // An unrelated terminal error with the same numeric diagnostic cannot
        // leave the retry editor active once the native owner is discarded.
        audit.recovery_response.phase = AccountLoginPhase::rejected;
        check(application_login_pump() && g_app.login.status == LoginStatus::rejected &&
            !g_app.account.login_instance && !g_app.login.mfa_capabilities && audit.stops == 1,
            "ordinary MFA controller: terminal phase discards owner and never presents stale MFA retry");
    }
    check(seed(false), "ordinary WebAuthn controller: reusable rejection fixture seeds");
    audit.scripted_mfa = true; g_app.login.sms_sending = false; g_app.login.status = LoginStatus::submitting;
    audit.recovery_response = {}; audit.recovery_response.phase = AccountLoginPhase::mfa_required;
    audit.recovery_response.event = UserAuthCoordinatorEventKind::rejected;
    audit.recovery_response.error_code = kLoginMfaInvalidCode;
    check(application_login_pump() && g_app.login.status == LoginStatus::webauthn_failed &&
        !g_app.login.webauthn_attempted && g_app.login.focus == LoginControl::submit &&
        zeroed(g_app.login.mfa.storage) && g_app.account.login_instance == &audit && !audit.stops,
          "ordinary WebAuthn controller: reusable invalid assertion enables a fresh ceremony while "
          "retaining scoped challenge");
    check(application_login_select_mfa(UserAuthMfaType::totp, false) &&
        g_app.login.status == LoginStatus::mfa_required && g_app.login.mfa_type == UserAuthMfaType::totp &&
        !audit.stops && !audit.provider_calls,
          "ordinary WebAuthn controller: invalid assertion can switch to advertised code method without "
          "restarting password");
}
void run_auth_cooldown() noexcept {
    check(seed(false), "auth cooldown: actual password rejection fixture seeds");
    audit.allow_load = audit.scripted_mfa = true;
    g_app.login.status = LoginStatus::submitting;
    g_app.login.remote_status = LoginRemoteStatus::idle;
    audit.recovery_response = {};
    audit.recovery_response.phase = AccountLoginPhase::rejected;
    audit.recovery_response.event = UserAuthCoordinatorEventKind::rejected;
    audit.recovery_response.http_status = 429;
    const u64 due = clock_monotonic_milliseconds() + 21'600'000;
    audit.recovery_response.retry_at_ms = due;
    check(application_login_pump() && !g_app.account.login_instance && audit.stops == 1 &&
              application_login_auth_deadline() == due && g_app.login.remote_restart_at_ms >= due,
          "auth cooldown: six-hour server floor survives terminal owner stop and bounds QR restart");
    const u32 password_bytes = g_app.login.password.storage.bytes;
    g_app.login.focus = LoginControl::password;
    bool blocked = true;
    for (u32 i = 0; i < 4; ++i) {
        blocked = blocked && application_login_handle_key(VK_RETURN) &&
                  application_login_begin_remote_auth() && application_login_begin_browser_auth() &&
                  application_login_submit_mfa() &&
                  application_login_select_mfa(UserAuthMfaType::sms, true) &&
                  application_login_tick_auth_cooldown(due - 1);
    }
    check(blocked && !audit.password_submits && !audit.remote_starts && !audit.browser_starts &&
              !audit.mfa_submits && !audit.provider_calls && !audit.module_loads && !audit.path_resolves &&
              !audit.login_starts && g_app.login.password.storage.bytes == password_bytes &&
              application_login_auth_deadline() == due,
          "auth cooldown: repeated password MFA browser and QR attempts preserve secrets and start no owner "
          "before deadline");
    check(application_login_handle_key(VK_ESCAPE) && editors_clear() &&
              g_app.login.status == LoginStatus::idle && !g_app.account.module &&
              application_login_auth_deadline() == due,
          "auth cooldown: Escape retires secrets and owner without shortening the server floor");
    check(secure_edit_insert_utf8(&g_app.login.login, utf8_literal("retry@example.com")) ==
                  SecureEditResult::changed &&
              secure_edit_insert_utf8(&g_app.login.password, utf8_literal("fresh-test-password")) ==
                  SecureEditResult::changed,
          "auth cooldown: explicit replacement credentials remain in existing locked editors");
    g_app.login.focus = LoginControl::password;
    check(application_login_handle_key(VK_RETURN) && !audit.login_starts && !audit.password_submits,
          "auth cooldown: returning to ordinary login cannot bypass the retained actor floor");
    const u32 refreshes = audit.refreshes;
    check(application_login_tick_auth_cooldown(due) && !application_login_auth_deadline() &&
              audit.refreshes == refreshes + 1 && !audit.password_submits && !audit.login_starts &&
              application_login_tick_auth_cooldown(due) && audit.refreshes == refreshes + 1,
          "auth cooldown: exact expiry updates once without automatically replaying credentials");
    check(application_login_handle_key(VK_RETURN, true) && !audit.password_submits && !audit.login_starts,
          "auth cooldown: a held Enter repeat after expiry cannot authorize a credential POST");
    check(application_login_handle_key(VK_RETURN) && audit.password_submits == 1 &&
              audit.login_starts == 1 && g_app.login.status == LoginStatus::submitting &&
              zeroed(g_app.login.password.storage),
          "auth cooldown: only a fresh explicit action after expiry creates an owner and submits the new "
          "password");

    check(seed(false), "auth cooldown: reusable MFA rate-limit fixture seeds");
    audit.scripted_mfa = true;
    g_app.login.status = LoginStatus::submitting;
    g_app.login.mfa_type = UserAuthMfaType::totp;
    g_app.login.sms_sending = false;
    audit.recovery_response = {};
    audit.recovery_response.phase = AccountLoginPhase::mfa_required;
    audit.recovery_response.event = UserAuthCoordinatorEventKind::rejected;
    audit.recovery_response.http_status = 429;
    audit.recovery_response.retry_at_ms = due;
    check(application_login_pump() && g_app.account.login_instance == &audit && !audit.stops &&
              zeroed(g_app.login.mfa.storage) && g_app.login.status == LoginStatus::mfa_required &&
              g_app.login.mfa_capabilities == login_mfa_capability_all,
          "auth cooldown: reusable MFA response retains its owner and methods while clearing the attempted "
          "answer");
    audit.recovery_response.retry_at_ms = due - 1'000;
    check(application_login_pump() && application_login_auth_deadline() == due,
          "auth cooldown: a shorter later response cannot reduce the maximum actor floor");
    check(secure_edit_insert_utf8(&g_app.login.mfa, utf8_literal("123456")) == SecureEditResult::changed &&
              application_login_submit_mfa() && !audit.mfa_submits &&
              application_login_tick_auth_cooldown(due) && !audit.mfa_submits && !audit.provider_calls &&
              application_login_submit_mfa() && audit.mfa_submits == 1 && !audit.password_submits,
          "auth cooldown: retained MFA waits, expiry performs no POST, and a fresh action submits only its "
          "new answer");

    for (u32 unknown = 0; unknown < 2; ++unknown) {
        check(seed(false), "auth cooldown: unpumped stop response fixture seeds");
        audit.allow_load = true;
        audit.stop_http_status = 429;
        audit.stop_retry_at_ms = unknown ? 0 : due;
        check(application_login_begin_browser_auth() && !audit.browser_starts && audit.stops == 1 &&
                  !g_app.account.login_instance && !audit.login_starts &&
                  g_app.login.auth_retry_at_ms == (unknown ? 0 : due) &&
                  g_app.login.auth_retry_time_unknown == (unknown != 0),
              "auth cooldown: method switch retains a joined worker's unpumped 429 before admitting a new "
              "owner");
        check(application_login_handle_key(VK_ESCAPE) && editors_clear() &&
                  g_app.login.auth_retry_at_ms == (unknown ? 0 : due) &&
                  g_app.login.auth_retry_time_unknown == (unknown != 0),
              "auth cooldown: Back after raced stop keeps the floor while retiring the old attempt");
        if (unknown) {
            const u32 before = audit.refreshes;
            check(!application_login_auth_deadline() && application_login_auth_cooldown_active() &&
                      application_login_tick_auth_cooldown(~u64{0}) && audit.refreshes == before &&
                      application_login_begin_remote_auth() && !g_app.login.remote_restart_at_ms &&
                      application_login_begin_browser_auth() && !audit.browser_starts &&
                      !audit.remote_starts,
                  "auth cooldown: missing retry time waits across owner recreation without "
                  "wake loops or invented expiry");
            g_app.login.focus = LoginControl::submit;
            check(application_login_handle_key(VK_RETURN, true) && g_app.login.auth_retry_time_unknown &&
                      !audit.password_submits && !audit.remote_starts,
                  "auth cooldown: held Enter cannot clear an unknown receipt");
            check(application_login_handle_key(VK_RETURN) && !g_app.login.auth_retry_time_unknown &&
                      !application_login_auth_deadline() && !g_app.login.remote_restart_at_ms &&
                      !audit.password_submits && !audit.remote_starts && !audit.browser_starts,
                  "auth cooldown: explicit Resume retires paused state and refreshes without sending any request");
            check(secure_edit_insert_utf8(&g_app.login.login, utf8_literal("resume@example.com")) == SecureEditResult::changed &&
                      secure_edit_insert_utf8(&g_app.login.password, utf8_literal("fresh-resume-password")) == SecureEditResult::changed,
                  "auth cooldown: resumed login accepts a new explicit credential attempt");
            g_app.login.focus = LoginControl::password;
            check(application_login_handle_key(VK_RETURN) && audit.password_submits == 1 && audit.login_starts == 1,
                  "auth cooldown: only a later fresh action submits after Resume");
        }
    }
    check(seed(false), "auth cooldown: mixed known and unknown receipts seed");
    g_app.account.login_status.http_status = 429;
    g_app.account.login_status.retry_at_ms = due;
    application_login_retain_auth_retry();
    g_app.account.login_status.retry_at_ms = 0;
    application_login_retain_auth_retry();
    g_app.login.focus = LoginControl::submit;
    audit.fail_stop = true;
    check(application_login_handle_key(VK_RETURN) && g_app.account.stop_failed &&
              g_app.login.auth_retry_time_unknown && application_login_auth_deadline() == due,
          "auth cooldown: failed Resume join preserves unknown restriction and known future floor");
    audit.fail_stop = false;
    check(application_login_handle_key(VK_ESCAPE) && !g_app.account.login_instance &&
              g_app.login.auth_retry_time_unknown && application_login_auth_deadline() == due,
          "auth cooldown: Back retry safely retires paused owner without clearing either receipt");
    g_app.login.focus = LoginControl::submit;
    check(application_login_handle_key(VK_RETURN) && !g_app.login.auth_retry_time_unknown &&
              application_login_auth_deadline() == due && !g_app.login.remote_restart_at_ms &&
              !audit.password_submits && !audit.remote_starts && !audit.browser_starts,
          "auth cooldown: Resume clears only unknown restriction and cannot bypass a known server floor");
    check(application_login_tick_auth_cooldown(due) && !g_app.login.remote_restart_at_ms &&
              !audit.password_submits && !audit.remote_starts,
          "auth cooldown: known expiry after Resume does not automatically restart QR or credentials");
}
void run_logout_fences() noexcept {
    // These predicates model the lifecycle owner's two independent fences.
    // Retain a real login controller with queued authenticated success so any
    // unintended pump/submit/promotion reaches the observed host boundary.
    for (u32 gate = 0; gate < 3; ++gate) {
        check(seed(false), "logout login fence: retained controller and locked editors seed");
        audit.logout_blocked = gate != 1;
        audit.logout_host_owned = gate != 0;
        audit.attach_success = audit.allow_load = true;
        g_app.login.status = LoginStatus::idle;
        g_app.login.remote_status = LoginRemoteStatus::idle;
        g_app.login.focus = LoginControl::submit;
        const auto epoch = g_app.login.application_auth_epoch;
        const auto login_revision = g_app.login.login.revision;
        const auto password_revision = g_app.login.password.revision;
        const auto mfa_revision = g_app.login.mfa.revision;
        check(!application_login_begin_remote_auth() && !application_login_begin_browser_auth() &&
                  !application_login_begin_recovery() && !application_login_begin_reset() &&
                  !application_login_submit_recovery() && !application_login_submit_reset() &&
                  !application_login_submit_phone_verify() && !application_login_submit_phone_resend() &&
                  !application_login_submit_mfa() &&
                  !application_login_select_mfa(UserAuthMfaType::totp, false),
              "logout login fence: direct user admissions refuse without starting any credential QR "
              "browser recovery reset phone or MFA owner");
        check(application_login_handle_key(VK_RETURN) && application_login_handle_key(VK_ESCAPE) &&
                  application_login_handle_click(0) && application_login_cancel_blocked() &&
                  application_login_run_phone_verify(epoch, mfa_revision) &&
                  application_login_run_webauthn(epoch) && application_login_pump() &&
                  g_app.login.status == LoginStatus::idle && g_app.login.application_auth_epoch == epoch &&
                  g_app.login.login.revision == login_revision &&
                  g_app.login.password.revision == password_revision &&
                  g_app.login.mfa.revision == mfa_revision,
              "logout login fence: late input native callbacks and queued authenticated success consume "
              "inertly without changing editors or auth epoch");
        g_app.account.login_status.phase = AccountLoginPhase::browser_waiting;
        g_app.account.login_status.browser_generation = epoch;
        g_app.login.browser_launch_epoch = epoch;
        bool handled = false;
        check(application_browser_auth_pump(&handled) && handled &&
                  g_app.login.browser_launch_epoch == epoch && !audit.browser_copies &&
                  !audit.browser_launches,
              "logout login fence: ready browser receipt cannot consume launch authorization or invoke OS "
              "browser");
        g_app.login.status = LoginStatus::mfa_backup_acknowledgement;
        g_app.login.backup_acknowledgement_seconds = 7;
        g_app.login.backup_acknowledgement_at_ms = 1;
        bool clocks_preserved = application_login_tick_backup_acknowledgement(1000) &&
                                g_app.login.backup_acknowledgement_seconds == 7 &&
                                g_app.login.backup_acknowledgement_at_ms == 1;
        g_app.account.login_status.retry_at_ms = 100;
        g_app.login.status = LoginStatus::recovery_rate_limited;
        clocks_preserved = clocks_preserved && !application_login_recovery_deadline() &&
                           application_login_tick_recovery(1000) &&
                           g_app.login.status == LoginStatus::recovery_rate_limited &&
                           g_app.account.login_status.retry_at_ms == 100;
        g_app.login.reset_mode = true;
        g_app.login.status = LoginStatus::reset_rate_limited;
        clocks_preserved = clocks_preserved && !application_login_reset_deadline() &&
                           application_login_tick_reset(1000) &&
                           g_app.login.status == LoginStatus::reset_rate_limited &&
                           g_app.account.login_status.retry_at_ms == 100;
        g_app.login.reset_mode = false;
        g_app.login.status = LoginStatus::recovery_phone_verify_rate_limited;
        clocks_preserved = clocks_preserved && !application_login_phone_deadline() &&
                           application_login_tick_phone(1000) &&
                           g_app.login.status == LoginStatus::recovery_phone_verify_rate_limited &&
                           g_app.account.login_status.retry_at_ms == 100;
        const auto restart = g_app.login.remote_restart_at_ms;
        const auto retries = g_app.login.remote_retry_attempt;
        application_login_schedule_remote_restart(LoginRemoteStatus::refreshing);
        check(clocks_preserved && g_app.login.remote_restart_at_ms == restart &&
                  g_app.login.remote_retry_attempt == retries && !audit.stops && !audit.pumps &&
                  !audit.login_starts && !audit.module_loads && !audit.path_resolves && !audit.releases &&
                  !audit.attachments && !audit.refreshes && !audit.provider_calls &&
                  !audit.challenge_copies && !audit.recovery_submits && !audit.reset_submits &&
                  !audit.mfa_submits && !audit.phone_verify_submits && !audit.phone_resend_submits &&
                  !audit.phone_takes && g_app.account.login_instance == &audit && !g_app.account.instance,
              "logout login fence: all automatic deadlines/ticks and retry scheduling preserve state with "
              "zero host path UI or provider calls");
    }
    check(seed(false), "logout browser copy fence: actual explicit listener owner seeds");
    audit.allow_load = true;
    check(application_login_begin_browser_auth() && audit.browser_starts == 1,
          "logout browser copy fence: explicit listener waits before transient URL handoff");
    audit.browser_response.phase = AccountLoginPhase::browser_waiting;
    audit.logout_on_browser_copy = true;
    const auto refreshes = audit.refreshes;
    check(application_login_pump() && audit.browser_copies == 1 && !audit.browser_launches &&
              audit.logout_blocked && audit.refreshes == refreshes &&
              g_app.login.status == LoginStatus::browser_starting,
          "logout browser copy fence: scope withdrawal during URL copy rejects launch and stale "
          "waiting/error UI");
    audit.logout_blocked = audit.logout_host_owned = audit.logout_on_browser_copy = false;
}

void run() noexcept {
    const bool allocated = secure_edit_initialize(&g_app.login.login, kUserAuthMaximumLoginBytes, false) &&
        secure_edit_initialize(&g_app.login.password, 256, true) &&
        secure_edit_initialize(&g_app.login.mfa, 256, true) &&
        secure_buffer_initialize(&audit.challenge, 8192).ok();
    audit.event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    check(allocated && audit.event, "login cancel: locked test buffers/event initialized");
    if (!allocated || !audit.event) return;

    check(seed(false) && application_login_cancel_blocked() && restored_kept() && audit.stops == 1 &&
        audit.releases == 1 && audit.secrets_cleared_before_stop && zeroed(audit.challenge),
          "password CAPTCHA Back wipes secrets before stop, destroys challenge and keeps the typed "
          "identifier");
    check(application_login_pump() && !audit.pumps && !audit.attachments,
        "cancelled password owner cannot adopt a queued authenticated result");

    check(seed(true) && application_login_handle_key(VK_ESCAPE) && restored_kept() && audit.stops == 1 &&
        audit.secrets_cleared_before_stop && zeroed(audit.challenge),
        "QR CAPTCHA Escape follows actual cancellation and clears QR retry/preview state");
    check(seed(false) && application_login_handle_key(VK_ESCAPE) && restored_kept(),
        "password CAPTCHA Escape follows the same cancellation path");

    check(seed(true), "failed stop fixture seeded");
    audit.fail_stop = true;
    check(application_login_cancel_blocked() && secrets_clear() && audit.secrets_cleared_before_stop &&
        g_app.login.status == LoginStatus::cancelling && g_app.login.focus == LoginControl::back &&
        g_app.account.login_instance == &audit && g_app.account.stop_failed && !audit.releases &&
        audit.challenge.bytes != 0,
        "failed stop retains worker/challenge ownership and Back retry while local credentials are wiped");
    HANDLE handles[kAccountHostNotificationHandleCapacity]{};
    check(account_host_notification_handles(&g_app.account, handles, kAccountHostNotificationHandleCapacity) == 0 &&
        !audit.notifications && application_login_pump() && !audit.pumps && !audit.attachments,
        "failed stop quarantines already-signaled notification and queued login success");
    g_app.login.webauthn_attempted = true;
    check(application_login_run_webauthn() && !audit.challenge_copies && !audit.provider_calls &&
        g_app.login.status == LoginStatus::cancelling,
          "stale posted security-key action cannot overwrite failed cancellation or enter "
          "challenge/provider APIs");
    audit.fail_stop = false;
    check(application_login_handle_key(VK_RETURN) && restored() && audit.stops == 2 && zeroed(audit.challenge),
        "Back Enter retries the retained worker stop and releases its challenge before restoring input");

    check(seed(false), "module release failure fixture seeded");
    audit.fail_release = true;
    check(application_login_cancel_blocked() && secrets_clear() && !g_app.account.login_instance &&
        g_app.account.module && g_app.login.status == LoginStatus::cancelling &&
        g_app.login.focus == LoginControl::back && audit.stops == 1 && audit.releases == 1,
        "module release failure retains cancelling rather than admitting a fresh login");
    check(application_login_pump() && !audit.pumps && !audit.attachments,
        "release-pending cancellation cannot adopt authentication results");
    audit.fail_release = false;
    check(application_login_handle_key(VK_SPACE) && restored() && audit.stops == 1 && audit.releases == 2,
        "Back Space retries module release without stopping an already-destroyed worker again");

    g_app.login.focus = LoginControl::forgot_password;
    check(application_login_handle_key(VK_RETURN) && g_app.login.status == LoginStatus::recovery_editing &&
        g_app.login.focus == LoginControl::login && !audit.attachments,
        "native Forgot Enter opens identifier-only recovery instead of password submission");
    check(application_login_handle_key(VK_ESCAPE) && restored(), "recovery editing Escape restores clean ordinary login");
    check(seed(true), "recovery QR handoff fixture seeds actual remote owner");
    g_app.login.remote_status = LoginRemoteStatus::qr_ready;
    g_app.login.focus = LoginControl::forgot_password;
    audit.allow_load = true;
    check(application_login_handle_key(VK_SPACE) && audit.stops == 1 && !g_app.account.login_instance &&
        g_app.login.status == LoginStatus::recovery_editing && !g_app.login.remote_active && !g_app.login.qr_ready &&
        !g_app.login.remote_restart_at_ms && zeroed(g_app.login.password.storage) && zeroed(g_app.login.mfa.storage) &&
        g_app.login.login.storage.bytes && !audit.recovery_submits,
          "native recovery Space quiesces QR before retaining only locked identifier and showing recovery "
          "form");
    constexpr auto exact_recovery = utf8_literal("{\"login\":\"synthetic-test-only\"}");
    check(application_login_handle_key(VK_RETURN) && audit.recovery_submits == 1 &&
        g_app.login.status == LoginStatus::recovery_pending && g_app.login.focus == LoginControl::back && editors_clear() &&
        audit.challenge.bytes == exact_recovery.bytes && bytes_equal(audit.challenge.data, exact_recovery.data, exact_recovery.bytes),
          "native identifier Enter calls only recovery SDK hook synchronously and clears every app secret "
          "before pending");
    check(application_login_handle_key(VK_RETURN) && restored() && audit.recovery_submits == 1,
        "pending Back Enter cancels recovery without duplicate request");
    const AccountLoginPhase phases[]{AccountLoginPhase::recovery_sent, AccountLoginPhase::recovery_phone_required,
        AccountLoginPhase::captcha_required, AccountLoginPhase::recovery_rejected, AccountLoginPhase::recovery_rate_limited};
    const LoginStatus statuses[]{LoginStatus::recovery_sent, LoginStatus::recovery_phone_required,
        LoginStatus::recovery_captcha_required, LoginStatus::recovery_failed, LoginStatus::recovery_rate_limited};
    for (u32 i = 0; i < 5; ++i) {
        check(seed(false), "recovery metadata fixture seeds owner");
        g_app.login.status = LoginStatus::recovery_pending;
        secure_edit_clear(&g_app.login.login); secure_edit_clear(&g_app.login.password); secure_edit_clear(&g_app.login.mfa);
        audit.recovery_submits = 1;
        audit.recovery_response.phase = phases[i];
        audit.recovery_response.event = i == 0 ? UserAuthCoordinatorEventKind::recovery_sent :
            i == 1 ? UserAuthCoordinatorEventKind::recovery_phone_required :
            i == 2 ? UserAuthCoordinatorEventKind::captcha_required : UserAuthCoordinatorEventKind::recovery_rejected;
        audit.recovery_response.retry_at_ms = i == 4 ? clock_monotonic_milliseconds() + 21'600'000 : 0;
        audit.recovery_response.http_status = i == 4 ? 429 : 0;
        check(application_login_pump() && g_app.login.status == statuses[i] && !audit.attachments,
              "actual recovery pump maps sent phone CAPTCHA rejection and rate states without "
              "Gateway/profile attachment");
        if (i == 4) {
            const auto due = g_app.account.login_status.retry_at_ms;
            check(application_login_recovery_deadline() == due && application_login_tick_recovery(due - 1) &&
                g_app.login.status == LoginStatus::recovery_rate_limited && audit.recovery_submits == 1,
                "native cooldown preserves authoritative long deadline before explicit retry becomes "
                "eligible");
            g_app.account.stop_failed = true;
            check(!application_login_recovery_deadline(), "native cooldown has no wake while owner is quarantined");
            g_app.account.stop_failed = false;
            check(application_login_tick_auth_cooldown(due) && application_login_tick_recovery(due) && g_app.login.status == LoginStatus::recovery_failed &&
                !application_login_recovery_deadline() && !g_app.account.login_status.retry_at_ms,
                "native exact cooldown expiry clears once without a zero-timeout wake loop");
        }
        check(application_login_cancel_blocked() && restored(), "native recovery terminal/error Back erases owner and restores login");
    }
    check(seed(false), "recovery no-deadline rate-limit fixture seeds owner");
    g_app.login.status = LoginStatus::recovery_pending; audit.recovery_submits = 1;
    audit.recovery_response.phase = AccountLoginPhase::recovery_rate_limited;
    audit.recovery_response.http_status = 429;
    audit.recovery_response.event = UserAuthCoordinatorEventKind::recovery_rejected;
    check(application_login_pump() && g_app.login.status == LoginStatus::recovery_rate_limited &&
        !application_login_recovery_deadline() && application_login_tick_recovery(~u64{0}) &&
        application_login_submit_recovery() && audit.recovery_submits == 1 && !audit.attachments,
        "native 429 without authoritative deadline stays static and admits no automatic or Submit replay");
    check(application_login_cancel_blocked() && restored(), "native no-deadline rate-limit Back restores explicit login ownership");
    check(seed(false), "recovery failed-stop fixture seeds actual owner");
    g_app.login.status = LoginStatus::recovery_pending; audit.fail_stop = true;
    check(application_login_handle_key(VK_ESCAPE) && editors_clear() && g_app.account.stop_failed &&
        g_app.login.status == LoginStatus::cancelling && application_login_pump() && !audit.pumps && !audit.attachments,
        "recovery Escape with failed stop quarantines queued result until explicit Back retry");
    audit.fail_stop = false;
    check(application_login_handle_key(VK_SPACE) && restored(), "recovery failed-stop Back retries safely");


    check(seed(true), "reset URI fixture seeds ordinary QR owner");
    g_app.login.status = LoginStatus::idle; g_app.login.remote_status = LoginRemoteStatus::qr_ready;
    g_app.login.focus = LoginControl::login;
    secure_edit_clear(&g_app.login.login);
    constexpr auto reset_link = utf8_literal("https://discord.com/reset?token=query#token=reset-test-token");
    audit.allow_load = true;
    check(secure_edit_insert_utf8(&g_app.login.login, reset_link) == SecureEditResult::changed &&
        application_login_handle_key(VK_RETURN) && g_app.login.reset_mode &&
        g_app.login.status == LoginStatus::reset_editing && g_app.login.focus == LoginControl::password &&
        g_app.login.login.password && g_app.login.login.storage.bytes == 16 &&
        bytes_equal(g_app.login.login.storage.data, "reset-test-token", 16) &&
        zeroed(g_app.login.password.storage) && zeroed(g_app.login.mfa.storage) && !g_app.account.login_instance &&
        !g_app.account.module && !g_app.login.remote_active && audit.stops == 1 && !audit.reset_submits,
          "actual identifier Enter consumes hash-first reset URI, wipes passwords and releases QR owner "
          "before editing");
    check(secure_edit_insert_utf8(&g_app.login.password, utf8_literal("new-test-password")) == SecureEditResult::changed &&
        application_login_handle_key(VK_RETURN) && audit.reset_submits == 1 && editors_clear() &&
        g_app.login.status == LoginStatus::reset_pending && g_app.login.focus == LoginControl::back && !audit.attachments,
          "native new-password Enter starts reset-specific SDK owner and erases every app secret before "
          "pending");
    audit.recovery_response.phase = AccountLoginPhase::reset_rejected;
    audit.recovery_response.event = UserAuthCoordinatorEventKind::rejected;
    audit.recovery_response.http_status = 400;
    check(application_login_pump() && g_app.login.reset_retry && g_app.login.status == LoginStatus::reset_rejected &&
        g_app.account.login_instance == &audit && !audit.attachments,
          "explicit initial reset rejection preserves owner and enables password retry without app token "
          "retention");
    check(secure_edit_insert_utf8(&g_app.login.password, utf8_literal("another-test-password")) == SecureEditResult::changed &&
        application_login_handle_key(VK_RETURN) && audit.reset_submits == 2 && audit.reset_null_retry && editors_clear(),
          "new-password retry passes null token only after retained initial rejection and wipes password "
          "synchronously");
    audit.recovery_response.phase = AccountLoginPhase::reset_rate_limited;
    audit.recovery_response.event = UserAuthCoordinatorEventKind::rejected;
    audit.recovery_response.retry_at_ms = clock_monotonic_milliseconds() + 10;
    check(application_login_pump() && g_app.login.status == LoginStatus::reset_rate_limited &&
        !application_login_recovery_deadline() && application_login_tick_recovery(~u64{0}) &&
        application_login_submit_reset() && audit.reset_submits == 2 && !audit.attachments,
        "reset rate-limit deadline has no automatic mutation retry or recovery wake");
    const auto reset_due = g_app.account.login_status.retry_at_ms;
    check(application_login_reset_deadline() == reset_due && application_login_tick_reset(reset_due - 1) &&
        g_app.login.status == LoginStatus::reset_rate_limited && audit.reset_submits == 2,
        "native reset cooldown preserves authoritative deadline without premature request");
    check(application_login_tick_auth_cooldown(reset_due) && application_login_tick_reset(reset_due) && g_app.login.status == LoginStatus::reset_editing &&
        g_app.login.reset_retry && !application_login_reset_deadline() && audit.reset_submits == 2,
          "native reset cooldown expiry reopens password editing without automatic POST and permits "
          "retained-token retry");
    check(application_login_handle_key(VK_ESCAPE) && restored(), "reset rate-limit Escape securely restores login");
    const UserAuthMfaType rate_methods[]{UserAuthMfaType::totp, UserAuthMfaType::backup,
        UserAuthMfaType::sms, UserAuthMfaType::webauthn, UserAuthMfaType::sms};
    for (u32 i = 0; i < 5; ++i) {
        check(seed(false), "retained reset MFA cooldown fixture seeds actual owner");
        g_app.login.reset_mode = true; g_app.login.status = LoginStatus::reset_mfa_pending;
        g_app.login.mfa_type = rate_methods[i]; g_app.login.mfa_capabilities = login_mfa_capability_all;
        g_app.login.sms_requested = i == 2; g_app.login.sms_sending = i == 4; g_app.login.sms_failed = false;
        secure_edit_clear(&g_app.login.login); secure_edit_clear(&g_app.login.password); secure_edit_clear(&g_app.login.mfa);
        audit.reset_submits = 1; audit.recovery_response = {};
        audit.recovery_response.phase = AccountLoginPhase::reset_rate_limited;
        audit.recovery_response.event = UserAuthCoordinatorEventKind::rejected;
        audit.recovery_response.http_status = 429;
        audit.recovery_response.retry_at_ms = clock_monotonic_milliseconds() + 100;
        const auto due = audit.recovery_response.retry_at_ms;
        check(application_login_pump() && g_app.login.status == LoginStatus::reset_rate_limited &&
            application_login_tick_reset(due - 1) && g_app.login.status == LoginStatus::reset_rate_limited,
            "all reset MFA methods and SMS send remain Back-only before authoritative cooldown expires");
        check(application_login_tick_auth_cooldown(due) && application_login_tick_reset(due) && !g_app.login.reset_retry &&
            g_app.login.status == (i == 3 ? LoginStatus::webauthn_failed : LoginStatus::mfa_required) &&
            g_app.login.mfa_type == rate_methods[i] && g_app.login.mfa_capabilities == login_mfa_capability_all &&
            !g_app.login.sms_sending && !g_app.login.webauthn_attempted && !audit.mfa_submits && audit.reset_submits == 1 &&
            (i != 2 || g_app.login.sms_requested) && (i != 4 || (!g_app.login.sms_requested && g_app.login.sms_failed)),
              "reset MFA cooldown restores exact retained chooser/key/SMS stage without new-password "
              "dispatch or automatic POST");
        audit.recovery_response.event = UserAuthCoordinatorEventKind::none;
        check(application_login_pump() && g_app.login.status == (i == 3 ? LoginStatus::webauthn_failed : LoginStatus::mfa_required) &&
            !application_login_reset_deadline() && !audit.mfa_submits && audit.reset_submits == 1,
              "idle coordinator rate snapshot cannot reblock an expired reset MFA cooldown or spin the "
              "wake scheduler");
        check(application_login_handle_key(VK_ESCAPE) && restored(), "retained reset MFA cooldown Back securely restores login");
    }
    for (u32 i = 0; i < 3; ++i) {
        check(seed(false), "terminal reset rejection fixture seeds actual owner");
        g_app.login.reset_mode = true; g_app.login.status = LoginStatus::reset_pending;
        g_app.login.mfa_capabilities = i == 1 ? login_mfa_capability_all : 0;
        audit.reset_submits = 1; audit.recovery_response = {};
        audit.recovery_response.phase = AccountLoginPhase::reset_rejected;
        audit.recovery_response.event = UserAuthCoordinatorEventKind::rejected;
        audit.recovery_response.http_status = i == 0 ? 401 : i == 1 ? 400 : 429;
        audit.recovery_response.error_code = i == 1 ? 50035 : 0;
        check(application_login_pump() && g_app.login.status == LoginStatus::reset_failed &&
            g_app.login.focus == LoginControl::back && !g_app.login.reset_retry && editors_clear() &&
            application_login_submit_reset() && audit.reset_submits == 1 && !audit.mfa_submits && !audit.attachments,
              "initial401 final non-code400 and no-deadline429 reset rejection remain terminal Back-only "
              "without null-token retry");
        check(application_login_handle_key(VK_ESCAPE) && restored(), "terminal reset rejection Back destroys retained owner");
    }
    check(seed(false), "reset failed-stop fixture seeds owner");
    g_app.login.reset_mode = true; g_app.login.status = LoginStatus::reset_captcha_required;
    audit.fail_stop = true;
    const auto old_epoch = g_app.login.application_auth_epoch;
    check(application_login_handle_key(VK_ESCAPE) && editors_clear() && audit.cleared_before_stop &&
        g_app.login.application_auth_epoch > old_epoch && g_app.login.reset_mode &&
        g_app.login.status == LoginStatus::cancelling && g_app.account.login_instance == &audit &&
        application_login_pump() && !audit.pumps && !audit.attachments,
          "reset CAPTCHA cancellation invalidates auth epoch and wipes local secrets before live-stop "
          "quarantine");
    audit.fail_stop = false;
    check(application_login_handle_key(VK_SPACE) && restored(), "reset failed-stop Back retries worker and release");
    for (u32 scenario = 1; scenario <= 14; ++scenario) {
        check(seed(false), "nested native ceremony fixture seeds actual app owner");
        g_app.login.reset_mode = true; g_app.login.status = LoginStatus::webauthn_required;
        g_app.login.mfa_type = UserAuthMfaType::webauthn;
        g_app.login.webauthn_attempted = true;
        ++g_app.login.application_auth_epoch;
        const auto posted = g_app.login.application_auth_epoch;
        g_app.login.webauthn_post_epoch = posted;
        audit.ceremony_scenario = scenario;
        if (scenario == 1)
            check(application_login_run_webauthn(posted - 1) && !audit.provider_calls && !audit.challenge_copies,
                "stale posted native action is discarded before challenge copy or platform ceremony");
        check(application_login_run_webauthn(posted) && audit.provider_calls == 1 && audit.challenge_copies == 1 &&
            audit.mfa_submits == (scenario == 9 ? 1u : 0u),
              "post-modal guard submits only unchanged owner module generation request method mode epoch "
              "visibility and status");
        if (scenario == 1) check(restored() && audit.cleared_before_stop,
            "successful nested cancellation stays restored after stale native credential returns");
        if (scenario == 2) check(g_app.login.status == LoginStatus::cancelling && g_app.account.stop_failed && editors_clear(),
                  "failed nested cancellation remains quarantined after platform return without stale UI "
                  "error mutation");
        if (scenario == 9) check(g_app.login.status == LoginStatus::reset_mfa_pending,
            "admitted native WebAuthn completion uses reset-specific final MFA pending phase");
        if (scenario >= 13) check(g_app.login.status == LoginStatus::reset_mfa_pending &&
            !audit.mfa_submits && audit.provider_calls == 1 && audit.challenge_copies == 1,
                  "logout starting during native ceremony discards its credential without host submission "
                  "or stale error mutation");
    }
    check(seed(false), "reset protected-profile save-only fixture seeds actual login owner");
    g_app.login.reset_mode = true; g_app.login.status = LoginStatus::reset_pending;
    g_app.login.mfa_capabilities = 0;
    secure_edit_clear(&g_app.login.login); secure_edit_clear(&g_app.login.password); secure_edit_clear(&g_app.login.mfa);
    audit.reset_submits = 1; audit.recovery_response = {};
    audit.recovery_response.phase = AccountLoginPhase::authenticated;
    audit.recovery_response.error = AccountLoginError::profile;
    audit.recovery_response.event = UserAuthCoordinatorEventKind::authenticated;
    check(application_login_pump() && g_app.login.status == LoginStatus::profile_save_failed &&
        g_app.login.focus == LoginControl::submit && editors_clear() && !audit.attachments &&
        audit.challenge.bytes && audit.reset_submits == 1 && !audit.mfa_submits,
          "actual reset save failure retains admitted protected-session owner without editing credentials "
          "or promoting Gateway");
    const auto save_pumps = audit.pumps;
    g_app.login.focus = LoginControl::password;
    check(application_login_handle_key(VK_RETURN) && audit.pumps == save_pumps &&
        g_app.login.status == LoginStatus::profile_save_failed && audit.reset_submits == 1 && !audit.mfa_submits,
        "stale password-field Enter is inert during actual reset save-only state");
    g_app.login.focus = LoginControl::submit;
    check(application_login_handle_key(VK_RETURN) && audit.pumps == save_pumps + 1 &&
        g_app.login.status == LoginStatus::profile_save_failed && editors_clear() &&
        audit.reset_submits == 1 && !audit.mfa_submits && !audit.attachments,
          "actual reset Try again Enter pumps existing protected-session save without repeating reset or "
          "MFA submission");
    check(application_login_handle_key(VK_ESCAPE) && restored() && zeroed(audit.challenge),
        "actual reset save-only Back destroys retained protected credential and restores native login");
    run_mfa_retry();
    run_auth_cooldown();
    run_phone();
    check(seed(true), "browser UI: QR challenge owner seeds"); audit.allow_load = true;
    check(application_login_begin_browser_auth() && audit.browser_starts == 1 && !audit.browser_launches &&
        audit.stops == 1 && audit.cleared_before_stop && !g_app.login.remote_active && !g_app.login.qr_ready &&
        g_app.login.status == LoginStatus::browser_starting && g_app.login.browser_launch_epoch == g_app.login.application_auth_epoch,
          "browser UI: explicit action clears QR/editors and joins old owner before asynchronously "
          "starting listener");
    check(application_login_pump() && !audit.browser_copies && !audit.browser_launches,
        "browser UI: starting listener never launches prematurely");
    audit.browser_response.phase = AccountLoginPhase::browser_waiting;
    check(application_login_pump() && audit.browser_copies == 1 && audit.browser_launches == 1 && !g_app.login.browser_launch_epoch &&
        g_app.login.status == LoginStatus::browser_waiting,
        "browser UI: matching waiting generation consumes explicit launch authorization exactly once");
    check(application_login_pump() && audit.browser_launches == 1 && application_login_handle_key(VK_RETURN) &&
        !audit.recovery_submits && !audit.reset_submits && application_login_handle_key(VK_ESCAPE) && restored(true),
        "browser UI: waiting never submits credentials and Escape joins owner before fresh native login");
    check(seed(false), "browser UI: retained CAPTCHA seeds"); audit.allow_load = true; audit.fail_stop = true;
    check(application_login_begin_browser_auth() && !audit.browser_starts && !audit.browser_launches &&
        g_app.login.status == LoginStatus::cancelling && g_app.account.login_instance && g_app.account.module,
        "browser UI: failed old-owner join quarantines module and denies browser attempt");
    audit.fail_stop = false; check(application_login_cancel_blocked() && restored(true), "browser UI: stop retry releases old owner before fresh QR");
    check(seed(false), "browser UI: module release failure seeds"); audit.allow_load = true; audit.fail_release = true;
    check(application_login_begin_browser_auth() && !audit.browser_starts && !audit.browser_launches &&
        !g_app.account.login_instance && g_app.account.module && g_app.login.browser_return_to_qr &&
        g_app.login.status == LoginStatus::cancelling,
        "browser UI: failed module release retains fresh-QR return intent without creating browser owner");
    audit.fail_release = false;
    check(application_login_cancel_blocked() && restored(true) && !g_app.login.browser_return_to_qr,
        "browser UI: release retry schedules fresh QR only after exact old module is gone");
    check(seed(false), "browser UI: launch failure seeds"); audit.allow_load = true;
    check(application_login_begin_browser_auth(), "browser UI: launch failure listener starts");
    audit.browser_response.phase = AccountLoginPhase::browser_waiting; audit.fail_browser_launch = true;
    check(application_login_pump() && audit.browser_launches == 1 && !g_app.account.login_instance && !g_app.account.module &&
        g_app.login.status == LoginStatus::browser_failed && !g_app.login.browser_launch_epoch,
        "browser UI: OS launch failure stops listener and leaves a recoverable native error");
    check(application_login_cancel_blocked() && restored(true), "browser UI: launch failure Back restores native login and fresh QR");
    check(seed(false), "browser UI: expiry seeds"); audit.allow_load = true;
    check(application_login_begin_browser_auth(), "browser UI: expiry listener starts");
    audit.browser_response.phase = AccountLoginPhase::browser_expired;
    check(application_login_pump() && !audit.browser_launches && !g_app.account.module &&
        g_app.login.status == LoginStatus::browser_expired && !g_app.login.browser_launch_epoch,
        "browser UI: expiry cancels unconsumed launch and stops exact owner");
    run_logout_fences();
    secure_edit_release(&g_app.login.login);
    secure_edit_release(&g_app.login.password);
    secure_edit_release(&g_app.login.mfa);
    secure_buffer_release(&audit.challenge);
    CloseHandle(audit.event); audit.event = nullptr;
    g_app.account = {};
}
} // namespace maicord::login_cancel_test

namespace maicord::app_detail {
// This auth-only harness has no scoped read owner or menu. Account cleanup may
// retire these dormant boundaries without introducing transport/UI work.
void application_scoped_read_reset() noexcept {
}
void application_scoped_read_menu_clear() noexcept {
}
bool application_account_logout_blocked() noexcept {
    return login_cancel_test::audit.logout_blocked;
}
bool application_account_logout_host_owned() noexcept {
    return login_cancel_test::audit.logout_host_owned;
}
bool application_refresh_login_view() noexcept {
    ++login_cancel_test::audit.refreshes;
    return true;
}
void application_login_end_pointer_selection() noexcept {
    g_app.login.pointer_selection = LoginControl::none;
}
bool application_account_module_load() noexcept {
    ++login_cancel_test::audit.module_loads;
    if (login_cancel_test::audit.allow_load) {
        g_app.account.module = reinterpret_cast<HMODULE>(1);
        g_app.account.api.login_start = login_cancel_test::start;
        g_app.account.api.login_submit_password = login_cancel_test::password;
        g_app.account.api.login_start_remote_auth = login_cancel_test::remote_start;
        g_app.account.api.login_start_reset = login_cancel_test::start;
        g_app.account.api.login_start_browser = login_cancel_test::browser_start;
        g_app.account.api.login_copy_browser_url = login_cancel_test::browser_copy;
        g_app.account.api.login_submit_reset = login_cancel_test::reset;
        g_app.account.api.login_submit_recovery = login_cancel_test::recovery;
        g_app.account.api.login_submit_phone_verify = login_cancel_test::phone_verify;
        g_app.account.api.login_submit_phone_resend = login_cancel_test::phone_resend;
        g_app.account.api.login_take_phone_reset_token = login_cancel_test::phone_take;
        g_app.account.api.login_submit_mfa = login_cancel_test::mfa;
        g_app.account.api.login_stop = login_cancel_test::stop;
        g_app.account.api.login_pump = login_cancel_test::pump;
    }
    return login_cancel_test::audit.allow_load;
}
void application_account_module_release() noexcept {
    ++login_cancel_test::audit.releases;
    if (!login_cancel_test::audit.fail_release && !g_app.account.login_instance && !g_app.account.instance)
        g_app.account = {};
}
bool application_account_attach_if_present() noexcept {
    ++login_cancel_test::audit.attachments;
    if (login_cancel_test::audit.attach_success) { g_app.account.instance = &login_cancel_test::audit; g_app.login.visible = false; }
    return true;
}
SecureEdit* application_active_login_edit() noexcept {
    return g_app.login.initialized && g_app.login.visible && login_phone_editable(g_app.login.status) &&
        g_app.login.focus == LoginControl::phone_code ? &g_app.login.mfa : nullptr;
}
bool application_login_paste() noexcept { return false; }
bool application_login_position_edit(LoginControl, i32, i32, bool) noexcept { return false; }
} // namespace maicord::app_detail

namespace maicord {
// The CAPTCHA bridge never launches in this harness (no challenge is bridged).
bool captcha_bridge_url_valid(const SecureBuffer*) noexcept { return false; }
bool captcha_bridge_launch(HWND, const SecureBuffer*) noexcept { return false; }
bool browser_sign_in_launch(HWND, const SecureBuffer* url) noexcept {
    auto& audit = login_cancel_test::audit;
    ++audit.browser_launches;
    login_cancel_test::check(url && url->locked && url->bytes && !app_detail::g_app.login.browser_launch_epoch,
        "browser UI: OS boundary receives locked transient URL after authorization is consumed");
    return !audit.fail_browser_launch;
}
AccountPathsResult account_paths_resolve(AccountPaths* paths) noexcept {
    ++login_cancel_test::audit.path_resolves;
    if (!paths) return {0, AccountPathsError::invalid_input};
    *paths = {}; paths->profile[0] = L'p'; return {};
}
AccountPathsResult account_paths_ensure_directories(const AccountPaths&) noexcept { return {}; }
WindowsWebAuthnResult windows_webauthn_authenticate(HWND, const WebAuthnMfaRequestOptions&, SecureBuffer* output) noexcept {
    using namespace app_detail;
    auto& audit = login_cancel_test::audit;
    ++audit.provider_calls;
    constexpr char credential[] = "synthetic-native-assertion";
    secure_buffer_assign(output, {reinterpret_cast<const u8*>(credential), sizeof(credential) - 1});
    switch (audit.ceremony_scenario) {
    case 1: application_login_cancel_blocked(); break;
    case 2: audit.fail_stop = true; application_login_cancel_blocked(); break;
    case 3: g_app.account.login_instance = &audit.event; break;
    case 4: ++g_app.account.login_status.generation; break;
    case 5: g_app.login.mfa_type = UserAuthMfaType::totp; break;
    case 6: g_app.login.reset_mode = false; break;
    case 7: g_app.login.visible = false; break;
    case 8: g_app.login.status = LoginStatus::reset_failed; break;
    case 10: g_app.account.module = reinterpret_cast<HMODULE>(2); break;
    case 11: ++g_app.account.login_status.request_id; break;
    case 12: ++g_app.login.application_auth_epoch; break;
    case 13: audit.logout_blocked = true; break;
    case 14: audit.logout_host_owned = true; break;
    default: break;
    }
    WindowsWebAuthnResult result{};
    if (!audit.ceremony_scenario) result.error = WindowsWebAuthnError::unavailable;
    return result;
}
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_login_cancel_runtime_entry() noexcept {
    maicord::login_cancel_test::run();
    constexpr char pass[] = "PASS application login cancel runtime\n";
    constexpr char fail[] = "FAIL application login cancel runtime\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), maicord::login_cancel_test::failures ? fail : pass,
        sizeof(pass) - 1, &written, nullptr);
    ExitProcess(maicord::login_cancel_test::failures ? 1 : 0);
}
