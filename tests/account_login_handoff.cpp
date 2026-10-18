// The same TU-local shipping module orchestration, with protected synthetic
// coordinator receipts and a persistence boundary that performs no file I/O.
#include "account_login_handoff.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::login_handoff_test {
bool prepare_remote(RemoteAuthClient*, HANDLE*) noexcept;
bool prepare_remote_stop_limit(RemoteAuthClient*) noexcept;
bool remote_credential_erased(const RemoteAuthClient&) noexcept;
enum class AdmissionFault : u8 { none, join, allocation };
struct Fixture {
    SecureBuffer credential;
    HANDLE notification;
    UserAuthCoordinatorResult state;
    AdmissionFault fault;
    u32 pumps, receipts, takes, saves, stops;
    bool event_ready, fail_save, fail_stop, fail_browser_stop;
    u16 stop_http_status;
    u64 stop_retry_at_ms;
} fixture{};
u32 failures{};
void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}
void require(bool value, const char* label) noexcept {
    check(value, label);
    if (!value) ExitProcess(1);
}
bool erased(const SecureBuffer& buffer) noexcept {
    if (buffer.bytes) return false;
    for (u32 i = 0; i < buffer.capacity; ++i) if (buffer.data[i]) return false;
    return true;
}
AccountLoginRuntime* new_owner() noexcept {
    auto* owner = static_cast<AccountLoginRuntime*>(VirtualAlloc(nullptr, sizeof(AccountLoginRuntime),
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (owner) {
        constexpr wchar_t path[] = L"synthetic-profile";
        copy_bytes(owner->profile_path, path, sizeof(path));
    }
    return owner;
}
void reset() noexcept {
    secure_buffer_release(&fixture.credential);
    if (fixture.notification) CloseHandle(fixture.notification);
    fixture = {};
}
bool prepare_password(AccountLoginRuntime* owner, AdmissionFault fault) noexcept {
    fixture.fault = fault; fixture.event_ready = true;
    fixture.state.phase = UserAuthCoordinatorPhase::password_pending;
    fixture.state.generation = 17;
    owner->coordinator_started = true; owner->coordinator.runtime = &fixture;
    fixture.notification = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    constexpr auto token = utf8_literal("synthetic-session");
    return fixture.notification && secure_buffer_initialize(&fixture.credential, 128).ok() &&
        secure_buffer_assign(&fixture.credential, {reinterpret_cast<const u8*>(token.data), token.bytes}).ok();
}
} // namespace maicord::login_handoff_test

namespace maicord {
UserAuthCoordinatorResult user_auth_coordinator_status(const UserAuthCoordinator*) noexcept {
    return login_handoff_test::fixture.state;
}
CaptchaChallengeSnapshot user_auth_coordinator_captcha_snapshot(const UserAuthCoordinator*) noexcept { return {}; }
UserAuthCoordinatorResult user_auth_coordinator_pump(UserAuthCoordinator*, UserAuthCoordinatorEvent* event) noexcept {
    using namespace login_handoff_test;
    ++fixture.pumps; *event = {};
    if (fixture.event_ready) {
        fixture.event_ready = false; ++fixture.receipts;
        fixture.state.phase = UserAuthCoordinatorPhase::authenticated;
        event->kind = UserAuthCoordinatorEventKind::authenticated;
        event->response.kind = UserAuthResponseKind::authenticated;
        event->response.user_id = 42; event->response.has_user_id = true;
    }
    return fixture.state;
}
UserAuthCoordinatorResult user_auth_coordinator_take_profile(UserAuthCoordinator*, u64 profile_id,
    AccountProfile* profile, u32) noexcept {
    using namespace login_handoff_test;
    ++fixture.takes;
    if (fixture.fault != AdmissionFault::none) {
        const auto fault = fixture.fault; fixture.fault = AdmissionFault::none;
        fixture.state.error = fault == AdmissionFault::join ? UserAuthCoordinatorError::stop : UserAuthCoordinatorError::memory;
        fixture.state.phase = fault == AdmissionFault::join ? UserAuthCoordinatorPhase::stopping : UserAuthCoordinatorPhase::failed;
        fixture.state.native_error = fault == AdmissionFault::join ? WAIT_TIMEOUT : ERROR_NOT_ENOUGH_MEMORY;
        return fixture.state;
    }
    if (!secure_buffer_transfer(&profile->credential, &fixture.credential)) {
        fixture.state.error = UserAuthCoordinatorError::invalid_input; return fixture.state;
    }
    profile->metadata.profile_id = profile_id;
    fixture.state.error = UserAuthCoordinatorError::none;
    fixture.state.phase = UserAuthCoordinatorPhase::session_promoted;
    return fixture.state;
}
UserAuthCoordinatorResult user_auth_coordinator_stop(UserAuthCoordinator* coordinator, u32) noexcept {
    using namespace login_handoff_test;
    ++fixture.stops;
    secure_buffer_clear(&fixture.credential);
    if (fixture.stop_http_status) {
        fixture.state.http_status = fixture.stop_http_status;
        fixture.state.retry_at_ms = fixture.stop_retry_at_ms;
    }
    fixture.state.phase =
        fixture.fail_stop ? UserAuthCoordinatorPhase::stopping : UserAuthCoordinatorPhase::stopped;
    fixture.state.error =
        fixture.fail_stop ? UserAuthCoordinatorError::stop : UserAuthCoordinatorError::none;
    if (!fixture.fail_stop) { secure_buffer_release(&fixture.credential); *coordinator = {}; }
    return fixture.state;
}
HANDLE user_auth_coordinator_notification_handle(const UserAuthCoordinator*) noexcept {
    // Keep the boundary signaled after a retained admission failure so the
    // shipping module's explicit retry policy must suppress it.
    return login_handoff_test::fixture.credential.data ? login_handoff_test::fixture.notification : nullptr;
}
BrowserSignInReceiverStatus browser_login_status(BrowserLogin* owner) noexcept { return owner->status; }
BrowserLoginPumpResult browser_login_pump(BrowserLogin*, UserAuthCoordinator*) noexcept { return {}; }
HANDLE browser_login_notification_handle(const BrowserLogin*, const UserAuthCoordinator*) noexcept { return nullptr; }
BrowserSignInReceiverStatus browser_login_stop(BrowserLogin* owner, u32) noexcept {
    if (login_handoff_test::fixture.fail_browser_stop) {
        owner->status.error = BrowserSignInReceiverError::stop;
        owner->status.phase = BrowserSignInReceiverPhase::stopping;
        owner->status.native_error = ERROR_TIMEOUT;
    } else {
        owner->receiver = {};
        owner->status = {};
    }
    return owner->status;
}
AccountProfileResult account_profile_save(const wchar_t* path, const AccountProfileMetadata& metadata,
    const SecureBuffer& credential) noexcept {
    using namespace login_handoff_test;
    ++fixture.saves;
    constexpr auto expected = utf8_literal("synthetic-session");
    check(path && metadata.profile_id == 42 && credential.locked && credential.bytes == expected.bytes &&
        bytes_equal(credential.data, expected.data, expected.bytes),
        "login handoff: shipping promotion commits exactly the protected synthetic credential");
    return {credential.bytes, fixture.fail_save ? ERROR_ACCESS_DENIED : 0u,
        fixture.fail_save ? AccountProfileError::commit : AccountProfileError::none};
}
void account_profile_release(AccountProfile* profile) noexcept {
    if (profile) { secure_buffer_release(&profile->credential); *profile = {}; }
}
// Only the ticket-owner destruction path is used by the actual QR fixture.
// Defining all REST boundaries keeps network implementations out of this test.
u32 rest_client_user_auth_runtime_bytes(u32) noexcept { return 0; }
RestClientResult rest_client_start_user_auth(RestClient*, u32) noexcept { return {}; }
RestClientSubmitResult rest_client_submit_user_auth(RestClient*, const RestUserAuthRequest&) noexcept { return {RestClientSubmitError::invalid_input}; }
RestClientTakeResult rest_client_consume_user_auth_response(RestClient*, RestClientResponse*, RestUserAuthConsumeFunction, void*) noexcept { return {0, RestClientTakeError::empty}; }
HANDLE rest_client_notification_handle(const RestClient*) noexcept { return nullptr; }
bool rest_client_cancel(RestClient*, u64) noexcept { return false; }
RestClientResult rest_client_stop(RestClient* client, u32) noexcept { *client = {}; return {}; }
RestClientResult rest_client_stop_user_auth(RestClient* client, u32 timeout_ms,
                                            RestClientResponse* receipt) noexcept {
    *receipt = {};
    return rest_client_stop(client, timeout_ms);
}
} // namespace maicord

namespace maicord::login_handoff_test {
void run() noexcept {
    for (u32 scenario = 0; scenario < 3; ++scenario) {
        reset();
        auto* pending = new_owner();
        require(pending && prepare_password(pending, AdmissionFault::none),
                "login handoff: ordinary pending owner prepares for late Stop receipt");
        check(!login_status(pending).http_status,
              "login handoff: ordinary pre-Stop snapshot has not observed late429");
        fixture.stop_http_status = 429;
        fixture.stop_retry_at_ms = scenario == 1 ? 0 : 900'000'123;
        fixture.fail_stop = scenario == 2;
        void* instance = pending;
        auto stopped = module_login_stop(&instance, 1);
        if (scenario == 2) {
            check(instance == pending && !stopped.ok() && stopped.http_status == 429 &&
                      stopped.retry_at_ms == 900'000'123 && erased(fixture.credential),
                  "login handoff: ordinary failed join retains late cooldown with erased credential");
            fixture.fail_stop = false;
            fixture.stop_http_status = 0;
            stopped = module_login_stop(&instance, 1000);
        }
        check(stopped.ok() && stopped.phase == AccountLoginPhase::stopped && !instance &&
                  stopped.http_status == 429 && stopped.retry_at_ms == (scenario == 1 ? 0 : 900'000'123) &&
                  !fixture.pumps && !fixture.receipts && !fixture.takes && !fixture.saves,
              "login handoff: late ordinary429 survives complete Stop without profile adoption, decoding "
              "or replay");
    }
    reset();
    auto* limited_remote = new_owner();
    require(limited_remote != nullptr,
            "login handoff: rate-limited QR status fixture allocates without network");
    limited_remote->remote_mode = true;
    limited_remote->remote_status.phase = RemoteAuthClientPhase::failed;
    limited_remote->remote_status.error = RemoteAuthClientError::ticket_response;
    limited_remote->remote_status.http_status = 429;
    limited_remote->remote_status.retry_at_ms = 900'000'123;
    const auto limited_status = login_status(limited_remote);
    check(limited_status.http_status == 429 && limited_status.retry_at_ms == 900'000'123 &&
        limited_status.error == AccountLoginError::remote_auth,
        "login handoff: existing public login status preserves terminal QR server retry deadline");
    limited_remote->remote_mode = false;
    check(login_status(limited_remote).retry_at_ms == 0,
        "login handoff: inactive QR retry metadata cannot replace ordinary coordinator status");
    VirtualFree(limited_remote, 0, MEM_RELEASE);
    reset();
    auto* late_remote = new_owner();
    require(late_remote && prepare_remote_stop_limit(&late_remote->remote_auth),
        "login handoff: late rate-limit worker waits for real stop without network");
    late_remote->remote_mode = late_remote->remote_active = true;
    check(login_status(late_remote).http_status == 0 &&
        remote_auth_client_status(&late_remote->remote_auth).http_status == 0 &&
        !remote_credential_erased(late_remote->remote_auth),
        "login handoff: pre-stop snapshot cannot observe the pending rate-limit receipt");
    void* late_instance = late_remote;
    const auto late_stopped = module_login_stop(&late_instance, 1000);
    check(late_stopped.ok() && late_stopped.phase == AccountLoginPhase::stopped && !late_instance &&
        late_stopped.http_status == 429 && late_stopped.retry_at_ms == 900'000'321 &&
        !late_stopped.profile_saved && !late_stopped.user_id && !fixture.takes && !fixture.saves,
        "login handoff: a 429 published during join survives owner erasure without credential adoption or stale actor identity");
    constexpr AdmissionFault faults[]{AdmissionFault::join, AdmissionFault::allocation};
    for (AdmissionFault fault : faults) {
        reset(); auto* owner = new_owner();
        require(owner && prepare_password(owner, fault), "login handoff: protected password receipt fixture prepares");
        auto status = module_login_pump(owner);
        check(status.phase == AccountLoginPhase::authenticated && !status.profile_saved &&
            status.error == AccountLoginError::profile && owner->authenticated_pending &&
            fixture.credential.locked && fixture.credential.bytes && fixture.receipts == 1 && fixture.takes == 1 &&
            !fixture.saves && !module_login_notification_handle(owner),
            "login handoff: join/allocation fault preserves authenticated ownership and exposes explicit profile retry");
        status = module_login_pump(owner);
        check(status.ok() && status.profile_saved && !owner->authenticated_pending && !fixture.credential.data &&
            status.event == UserAuthCoordinatorEventKind::none &&
            fixture.pumps == 1 && fixture.receipts == 1 && fixture.takes == 2 && fixture.saves == 1,
            "login handoff: retry adopts password credential without another auth receipt or submission");
        module_login_pump(owner);
        check(fixture.takes == 2 && fixture.saves == 1, "login handoff: saved password profile adoption stays one shot");
        void* instance = owner;
        check(module_login_stop(&instance, 1000).phase == AccountLoginPhase::stopped && !instance,
              "login handoff: completed password owner releases");
    }
    reset(); auto* owner = new_owner();
    require(owner && prepare_password(owner, AdmissionFault::allocation), "login handoff: cancellation quarantine fixture prepares");
    module_login_pump(owner); fixture.fail_stop = true;
    void* instance = owner;
    check(!module_login_stop(&instance, 1).ok() && instance == owner && !owner->authenticated_pending &&
        erased(fixture.credential) && !account_profile_promotion_has_credential(owner->promotion),
        "login handoff: failed stop clears deferred admission and erases protected credentials while retaining owner");
    const auto attempts = fixture.takes; module_login_pump(owner);
    check(fixture.takes == attempts && !fixture.saves, "login handoff: cancelled quarantined owner cannot revive profile adoption");
    fixture.fail_stop = false;
    check(module_login_stop(&instance, 1000).ok() && !instance, "login handoff: quarantine stop retry releases owner");

    reset(); owner = new_owner();
    require(owner && prepare_password(owner, AdmissionFault::none), "login handoff: staged profile save fixture prepares");
    fixture.fail_save = true;
    auto saved_status = module_login_pump(owner);
    check(saved_status.error == AccountLoginError::profile && !saved_status.profile_saved &&
        !owner->authenticated_pending && account_profile_promotion_has_credential(owner->promotion) &&
        !fixture.credential.data && fixture.takes == 1 && fixture.saves == 1 && !module_login_notification_handle(owner),
        "login handoff: staged save fault retains one protected promotion without a signaled retry loop");
    fixture.fail_save = false; saved_status = module_login_pump(owner);
    check(saved_status.ok() && saved_status.profile_saved && !account_profile_promotion_has_credential(owner->promotion) &&
        fixture.takes == 1 && fixture.pumps == 1 && fixture.saves == 2,
        "login handoff: explicit staged save retry commits without taking or authenticating again");
    instance = owner;
    check(module_login_stop(&instance, 1000).ok() && !instance, "login handoff: saved promotion owner releases");

    reset(); owner = new_owner();
    require(owner && prepare_password(owner, AdmissionFault::none), "login handoff: staged cancellation fixture prepares");
    fixture.fail_save = true; module_login_pump(owner); fixture.fail_stop = true; instance = owner;
    check(!module_login_stop(&instance, 1).ok() && instance == owner && !owner->authenticated_pending &&
        !account_profile_promotion_has_credential(owner->promotion) && !owner->promotion.profile.credential.data,
        "login handoff: quarantined stop discards staged promotion before waiting for native owners");
    fixture.fail_stop = false;
    check(module_login_stop(&instance, 1000).ok() && !instance, "login handoff: staged cancellation stop retry releases");

    reset(); owner = new_owner();
    require(owner && prepare_password(owner, AdmissionFault::allocation), "login handoff: browser quarantine fixture prepares");
    owner->browser_mode = true; owner->browser.authenticated = true;
    owner->browser.receiver.runtime = &fixture; owner->browser.status.phase = BrowserSignInReceiverPhase::complete;
    owner->user_id = 42;
    module_login_pump(owner);
    fixture.fail_browser_stop = fixture.fail_stop = true; instance = owner;
    auto stopped = module_login_stop(&instance, 1);
    check(stopped.error == AccountLoginError::stop && stopped.native_error == ERROR_TIMEOUT &&
        instance == owner && owner->browser.receiver.runtime == &fixture && owner->coordinator_started &&
        !owner->authenticated_pending && erased(fixture.credential) && fixture.stops == 1,
        "login handoff: browser join failure still erases independent coordinator credentials and retains first diagnostic");
    const auto browser_takes = fixture.takes; module_login_pump(owner);
    check(fixture.takes == browser_takes && !fixture.saves,
        "login handoff: retained browser receipt cannot revive cancelled credential adoption");
    fixture.fail_stop = false;
    stopped = module_login_stop(&instance, 1);
    check(!stopped.ok() && stopped.native_error == ERROR_TIMEOUT && instance == owner &&
        !owner->coordinator_started && !fixture.credential.data && owner->browser.receiver.runtime == &fixture,
        "login handoff: independent coordinator can release while browser address stays quarantined");
    fixture.fail_browser_stop = false;
    check(module_login_stop(&instance, 1000).ok() && !instance,
        "login handoff: browser quarantine stop retry releases complete owner");

    reset(); owner = new_owner(); HANDLE release{};
    require(owner && prepare_remote(&owner->remote_auth, &release), "login handoff: actual QR owner with held terminal thread prepares");
    owner->remote_mode = owner->remote_active = true;
    auto status = module_login_pump(owner);
    const HANDLE terminal = module_login_notification_handle(owner);
    check(status.ok() && !status.profile_saved && owner->authenticated_pending && !fixture.saves &&
        terminal == owner->remote_auth.thread && WaitForSingleObject(terminal, 0) == WAIT_TIMEOUT &&
        !remote_credential_erased(owner->remote_auth),
        "login handoff: early QR event preserves credential and waits on worker terminal handle without rejecting auth");
    const auto repeated = module_login_pump(owner);
    check(repeated.ok() && repeated.remote_event == RemoteAuthClientEventKind::none &&
        owner->authenticated_pending && !fixture.saves && module_login_notification_handle(owner) == terminal,
        "login handoff: consumed QR event does not lose authenticated readiness ownership");
    SetEvent(release);
    check(WaitForSingleObject(terminal, 3000) == WAIT_OBJECT_0, "login handoff: retained notification wakes exactly on terminal worker readiness");
    status = module_login_pump(owner);
    check(status.ok() && status.profile_saved && !owner->authenticated_pending && !owner->remote_auth.runtime &&
        status.remote_event == RemoteAuthClientEventKind::none && fixture.saves == 1,
        "login handoff: joined QR thread adopts once without another completion event");
    CloseHandle(release); instance = owner;
    check(module_login_stop(&instance, 1000).ok() && !instance, "login handoff: completed QR owner releases");

    reset(); owner = new_owner(); release = nullptr;
    require(owner && prepare_remote(&owner->remote_auth, &release), "login handoff: held QR cancellation fixture prepares");
    owner->remote_mode = owner->remote_active = true;
    module_login_pump(owner); instance = owner;
    check(!module_login_stop(&instance, 1).ok() && instance == owner && !owner->authenticated_pending &&
        remote_credential_erased(owner->remote_auth) && !fixture.saves,
        "login handoff: cancel while QR worker exits erases credential and defers no profile commit");
    SetEvent(release);
    check(WaitForSingleObject(owner->remote_auth.thread, 3000) == WAIT_OBJECT_0 &&
        module_login_stop(&instance, 1000).ok() && !instance && !fixture.saves,
        "login handoff: QR stop retry releases quarantine without adopting late authentication");
    CloseHandle(release); reset();
}
} // namespace maicord::login_handoff_test
extern "C" __declspec(noreturn) void WINAPI maicord_account_login_handoff_entry() noexcept {
    maicord::login_handoff_test::run();
    ExitProcess(maicord::login_handoff_test::failures ? 1 : 0);
}
