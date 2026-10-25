#include "application_self_status.hpp"
#include "application_state.hpp"
#include "maicord/core/time.hpp"
#include "maicord/app/gateway_client.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::self_status_runtime_test {
using namespace app_detail;
GatewayClientSelfStatusSnapshot snapshot;
RestClientSubmitError admission;
GatewaySelfStatusError completion_error;
SelfStatusSettingsOperation submitted_operation;
GatewayUserStatus submitted_status;
u64 submitted_id;
u32 failures, submissions, completions;
void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}
GatewayClientSelfStatusSnapshot WINAPI get(void*) noexcept {
    return snapshot;
}
RestClientSubmitResult WINAPI submit(void*, u64 profile, u64 self, SelfStatusSettingsOperation operation,
                                     GatewayUserStatus status, u64 id, u64 unix_ms) noexcept {
    check(profile == snapshot.profile_id && self == snapshot.self_user_id && id && unix_ms,
          "self status frontend: submit keeps exact account, identity and creation timestamp");
    if (admission != RestClientSubmitError::none)
        return {admission};
    ++submissions;
    submitted_operation = operation;
    submitted_status = status;
    submitted_id = id;
    return {};
}
bool WINAPI complete(void*, u64 profile, u64 self, u64 id, const RestClientResponse*, Utf8View) noexcept {
    check(profile == snapshot.profile_id && self == snapshot.self_user_id && id == submitted_id,
          "self status frontend: completion keeps exact submitted scope and request");
    ++completions;
    snapshot.last_error = completion_error;
    if (completion_error == GatewaySelfStatusError::none) {
        snapshot.authoritative = true;
        if (submitted_operation == SelfStatusSettingsOperation::update)
            snapshot.status = snapshot.confirmed_status = submitted_status;
        ++snapshot.revision;
    }
    return true; // A consumed response is separate from a successful mutation.
}
RestClientResponse response(u64 id, u16 status = 200) noexcept {
    RestClientResponse value{};
    value.request_id = id;
    value.status_code = status;
    return value;
}
void run() noexcept {
    g_app.account.instance = reinterpret_cast<void*>(1);
    g_app.account.api.self_status_snapshot = get;
    g_app.account.api.self_status_submit = submit;
    g_app.account.api.self_status_complete = complete;
    snapshot.profile_id = 4;
    snapshot.self_user_id = 7;
    snapshot.available = true;
    snapshot.status = snapshot.confirmed_status = GatewayUserStatus::invisible;
    application_self_status_synchronize();
    check(application_self_status_pending(1) && application_self_status_next_wake() == 1 &&
              application_self_status_submit(101, 1) &&
              submitted_operation == SelfStatusSettingsOperation::fetch,
          "self status frontend: unknown saved status uses one existing-lane GET");
    check(!application_self_status_next_wake() && !application_self_status_pending(1),
          "self status frontend: an in-flight fetch has no independent polling deadline");
    check(application_self_status_response(response(101), {}) && snapshot.authoritative &&
              !application_self_status_next_wake(),
          "self status frontend: accepted saved settings settle the initial fetch");
    const u32 before = submissions;
    check(application_self_status_request(GatewayUserStatus::invisible) && submissions == before &&
              !application_self_status_pending(1),
          "self status frontend: choosing already confirmed status preserves age and avoids a PATCH");
    check(application_self_status_request(GatewayUserStatus::idle) &&
              application_self_status_submit(102, 1),
          "self status frontend: changed status enters the existing primary lane");
    const u64 first_revision = application_self_status_snapshot().revision;
    check(application_self_status_request(GatewayUserStatus::dnd) &&
              application_self_status_snapshot().pending &&
              application_self_status_snapshot().status == GatewayUserStatus::dnd &&
              application_self_status_snapshot().revision != first_revision &&
              !application_self_status_submit(103, 1),
          "self status frontend: a newer choice is visible and fenced without overwriting the in-flight "
          "ticket");
    const u64 queued_revision = application_self_status_snapshot().revision;
    check(application_self_status_request(GatewayUserStatus::dnd) &&
              application_self_status_snapshot().revision == queued_revision,
          "self status frontend: repeated queued choice does not create a redundant followup update");
    check(application_self_status_response(response(999), {}) && g_app.self_status.request_id == 102,
          "self status frontend: unrelated completion cannot retire the submitted update");
    check(application_self_status_response(response(102), {}) && application_self_status_submit(103, 1) &&
              submitted_status == GatewayUserStatus::dnd &&
              application_self_status_response(response(103), {}) && !application_self_status_next_wake(),
          "self status frontend: exact completion admits the newest choice and settles it");
    check(
        application_self_status_request(GatewayUserStatus::idle) &&
            application_self_status_submit(111, 1) &&
            application_self_status_request(GatewayUserStatus::dnd),
        "self status frontend: reconnect fixture queues a newer choice behind the exact in-flight ticket");
    const u32 disconnected_completions = completions;
    snapshot.available = false;
    check(application_self_status_response(response(111), {}) &&
              completions == disconnected_completions + 1 && !g_app.self_status.request_id &&
              g_app.self_status.queued && g_app.self_status.desired == GatewayUserStatus::dnd &&
              !application_self_status_pending(1) && !application_self_status_next_wake(),
          "self status frontend: same-account disconnected completion drains backend and retains newer "
          "desire without a wake");
    application_self_status_synchronize();
    check(g_app.self_status.queued && g_app.self_status.desired == GatewayUserStatus::dnd,
          "self status frontend: disconnected synchronization cannot discard the retained choice");
    snapshot.available = true;
    check(application_self_status_pending(1) && application_self_status_submit(112, 1) &&
              submitted_status == GatewayUserStatus::dnd &&
              application_self_status_response(response(112), {}) && !application_self_status_next_wake(),
          "self status frontend: reconnect admits the retained newest choice exactly once");
    completion_error = GatewaySelfStatusError::out_of_date;
    check(application_self_status_request(GatewayUserStatus::online) &&
              application_self_status_submit(104, 1) &&
              application_self_status_response(response(104), {}) &&
              application_self_status_submit(105, 1) &&
              submitted_operation == SelfStatusSettingsOperation::fetch,
          "self status frontend: out-of-date settings reload instead of replaying a stale group");
    completion_error = GatewaySelfStatusError::none;
    check(application_self_status_response(response(105), {}) && application_self_status_submit(106, 1) &&
              submitted_operation == SelfStatusSettingsOperation::update &&
              application_self_status_response(response(106), {}),
          "self status frontend: a confirmed reload admits the retained desired status");
    completion_error = GatewaySelfStatusError::rejected;
    check(
        application_self_status_request(GatewayUserStatus::idle) &&
            application_self_status_submit(107, 1) &&
            application_self_status_response(response(107, 403), {}) && !g_app.self_status.queued &&
            !application_self_status_next_wake(),
        "self status frontend: consuming a rejected response cannot claim a saved status or retry forever");
    completion_error = GatewaySelfStatusError::none;
    admission = RestClientSubmitError::busy;
    check(application_self_status_request(GatewayUserStatus::idle) &&
              !application_self_status_submit(108, 1) && !application_self_status_next_wake(),
          "self status frontend: busy admission leaves no zero-deadline spin");
    admission = RestClientSubmitError::none;
    check(application_self_status_submit(109, 1),
          "self status frontend: the normal primary-lane continuation can retry admission");
    auto limited = response(109, 429);
    limited.rate_limited = true;
    limited.retry_at_ms = clock_monotonic_milliseconds() + 5000;
    completion_error = GatewaySelfStatusError::rejected;
    check(application_self_status_response(limited, {}) &&
              application_self_status_next_wake() == limited.retry_at_ms &&
              !application_self_status_pending(limited.retry_at_ms - 1),
          "self status frontend: an authoritative rate deadline is retained without early retry");
    completion_error = GatewaySelfStatusError::none;
    check(application_self_status_submit(110, limited.retry_at_ms),
          "self status frontend: the exact rate deadline admits the pending choice");
    const u32 prior_completions = completions;
    ++snapshot.profile_id;
    check(application_self_status_response(response(110), {}) && completions == prior_completions &&
              !g_app.self_status.request_id && !application_self_status_next_wake(),
          "self status frontend: changed account drains the ticket without submitting to the new identity");
    application_self_status_clear();
    g_app.account = {};
}
} // namespace maicord::self_status_runtime_test
extern "C" __declspec(noreturn) void WINAPI maicord_self_status_runtime_entry() noexcept {
    maicord::self_status_runtime_test::run();
    const bool failed = maicord::self_status_runtime_test::failures != 0;
    constexpr char passed[] = "PASS self status frontend\n", failure[] = "FAIL self status frontend\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? failure : passed, sizeof(passed) - 1, &written,
              nullptr);
    ExitProcess(failed ? 1 : 0);
}
