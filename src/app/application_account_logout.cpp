#include "application_account_logout.hpp"
#include "application_account_module.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "application_login_ui.hpp"
#include "maicord/app/account_credential_forget.hpp"
#include "maicord/app/account_logout_stop.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/app/account_logout.hpp"

namespace maicord::app_detail {
namespace {
enum class LogoutPhase : u8 {
    confirmation,
    marker_failed,
    admission,
    server,
    stop_start_failed,
    stopping,
    stop_failed,
    forget_failed,
    startup_recovery,
    complete
};
constexpr u64 kLogoutDeadlineMilliseconds = 10'000;
constexpr u64 kLogoutAdmissionRetryMilliseconds = 125;
using LogoutCallback = decltype(AccountModuleApi::logout);
bool startup_blocked{};
u64 startup_retry_due{};
} // namespace

struct ApplicationAccountLogout {
    AccountPaths paths;
    AccountLogoutStopOwner stop;
    AccountHost stop_host;
    const AccountUiRuntime* source_ui;
    void* source_instance;
    LogoutCallback server;
    TextFormOwnerKey key;
    u64 actor_scope, deadline, admission_due, form_due, request_id, revision;
    Utf8View status;
    LogoutPhase phase;
    bool committed, paths_valid, submitted, cancelled, had_account;
    bool unconfirmed, form_bound, cleanup_joined, startup, login_published, host_transferred;
};
static_assert(sizeof(ApplicationAccountLogout) < 16 * 1024);

namespace {
bool owner_key(const ApplicationAccountLogout& owner, TextFormOwnerKey key) noexcept {
    return owner.key.scope == key.scope && owner.key.collection == key.collection &&
           owner.key.item == key.item && key.scope == g_app.account_logout_lifetime_serial;
}
bool source_current(const ApplicationAccountLogout& owner) noexcept {
    // Never call after ownership passes to the stop thread.
    return !account_logout_stop_owned(owner.stop) && g_app.account_ui && !g_app.login.visible &&
           g_app.account_ui == owner.source_ui && g_app.account.instance == owner.source_instance &&
           !g_app.account.stop_failed && g_app.account.api.logout == owner.server &&
           g_app.plugin_account_scope_serial == owner.actor_scope &&
           g_app.account.status.profile_id == owner.key.collection &&
           g_app.account_ui->chat_state.self_user_id == owner.key.item;
}
bool form_valid(TextFormOwnerKey key) noexcept {
    const auto* owner = g_app.account_logout;
    return owner && owner_key(*owner, key) && (owner->committed || source_current(*owner));
}
void publish(ApplicationAccountLogout& owner, LogoutPhase phase, Utf8View status) noexcept {
    if (owner.phase == phase && utf8_equal(owner.status, status))
        return;
    owner.phase = phase;
    owner.status = status;
    ++owner.revision;
    application_request_frame();
}
bool allocate_owner() noexcept {
    if (g_app.account_logout)
        return true;
    g_app.account_logout = static_cast<ApplicationAccountLogout*>(
        VirtualAlloc(nullptr, sizeof(ApplicationAccountLogout), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return g_app.account_logout != nullptr;
}
void initialize_key(ApplicationAccountLogout& owner, u64 profile, u64 self) noexcept {
    ++g_app.account_logout_lifetime_serial;
    if (!g_app.account_logout_lifetime_serial)
        ++g_app.account_logout_lifetime_serial;
    owner.key = {g_app.account_logout_lifetime_serial, profile, self};
    owner.revision = 1;
}
bool resolve_paths(ApplicationAccountLogout& owner) noexcept {
    if (owner.paths_valid)
        return true;
    owner.paths_valid = account_paths_resolve(&owner.paths).ok();
    return owner.paths_valid;
}
void cancel_server(ApplicationAccountLogout& owner) noexcept {
    if (!owner.request_id || !owner.server || owner.cancelled)
        return;
    owner.server(owner.source_instance, AccountLogoutOperation::cancel, owner.request_id, nullptr);
    owner.cancelled = true;
}
void start_stop(ApplicationAccountLogout& owner) noexcept {
    if (account_logout_stop_owned(owner.stop))
        return;
    owner.admission_due = owner.deadline = 0;
    // Move the entire host out on the UI thread. Any remaining UI reads
    // see a stable empty host; only the worker touches the private copy.
    owner.stop_host = g_app.account;
    g_app.account = {};
    owner.host_transferred = true;
    const auto started = account_logout_stop_start(&owner.stop, &owner.stop_host, kAccountStopTimeoutMs,
                                                   clock_monotonic_milliseconds(),
                                                   clock_unix_milliseconds(), account_host_stop);
    if (!started.ok()) {
        g_app.account = owner.stop_host;
        owner.stop_host = {};
        owner.host_transferred = false;
        publish(owner, LogoutPhase::stop_start_failed,
                utf8_literal("Couldn't finish logging out. Try again."));
        return;
    }
    publish(owner, LogoutPhase::stopping, utf8_literal("Logging out…"));
}
void cleanup(ApplicationAccountLogout& owner, bool publish_login) noexcept {
    // Both account and login workers must have joined. No deletion is permitted
    // while the asynchronous worker still borrows the host.
    if (account_logout_stop_owned(owner.stop) || !owner.cleanup_joined || g_app.account.instance ||
        g_app.account.login_instance || owner.stop_host.instance || owner.stop_host.login_instance ||
        owner.stop_host.module) {
        publish(owner, LogoutPhase::stop_failed,
                utf8_literal("Still logging out. Try again in a moment."));
        return;
    }
    const auto forgotten =
        resolve_paths(owner)
            ? account_credential_forget_complete_after_join(owner.paths, true)
            : AccountCredentialForgetResult{0, AccountCredentialForgetError::probe, 0, true};
    if (!forgotten.ok()) {
        publish(owner, LogoutPhase::forget_failed,
                utf8_literal("Couldn't remove saved sign-in data. Try again."));
        return;
    }
    startup_blocked = false;
    // Release the form before its borrowed account UI storage. Its closed
    // callback observes only this terminal owner, never the stopped module.
    publish(owner, LogoutPhase::complete, {});
    if (owner.form_bound)
        application_text_form_cancel_owner(form_valid);
    owner.committed = false;
    if (publish_login && !owner.login_published) {
        owner.login_published = application_account_publish_logged_out(owner.unconfirmed);
        if (!owner.login_published) {
            // Credentials are already absent. Keep login blocked
            // until the neutral login presentation can be rebuilt.
            owner.committed = true;
            owner.startup = true;
            g_app.login.visible = true;
            owner.form_due = maximum(u64{1}, clock_monotonic_milliseconds());
            publish(owner, LogoutPhase::forget_failed,
                    utf8_literal("Couldn't open sign-in. Try again."));
        }
    }
}
void stopped(ApplicationAccountLogout& owner, const AccountHostStopResult& result,
             bool publish_login) noexcept {
    // This function is called only after the kernel join has completed.
    if (owner.host_transferred) {
        if (!result.ok())
            g_app.account = owner.stop_host;
        owner.stop_host = {};
        owner.host_transferred = false;
    }
    application_account_stop_observed(result, owner.had_account);
    owner.request_id = 0;
    owner.submitted = false;
    if (!result.ok()) {
        publish(owner, LogoutPhase::stop_failed,
                utf8_literal("Couldn't finish logging out. Try again."));
        return;
    }
    owner.cleanup_joined = true;
    cleanup(owner, publish_login);
}
bool save(TextFormOwnerKey key, Utf8View) noexcept {
    auto* owner = g_app.account_logout;
    if (!owner || !owner_key(*owner, key))
        return false;
    if (owner->committed) {
        if (account_logout_stop_owned(owner->stop))
            return false;
        switch (owner->phase) {
        case LogoutPhase::stop_failed:
        case LogoutPhase::stop_start_failed:
            start_stop(*owner);
            break;
        case LogoutPhase::forget_failed:
        case LogoutPhase::startup_recovery:
            cleanup(*owner, true);
            break;
        default:
            break;
        }
        return false;
    }
    if (!source_current(*owner) ||
        (owner->phase != LogoutPhase::confirmation && owner->phase != LogoutPhase::marker_failed))
        return false;
    // Commit local intent before quiescence or the first authenticated POST.
    // Failure is still cancellable and sends no server request.
    if (!resolve_paths(*owner) || !account_credential_forget_begin(owner->paths).ok()) {
        publish(*owner, LogoutPhase::marker_failed,
                utf8_literal("Couldn't log out. Try again."));
        return false;
    }
    owner->committed = true;
    owner->unconfirmed = true;
    owner->had_account = g_app.account.instance != nullptr;
    ++g_app.login.application_auth_epoch;
    if (!g_app.login.application_auth_epoch)
        ++g_app.login.application_auth_epoch;
    g_app.login.remote_restart_at_ms = 0;
    owner->request_id = application_next_request_id();
    const u64 now = clock_monotonic_milliseconds();
    owner->deadline = deadline_after(now, kLogoutDeadlineMilliseconds);
    owner->admission_due = maximum(u64{1}, now);
    publish(*owner, LogoutPhase::admission, utf8_literal("Logging out…"));
    application_prepare_account_stop();
    // Quiescence invalidates the old account scope. From here
    // on, the accepted owner key is a pure value and no longer reads the source.
    return false;
}
ApplicationTextFormProgress progress(TextFormOwnerKey key) noexcept {
    const auto* owner = g_app.account_logout;
    if (!owner || !owner_key(*owner, key))
        return {{}, {}, 0, false, false, true, false, false};
    const bool retry =
        owner->phase == LogoutPhase::marker_failed || owner->phase == LogoutPhase::stop_start_failed ||
        owner->phase == LogoutPhase::stop_failed || owner->phase == LogoutPhase::forget_failed ||
        owner->phase == LogoutPhase::startup_recovery;
    return {owner->status,
            retry ? utf8_literal("Retry") : utf8_literal("Log out"),
            owner->revision,
            false,
            retry || owner->phase == LogoutPhase::confirmation,
            owner->phase == LogoutPhase::complete,
            !owner->committed,
            !owner->committed};
}
void closed(TextFormOwnerKey key) noexcept {
    auto* owner = g_app.account_logout;
    if (!owner || !owner_key(*owner, key))
        return;
    owner->form_bound = false;
    if (!owner->committed)
        publish(*owner, LogoutPhase::complete, {});
}
bool open_form(ApplicationAccountLogout& owner) noexcept {
    if (owner.form_bound)
        return true;
    ApplicationTextFormRequest request{};
    request.title = owner.startup ? utf8_literal("Finish logging out") : utf8_literal("Log out?");
    request.context = utf8_literal("You will need to sign in again to use this account.");
    request.accept = owner.startup ? utf8_literal("Retry") : utf8_literal("Log out");
    request.owner = owner.key;
    request.valid = form_valid;
    request.save = save;
    request.confirmation_only = true;
    request.progress = progress;
    request.closed = closed;
    request.login_owner = owner.startup;
    request.danger = !owner.startup;
    owner.form_bound = true;
    if (application_text_form_open(request)) {
        owner.form_due = 0;
        return true;
    }
    owner.form_bound = false;
    owner.form_due = deadline_after(clock_monotonic_milliseconds(), 1000);
    return false;
}
} // namespace

bool application_account_logout_available() noexcept {
    if (application_account_logout_locked())
        return false;
    return g_app.account_ui && !g_app.login.visible && g_app.account.instance &&
           !g_app.account.stop_failed && g_app.account.api.logout && g_app.account.status.profile_id &&
           g_app.account_ui->chat_state.self_user_id;
}
bool application_account_logout_open() noexcept {
    if (application_account_logout_blocked()) {
        auto* owner = g_app.account_logout;
        return owner && open_form(*owner);
    }
    if (!application_account_logout_available() || !allocate_owner())
        return false;
    auto& owner = *g_app.account_logout;
    if (owner.form_bound)
        return true;
    owner = {};
    initialize_key(owner, g_app.account.status.profile_id, g_app.account_ui->chat_state.self_user_id);
    owner.source_ui = g_app.account_ui;
    owner.source_instance = g_app.account.instance;
    owner.server = g_app.account.api.logout;
    owner.actor_scope = g_app.plugin_account_scope_serial;
    owner.phase = LogoutPhase::confirmation;
    return open_form(owner);
}
bool application_account_logout_blocked() noexcept {
    return startup_blocked || (g_app.account_logout && g_app.account_logout->committed);
}
bool application_account_logout_host_owned() noexcept {
    return g_app.account_logout && account_logout_stop_owned(g_app.account_logout->stop);
}
HANDLE application_account_logout_notification() noexcept {
    return g_app.account_logout ? account_logout_stop_notification(g_app.account_logout->stop) : nullptr;
}
u64 application_account_logout_next_wake() noexcept {
    const auto* owner = g_app.account_logout;
    if (!owner)
        return startup_blocked ? startup_retry_due : 0;
    if (account_logout_stop_owned(owner->stop))
        return 0;
    if (owner->committed && !owner->form_bound && owner->phase != LogoutPhase::complete &&
        owner->phase != LogoutPhase::server && owner->phase != LogoutPhase::admission)
        return owner->form_due;
    if (owner->phase == LogoutPhase::admission || owner->phase == LogoutPhase::server)
        return earliest_deadline(owner->deadline, owner->admission_due);
    return 0;
}
void application_account_logout_pump() noexcept {
    auto* owner = g_app.account_logout;
    if (!owner) {
        if (startup_blocked && deadline_due(clock_monotonic_milliseconds(), startup_retry_due))
            application_account_logout_prepare_startup();
        return;
    }
    if (account_logout_stop_owned(owner->stop)) {
        AccountHostStopResult result{};
        if (account_logout_stop_take(&owner->stop, &result))
            stopped(*owner, result, true);
        return;
    }
    if (!owner->committed)
        return;
    const u64 now = clock_monotonic_milliseconds();
    if (!owner->form_bound && deadline_due(now, owner->form_due))
        open_form(*owner);
    if (owner->phase == LogoutPhase::admission || owner->phase == LogoutPhase::server) {
        if (deadline_due(now, owner->deadline)) {
            cancel_server(*owner);
            start_stop(*owner);
            return;
        }
    }
    if (owner->phase == LogoutPhase::admission && deadline_due(now, owner->admission_due)) {
        const auto admitted = owner->server(owner->source_instance, AccountLogoutOperation::begin,
                                            owner->request_id, nullptr);
        if (admitted.submit_error == RestClientSubmitError::busy) {
            owner->admission_due = deadline_after(now, kLogoutAdmissionRetryMilliseconds);
            return;
        }
        if (admitted.submit_error != RestClientSubmitError::none) {
            cancel_server(*owner);
            start_stop(*owner);
            return;
        }
        owner->submitted = true;
        owner->admission_due = maximum(u64{1}, now);
        publish(*owner, LogoutPhase::server, utf8_literal("Logging out…"));
    }
    if (owner->phase == LogoutPhase::server && deadline_due(now, owner->admission_due)) {
        AccountLogoutResponse response{};
        const auto taken = owner->server(owner->source_instance, AccountLogoutOperation::take,
                                         owner->request_id, &response);
        if (!taken.consumed) {
            owner->admission_due = deadline_after(now, kLogoutAdmissionRetryMilliseconds);
            return;
        }
        owner->unconfirmed = response.request_id != owner->request_id ||
                             response.transport_error != RestClientError::none ||
                             response.rate_limited || response.rejection.captcha ||
                             response.rejection.has_code || response.http_status < 200 ||
                             response.http_status >= 300;
        // 401, 429, rejection, captcha and transport uncertainty all finish
        // local logout. No response starts another authenticated POST.
        start_stop(*owner);
    }
}
bool application_account_logout_prepare_startup(const AccountPaths* paths) noexcept {
    if (application_account_logout_host_owned() ||
        (application_account_logout_blocked() && g_app.account_logout))
        return false;
    AccountPaths resolved{};
    const bool valid = paths || account_paths_resolve(&resolved).ok();
    const auto probed =
        valid ? account_credential_forget_probe(paths ? *paths : resolved)
              : AccountCredentialForgetResult{0, AccountCredentialForgetError::probe, 0, true};
    if (probed.ok() && !probed.pending && !startup_blocked)
        return true;
    startup_blocked = true;
    if (!allocate_owner()) {
        startup_retry_due = deadline_after(clock_monotonic_milliseconds(), 1000);
        return false;
    }
    startup_retry_due = 0;
    auto& owner = *g_app.account_logout;
    owner = {};
    initialize_key(owner, 0, 0);
    if (valid) {
        owner.paths = paths ? *paths : resolved;
        owner.paths_valid = true;
    }
    owner.committed = owner.startup = owner.unconfirmed = true;
    owner.cleanup_joined = !g_app.account.instance && !g_app.account.login_instance;
    publish(owner, LogoutPhase::startup_recovery,
            utf8_literal("Couldn't finish logging out. Try again."));
    g_app.login.visible = true;
    open_form(owner);
    application_request_frame();
    return false;
}
bool application_account_logout_shutdown() noexcept {
    auto* owner = g_app.account_logout;
    if (!owner)
        return !g_app.account.instance && !g_app.account.login_instance;
    if (!owner->committed)
        return true;
    if (!account_logout_stop_owned(owner->stop) && !owner->cleanup_joined) {
        cancel_server(*owner);
        start_stop(*owner);
    }
    if (account_logout_stop_owned(owner->stop)) {
        AccountHostStopResult result{};
        if (!account_logout_stop_join_for_shutdown(&owner->stop, kAccountStopTimeoutMs, &result))
            return false;
        stopped(*owner, result, false);
        return result.ok();
    }
    if (!owner->cleanup_joined)
        return false;
    cleanup(*owner, false);
    return true;
}
void application_account_logout_release() noexcept {
    auto* owner = g_app.account_logout;
    if (!owner || account_logout_stop_owned(owner->stop))
        return;
    if (owner->committed)
        return;
    application_text_form_cancel_owner(form_valid);
    g_app.account_logout = nullptr;
    VirtualFree(owner, 0, MEM_RELEASE);
}
} // namespace maicord::app_detail
