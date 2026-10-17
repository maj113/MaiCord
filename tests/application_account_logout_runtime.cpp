// Actual application logout controller, durable credential helper, real files,
// and real background stop/join helper. Form projection and server/host-stop
// operations are bounded injected seams; this is not a live account/UI probe.
#include "../src/app/application_account_logout.hpp"
#include "../src/app/application_account_module.hpp"
#include "../src/app/application_state.hpp"
#include "../src/app/application_text_form.hpp"
#include "maicord/app/account_credential_forget.hpp"
#include "maicord/app/account_logout_stop.hpp"
#include "maicord/core/time.hpp"
#include "test_output.hpp"

namespace maicord::logout_fixture {
AccountPaths paths{};
u64 now = 1000;
u32 failures{}, frames{}, prepares{}, observed{}, published{}, forgets{};
u32 begins{}, accepted_posts{}, cancels{}, takes{}, busy_remaining{}, shutdown_join_timeout{};
u32 allocation_attempts{};
u64 accepted_id{}, cancelled_id{};
AccountLogoutResponse response{};
app_detail::ApplicationTextFormRequest form{};
bool form_visible{}, response_ready{}, marker_failure{}, resolve_failure{}, stop_failure{};
bool allocation_failure{};
bool published_unconfirmed{}, ownership_violation{}, file_order_violation{};
HANDLE stop_entered{}, stop_allowed{};
volatile LONG stop_calls{}, stop_active{};
DWORD stop_thread_id{};
wchar_t directory[kAccountProfileMaximumPathCharacters]{};
wchar_t marker[kAccountProfileMaximumPathCharacters]{};
wchar_t profile_new[kAccountProfileMaximumPathCharacters]{};
wchar_t session_new[kAccountProfileMaximumPathCharacters]{};
void check(bool ok, const char* label) noexcept {
    test_check(&failures, ok, label);
}
bool exists(const wchar_t* path) noexcept {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}
bool append(wchar_t* target, const wchar_t* suffix) noexcept {
    u32 at{}, i{};
    while (target[at])
        ++at;
    while (suffix[i]) {
        if (at + 1 >= kAccountProfileMaximumPathCharacters)
            return false;
        target[at++] = suffix[i++];
    }
    target[at] = 0;
    return true;
}
bool copy(wchar_t* target, const wchar_t* value) noexcept {
    target[0] = 0;
    return append(target, value);
}
bool write(const wchar_t* path) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    constexpr char data[] = "offline fixture";
    DWORD written{};
    const bool ok = WriteFile(file, data, sizeof(data), &written, nullptr) && written == sizeof(data);
    return CloseHandle(file) && ok;
}
bool credentials_present() noexcept {
    return exists(paths.profile) && exists(paths.session) && exists(profile_new) && exists(session_new);
}
bool credentials_absent() noexcept {
    return !exists(paths.profile) && !exists(paths.session) && !exists(profile_new) && !exists(session_new);
}
} // namespace maicord::logout_fixture

namespace maicord {
u64 logout_fixture_now() noexcept {
    return logout_fixture::now;
}
u64 logout_fixture_unix() noexcept {
    return 1759667400000ull + logout_fixture::now;
}
void* WINAPI logout_fixture_allocate(void* address, SIZE_T bytes, DWORD allocation,
                                     DWORD protection) noexcept {
    ++logout_fixture::allocation_attempts;
    return logout_fixture::allocation_failure ? nullptr
                                              : VirtualAlloc(address, bytes, allocation, protection);
}
AccountPathsResult logout_fixture_paths(AccountPaths* output) noexcept {
    if (!output || logout_fixture::resolve_failure)
        return {ERROR_ACCESS_DENIED, AccountPathsError::environment};
    *output = logout_fixture::paths;
    return {};
}
AccountCredentialForgetResult logout_fixture_begin(const AccountPaths& paths,
                                                   const AccountCredentialForgetIo* io = nullptr) noexcept {
    if (logout_fixture::marker_failure)
        return {ERROR_ACCESS_DENIED, AccountCredentialForgetError::marker_write, account_forget_marker,
                true};
    return account_credential_forget_begin(paths, io);
}
AccountCredentialForgetResult logout_fixture_forget(
    const AccountPaths& paths, bool joined, const AccountCredentialForgetIo* io = nullptr) noexcept {
    ++logout_fixture::forgets;
    if (!joined || app_detail::application_account_logout_host_owned() ||
        ReadAcquire(&logout_fixture::stop_active))
        logout_fixture::file_order_violation = true;
    return account_credential_forget_complete_after_join(paths, joined, io);
}
AccountHostStopResult logout_fixture_stop(AccountHost* host, u32 timeout, u64 monotonic,
                                          u64 unix_time) noexcept {
    using namespace logout_fixture;
    InterlockedIncrement(&stop_calls);
    InterlockedExchange(&stop_active, 1);
    stop_thread_id = GetCurrentThreadId();
    if (host == &app_detail::g_app.account || timeout != app_detail::kAccountStopTimeoutMs || !monotonic ||
        !unix_time)
        ownership_violation = true;
    SetEvent(stop_entered);
    const DWORD allowed = WaitForSingleObject(stop_allowed, 15'000);
    AccountHostStopResult result{};
    result.account_status.profile_id = 42;
    if (stop_failure || allowed != WAIT_OBJECT_0) {
        host->stop_failed = true;
        result.error = AccountHostError::account_stop;
        result.native_error = WAIT_TIMEOUT;
    } else {
        *host = {};
    }
    InterlockedExchange(&stop_active, 0);
    return result;
}
// The worker is parked on stop_allowed, so the production join's bounded wait
// can only expire. Record the requested bound and join the real thread with a
// zero wait, which reaches the same timeout result without sleeping for it.
bool logout_fixture_join_for_shutdown(AccountLogoutStopOwner* owner, u32 timeout_ms,
                                      AccountHostStopResult* result) noexcept {
    logout_fixture::shutdown_join_timeout = timeout_ms;
    return account_logout_stop_join_for_shutdown(owner, 0, result);
}
} // namespace maicord
#define clock_monotonic_milliseconds logout_fixture_now
#define clock_unix_milliseconds logout_fixture_unix
#define account_paths_resolve logout_fixture_paths
#define account_credential_forget_begin logout_fixture_begin
#define account_credential_forget_complete_after_join logout_fixture_forget
#define account_host_stop logout_fixture_stop
#define account_logout_stop_join_for_shutdown logout_fixture_join_for_shutdown
#define VirtualAlloc logout_fixture_allocate
#include "../src/app/application_account_logout.cpp"
#undef clock_monotonic_milliseconds
#undef clock_unix_milliseconds
#undef account_paths_resolve
#undef account_credential_forget_begin
#undef account_credential_forget_complete_after_join
#undef account_host_stop
#undef account_logout_stop_join_for_shutdown
#undef VirtualAlloc

namespace maicord::app_detail {
constinit Application g_app{};
bool application_request_frame() noexcept {
    ++logout_fixture::frames;
    return true;
}
bool application_text_form_open(const ApplicationTextFormRequest& request) noexcept {
    if (logout_fixture::form_visible || !request.valid || !request.valid(request.owner))
        return false;
    logout_fixture::form = request;
    logout_fixture::form_visible = true;
    return true;
}
void application_text_form_cancel_owner(TextFormOwnerValid validator) noexcept {
    if (!logout_fixture::form_visible || logout_fixture::form.valid != validator)
        return;
    const auto request = logout_fixture::form;
    logout_fixture::form_visible = false;
    if (request.closed)
        request.closed(request.owner);
}
void application_prepare_account_stop() noexcept {
    ++logout_fixture::prepares;
    ++g_app.plugin_account_scope_serial;
}
void application_account_stop_observed(const AccountHostStopResult&, bool) noexcept {
    ++logout_fixture::observed;
    if (application_account_logout_host_owned())
        logout_fixture::ownership_violation = true;
}
bool application_account_publish_logged_out(bool unconfirmed) noexcept {
    ++logout_fixture::published;
    logout_fixture::published_unconfirmed = unconfirmed;
    if (application_account_logout_host_owned() || g_app.account.instance || g_app.account.login_instance ||
        !logout_fixture::credentials_absent() ||
        account_credential_forget_probe(logout_fixture::paths).pending)
        logout_fixture::file_order_violation = true;
    g_app.login.visible = true;
    return true;
}
} // namespace maicord::app_detail

namespace maicord::logout_fixture {
using namespace app_detail;
AccountLogoutResult WINAPI server(void* instance, AccountLogoutOperation operation, u64 id,
                                  AccountLogoutResponse* output) noexcept {
    if (ReadAcquire(&stop_active) || instance != &paths || !id)
        ownership_violation = true;
    if (operation == AccountLogoutOperation::begin) {
        ++begins;
        if (!account_credential_forget_probe(paths).pending || prepares != 1)
            file_order_violation = true;
        if (busy_remaining) {
            --busy_remaining;
            return {RestClientSubmitError::busy};
        }
        if (accepted_id) {
            ownership_violation = true;
            return {RestClientSubmitError::busy};
        }
        accepted_id = id;
        ++accepted_posts;
        return {};
    }
    if (operation == AccountLogoutOperation::cancel) {
        ++cancels;
        cancelled_id = id;
        return {RestClientSubmitError::none, RestClientTakeError::empty, true, false};
    }
    ++takes;
    if (!response_ready)
        return {RestClientSubmitError::none, RestClientTakeError::empty, false, false};
    response_ready = false;
    *output = response;
    output->request_id = id;
    return {RestClientSubmitError::none, RestClientTakeError::none, false, true};
}
bool seed() noexcept {
    check(!application_account_logout_host_owned(),
          "logout controller: scenario reset never discards owned worker");
    if (application_account_logout_host_owned())
        return false;
    application_account_logout_release();
    if (g_app.account_logout)
        return false;
    DeleteFileW(marker);
    begins = accepted_posts = cancels = takes = prepares = observed = published = forgets = busy_remaining =
        0;
    accepted_id = cancelled_id = 0;
    frames = 0;
    now = 1000;
    response = {};
    response_ready = marker_failure = resolve_failure = stop_failure = false;
    allocation_attempts = 0;
    allocation_failure = false;
    published_unconfirmed = ownership_violation = file_order_violation = false;
    form_visible = false;
    form = {};
    InterlockedExchange(&stop_calls, 0);
    InterlockedExchange(&stop_active, 0);
    ResetEvent(stop_entered);
    ResetEvent(stop_allowed);
    g_app.account = {};
    g_app.account.module = GetModuleHandleW(nullptr);
    g_app.account.instance = &paths;
    g_app.account.api.logout = server;
    g_app.account.status.profile_id = 42;
    g_app.account_ui->chat_state.self_user_id = 99;
    g_app.login.visible = false;
    g_app.login.application_auth_epoch = 7;
    g_app.login.remote_restart_at_ms = 7777;
    ++g_app.plugin_account_scope_serial;
    return write(paths.profile) && write(paths.session) && write(profile_new) && write(session_new) &&
           write(paths.drafts);
}
bool open() noexcept {
    const bool opened = application_account_logout_open();
    check(opened && form_visible && form.confirmation_only && !form.login_owner &&
              utf8_equal(form.title, utf8_literal("Log out?")),
          "logout controller: actual owner opens bounded confirmation form");
    return opened;
}
void accept() noexcept {
    if (form.save)
        form.save(form.owner, {});
}
ApplicationTextFormProgress view() noexcept {
    return form.progress ? form.progress(form.owner) : ApplicationTextFormProgress{};
}
bool wait_for_stop() noexcept {
    const bool entered = WaitForSingleObject(stop_entered, 3000) == WAIT_OBJECT_0;
    check(entered && application_account_logout_host_owned() && application_account_logout_notification() &&
              stop_thread_id != GetCurrentThreadId(),
          "logout controller: actual stop helper runs injected host operation on a distinct real thread");
    return entered;
}
bool finish_stop() noexcept {
    const HANDLE completion = application_account_logout_notification();
    SetEvent(stop_allowed);
    const bool done = completion && WaitForSingleObject(completion, 3000) == WAIT_OBJECT_0;
    check(done, "logout controller: real stop terminal signal before UI receipt publication");
    if (done)
        application_account_logout_pump();
    return done;
}
void success_receipt(u16 status = 204) noexcept {
    application_account_logout_pump();
    response = {};
    response.http_status = status;
    response_ready = true;
    now += 125;
    application_account_logout_pump();
}
bool setup_files() noexcept {
    const DWORD count = GetTempPathW(kAccountProfileMaximumPathCharacters, directory);
    if (!count || count >= kAccountProfileMaximumPathCharacters ||
        !append(directory, L"MaiCordLogoutRuntime-"))
        return false;
    wchar_t digits[20]{};
    u32 used{};
    u64 pid = GetCurrentProcessId();
    do {
        digits[used++] = static_cast<wchar_t>(L'0' + pid % 10);
        pid /= 10;
    } while (pid);
    wchar_t suffix[20]{};
    for (u32 i = 0; i < used; ++i)
        suffix[i] = digits[used - i - 1];
    if (!append(directory, suffix) || !CreateDirectoryW(directory, nullptr) ||
        !account_paths_build_slot(directory, 0, &paths).ok() || !account_paths_ensure_directories(paths).ok())
        return false;
    return copy(marker, paths.account_directory) && append(marker, L"\\active.logout") &&
           copy(profile_new, paths.profile) && append(profile_new, L".new") &&
           copy(session_new, paths.session) && append(session_new, L".new");
}
void run() noexcept {
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    stop_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    stop_allowed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(g_app.account_ui && stop_entered && stop_allowed && setup_files(),
          "logout controller: isolated real file/event fixture setup");
    if (!g_app.account_ui || !stop_entered || !stop_allowed || !exists(paths.account_directory))
        return;

    if (!seed() || !open())
        return;
    const auto cancelled_form = form;
    const auto initial = view();
    check(initial.can_cancel && initial.dismissible && initial.can_accept &&
              !application_account_logout_blocked(),
          "logout controller: confirmation remains cancellable before durable commit");
    application_text_form_cancel_owner(form.valid);
    check(!form_visible && !begins && !exists(marker) && credentials_present(),
          "logout controller: cancelling confirmation sends no POST and changes no credentials");
    if (!seed() || !open())
        return;
    check(!cancelled_form.valid(cancelled_form.owner),
          "logout controller: replaced owner rejects stale callback key");
    marker_failure = true;
    accept();
    check(!application_account_logout_blocked() && view().can_cancel && view().dismissible &&
              utf8_equal(view().accept, utf8_literal("Retry")) && !begins && !prepares &&
              credentials_present(),
          "logout controller: marker failure allows Retry and Cancel without POST or quiescence");
    marker_failure = false;
    accept();
    const auto committed = view();
    check(application_account_logout_blocked() && !committed.can_cancel && !committed.dismissible &&
              !committed.can_accept && exists(marker) && prepares == 1 &&
              g_app.login.application_auth_epoch == 8 && !g_app.login.remote_restart_at_ms,
          "logout controller: real marker precedes quiescence and commit permanently removes dismissal");
    const u64 committed_revision = committed.revision;
    for (u32 i = 0; i < 4; ++i)
        accept();
    check(!begins && view().revision == committed_revision,
          "logout controller: repeated accepted form action cannot duplicate intent");
    success_receipt();
    if (!wait_for_stop())
        return;
    check(!g_app.account.module && !g_app.account.instance && !g_app.account.login_instance &&
              !g_app.account.api.logout && g_app.account_logout->stop_host.instance == &paths &&
              g_app.account_logout->stop_host.module && !forgets && credentials_present() && exists(marker),
          "logout controller: public host is empty while private host retains live resources and "
          "credentials");
    const auto before_frames = frames;
    const auto before_takes = takes;
    const auto before_revision = view().revision;
    bool pure = true;
    for (u32 i = 0; i < 8; ++i) {
        pure = application_account_logout_blocked() && !application_account_logout_available() &&
               !application_account_logout_next_wake() && form.valid(form.owner) && pure;
        application_account_logout_pump();
        accept();
        application_account_logout_release();
    }
    check(
        pure && accepted_posts == 1 && ReadAcquire(&stop_calls) == 1 && takes == before_takes &&
            frames == before_frames && view().revision == before_revision && !forgets && !published &&
            g_app.account_logout && !ownership_violation,
        "logout controller: repeated owned polls are stable, pure, single-POST, single-stop and retain owner VM");
    const auto* retained_owner = g_app.account_logout;
    check(
        !application_account_logout_shutdown() && shutdown_join_timeout == kAccountStopTimeoutMs &&
            g_app.account_logout == retained_owner && application_account_logout_host_owned() &&
            !g_app.account.instance && credentials_present() && exists(marker) && !forgets && !published,
        "logout controller: finite cancelled shutdown join retains private host, owner VM and credentials");
    if (!finish_stop())
        return;
    check(
        !application_account_logout_blocked() && credentials_absent() && !exists(marker) &&
            exists(paths.drafts) && published == 1 && !published_unconfirmed && !file_order_violation &&
            !ownership_violation,
        "logout controller: joined success deletes exactly credentials then marker before confirmed login");

    if (!seed() || !open())
        return;
    accept();
    busy_remaining = 100;
    application_account_logout_pump();
    const auto first_wake = application_account_logout_next_wake();
    for (u32 i = 0; i < 8; ++i)
        application_account_logout_pump();
    check(begins == 1 && first_wake == 1125 && !accepted_posts,
          "logout controller: busy admission has finite 125ms retry instead of frame hot loop");
    now = 11'000;
    application_account_logout_pump();
    if (!wait_for_stop())
        return;
    check(cancels == 1 && cancelled_id && !accepted_posts && !forgets,
          "logout controller: ten-second admission deadline cancels exact ticket and always starts stop");
    if (!finish_stop())
        return;
    check(published_unconfirmed && credentials_absent(),
          "logout controller: unsent deadline ends in neutral unconfirmed local logout");

    if (!seed() || !open())
        return;
    accept();
    application_account_logout_pump();
    const auto before_pending_takes = takes;
    for (u32 i = 0; i < 8; ++i)
        application_account_logout_pump();
    check(takes == before_pending_takes && application_account_logout_next_wake() == 1125,
          "logout controller: accepted server polling is bounded instead of following unrelated UI frames");
    now = 1125;
    application_account_logout_pump();
    check(takes == before_pending_takes + 1 && application_account_logout_next_wake() == 1250,
          "logout controller: empty exact receipt schedules the next finite server poll");
    now = 11'000;
    application_account_logout_pump();
    if (!wait_for_stop())
        return;
    check(accepted_posts == 1 && cancels == 1 && cancelled_id == accepted_id && !forgets,
          "logout controller: accepted rate-wait/transport deadline covers overall request and retains "
          "files until join");
    if (!finish_stop())
        return;

    constexpr u16 rejected_statuses[]{401, 429};
    for (u16 status : rejected_statuses) {
        if (!seed() || !open())
            return;
        accept();
        success_receipt(status);
        if (!wait_for_stop())
            return;
        check(accepted_posts == 1 && !forgets,
              "logout controller: 401 and 429 terminal receipts always enter stop without replay");
        if (!finish_stop())
            return;
        check(published_unconfirmed && credentials_absent() && !application_account_logout_blocked(),
              "logout controller: server rejection still completes local sign-out after join");
    }

    if (!seed() || !open())
        return;
    const HMODULE retained_module = g_app.account.module;
    accept();
    stop_failure = true;
    success_receipt();
    if (!wait_for_stop() || !finish_stop())
        return;
    check(application_account_logout_blocked() && !application_account_logout_host_owned() &&
              g_app.account.instance == &paths && g_app.account.module == retained_module &&
              view().can_accept && !view().can_cancel && !forgets && credentials_present() &&
              exists(marker),
          "logout controller: joined failed stop restores retained public host and leaves every credential "
          "untouched");
    ResetEvent(stop_entered);
    ResetEvent(stop_allowed);
    stop_failure = false;
    accept();
    if (!wait_for_stop())
        return;
    check(accepted_posts == 1 && ReadAcquire(&stop_calls) == 2 && !g_app.account.instance,
          "logout controller: explicit stop Retry transfers retained host without another POST");
    if (!finish_stop())
        return;

    if (!seed() || !open())
        return;
    const HANDLE blocked_profile = CreateFileW(paths.profile, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(blocked_profile != INVALID_HANDLE_VALUE,
          "logout controller: real credential deletion blocker opened");
    if (blocked_profile == INVALID_HANDLE_VALUE)
        return;
    accept();
    success_receipt();
    if (!wait_for_stop() || !finish_stop()) {
        CloseHandle(blocked_profile);
        return;
    }
    check(application_account_logout_blocked() && exists(paths.profile) && exists(marker) && !published &&
              view().can_accept && !view().can_cancel,
          "logout controller: real sharing violation retains durable marker and blocks new login after "
          "successful join");
    application_account_logout_release();
    check(g_app.account_logout && application_account_logout_blocked(),
          "logout controller: release cannot drop committed failed-forget guard");
    CloseHandle(blocked_profile);
    accept();
    check(credentials_absent() && !exists(marker) && published == 1 && accepted_posts == 1 &&
              ReadAcquire(&stop_calls) == 1 && !file_order_violation,
          "logout controller: explicit forget Retry uses real files without POST or additional worker");

    if (!seed())
        return;
    g_app.account = {};
    g_app.login.visible = true;
    check(account_credential_forget_begin(paths).ok(),
          "logout controller: interrupted logout marker seeded through real durable helper");
    check(!application_account_logout_prepare_startup(&paths) && application_account_logout_blocked() &&
              form_visible && form.login_owner && !view().can_cancel && !begins &&
              !ReadAcquire(&stop_calls),
          "logout controller: restart probes marker before attachment and opens non-dismissible login "
          "recovery without POST");
    accept();
    check(
        credentials_absent() && !exists(marker) && published == 1 && !begins && !ReadAcquire(&stop_calls),
        "logout controller: explicit startup cleanup requires empty host and never replays server request");
    application_account_logout_release();
    check(application_account_logout_prepare_startup(&paths),
          "logout controller: missing marker permits subsequent startup attachment");

    if (!seed())
        return;
    g_app.account = {};
    g_app.login.visible = true;
    check(account_credential_forget_begin(paths).ok(),
          "logout controller: allocation recovery has a real pending marker");
    allocation_failure = true;
    check(
        !application_account_logout_prepare_startup(&paths) && !g_app.account_logout &&
            application_account_logout_blocked() && application_account_logout_next_wake() == 2000,
        "logout controller: startup owner allocation failure remains blocked with a finite recovery wake");
    for (u32 i = 0; i < 8; ++i)
        application_account_logout_pump();
    check(allocation_attempts == 1 && !begins && credentials_present(),
          "logout controller: startup allocation failure never spins, submits or loads an account");
    allocation_failure = false;
    now = 2000;
    application_account_logout_pump();
    check(allocation_attempts == 2 && g_app.account_logout && form_visible && form.login_owner &&
              !application_account_logout_next_wake() && application_account_logout_blocked(),
          "logout controller: scheduled allocation recovery constructs the actual login Retry owner");
    accept();
    check(
        credentials_absent() && !exists(marker) && !begins && !ReadAcquire(&stop_calls),
        "logout controller: recovered startup owner completes explicit real cleanup without server replay");

    if (!seed() || !open())
        return;
    const auto stale = form;
    ++g_app.plugin_account_scope_serial;
    accept();
    check(
        !stale.valid(stale.owner) && !begins && !exists(marker) && !application_account_logout_blocked(),
        "logout controller: changed precommit actor scope rejects stale acceptance without files or POST");
    application_text_form_cancel_owner(form.valid);
    application_account_logout_release();

    DeleteFileW(paths.profile);
    DeleteFileW(paths.session);
    DeleteFileW(profile_new);
    DeleteFileW(session_new);
    DeleteFileW(paths.drafts);
    DeleteFileW(marker);
    RemoveDirectoryW(paths.account_directory);
    RemoveDirectoryW(paths.application_directory);
    RemoveDirectoryW(directory);
    CloseHandle(stop_entered);
    CloseHandle(stop_allowed);
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
}
} // namespace maicord::logout_fixture
extern "C" __declspec(noreturn) void WINAPI maicord_application_account_logout_runtime_entry() noexcept {
    maicord::logout_fixture::run();
    ExitProcess(maicord::logout_fixture::failures ? 1 : 0);
}
