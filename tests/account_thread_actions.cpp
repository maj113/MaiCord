#include "maicord/app/account_thread_actions.hpp"
#include "maicord/protocol/channel_overview.hpp"
#include "maicord/core/utf.hpp"
#include "test_output.hpp"

namespace maicord {
namespace thread_action_test {
u32 failures{};
struct Capture {
    RestClient* client{};
    RestRouteKey route{};
    u64 request_id{}, active_id{};
    u32 submissions{}, cancellations{}, target_bytes{}, body_bytes{};
    u8 target[kThreadActionMaximumTargetBytes]{}, body[kThreadActionMaximumBodyBytes]{};
    bool safe{}, json{}, published{};
    RestClientSubmitError error{};
} capture;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
bool exact(RestClient* rest, u64 request_id, u64 thread_id, RestMethod method,
           u64 route_id, Utf8View target, Utf8View body = {}) noexcept {
    return capture.safe && capture.client == rest && capture.request_id == request_id &&
        capture.route.method == method && capture.route.route_id == route_id &&
        capture.route.major_key == thread_id && capture.route.major == RestMajorParameter::channel &&
        utf8_equal({reinterpret_cast<const char*>(capture.target), capture.target_bytes}, target) &&
        utf8_equal({reinterpret_cast<const char*>(capture.body), capture.body_bytes}, body) &&
        capture.json == (body.bytes != 0);
}
void run() noexcept {
    RestClient rest{};
    ThreadActionRequest request{20, 30, ThreadActionOperation::fetch_self, {}};
    check(account_thread_action_submit(&rest, &request, 101).ok() &&
        exact(&rest, 101, 20, RestMethod::get, kThreadMemberRouteId,
              utf8_literal("/api/v10/channels/20/thread-members/30")),
        "thread account: GET numeric self uses exact existing owner and no body/query");
    request.operation = ThreadActionOperation::join;
    check(account_thread_action_submit(&rest, &request, 102).ok() &&
        exact(&rest, 102, 20, RestMethod::post, kThreadMemberRouteId,
              utf8_literal("/api/v10/channels/20/thread-members/@me")),
        "thread account: native user Join POST @me has no automatic unarchive or optional analytics");
    request.operation = ThreadActionOperation::leave;
    check(account_thread_action_submit(&rest, &request, 103).ok() &&
        exact(&rest, 103, 20, RestMethod::delete_method, kThreadMemberRouteId,
              utf8_literal("/api/v10/channels/20/thread-members/@me")),
        "thread account: Leave uses DELETE own membership only");
    request.operation = ThreadActionOperation::unarchive;
    check(account_thread_action_submit(&rest, &request, 104).ok() &&
        exact(&rest, 104, 20, RestMethod::patch, kChannelOverviewRouteId,
              utf8_literal("/api/v10/channels/20"), utf8_literal("{\"archived\":false}")),
        "thread account: Unarchive PATCH contains only archived=false and cannot unlock");
    request.thread_id = request.self_user_id = 999;
    check(exact(&rest, 104, 20, RestMethod::patch, kChannelOverviewRouteId,
        utf8_literal("/api/v10/channels/20"), utf8_literal("{\"archived\":false}")),
        "thread account: transport synchronously retains request bytes after caller reuse");
    request = {~u64{0}, ~u64{0}, ThreadActionOperation::fetch_self, {}};
    check(account_thread_action_submit(&rest, &request, 105).ok() &&
        exact(&rest, 105, ~u64{0}, RestMethod::get, kThreadMemberRouteId,
        utf8_literal("/api/v10/channels/18446744073709551615/thread-members/18446744073709551615")),
        "thread account: full-width IDs remain exact numeric identities");
    const u32 before_invalid = capture.submissions;
    bool rejected = account_thread_action_submit(nullptr, &request, 106).error == RestClientSubmitError::invalid_input &&
        account_thread_action_submit(&rest, nullptr, 106).error == RestClientSubmitError::invalid_input &&
        account_thread_action_submit(&rest, &request, 0).error == RestClientSubmitError::invalid_input;
    ThreadActionRequest invalid[10]{};
    for (auto& value : invalid) value = request;
    invalid[0].thread_id = 0; invalid[1].self_user_id = 0;
    invalid[2].operation = static_cast<ThreadActionOperation>(255);
    for (u32 i = 3; i < 10; ++i) invalid[i].reserved[i - 3] = 1;
    u8 destination[112]; for (auto& byte : destination) byte = 0xA5;
    for (const auto& value : invalid) {
        rejected = rejected && account_thread_action_submit(&rest, &value, 106).error == RestClientSubmitError::invalid_input &&
            !thread_action_write_target(value, {destination, sizeof(destination)}).ok() &&
            !thread_action_write_body(value, {destination, sizeof(destination)}).ok();
    }
    check(rejected && capture.submissions == before_invalid && destination[0] == 0xA5,
        "thread account: zero IDs invalid enum and each reserved byte refuse before REST/writer publication");
    check(thread_action_write_target(request, {destination, 1}).error == ThreadActionError::capacity &&
        destination[0] == 0xA5, "thread protocol: target capacity failure preserves caller output");
    request.operation = ThreadActionOperation::unarchive;
    check(thread_action_write_body(request, {destination, 1}).error == ThreadActionError::capacity &&
        destination[0] == 0xA5, "thread protocol: body capacity failure preserves caller output");
    constexpr RestClientSubmitError submit_errors[]{RestClientSubmitError::busy,
        RestClientSubmitError::signal, RestClientSubmitError::account_restricted,
        RestClientSubmitError::memory};
    for (auto error : submit_errors) {
        capture.error = error;
        const u32 before = capture.submissions;
        check(account_thread_action_submit(&rest, &request, 107).error == error && capture.submissions == before + 1,
            "thread account: admission errors propagate once without fallback method/retry");
    }
    capture.active_id = 200;
    check(account_thread_action_cancel(&rest, 199).error == RestClientSubmitError::busy &&
        capture.active_id == 200 && account_thread_action_cancel(&rest, 200).ok() && capture.active_id == 0,
        "thread account: cancellation cannot steal a different DETAILS request");
    capture.active_id = 201; capture.published = true;
    const u32 before_cancel_invalid = capture.cancellations;
    check(account_thread_action_cancel(&rest, 201).error == RestClientSubmitError::busy &&
        capture.active_id == 201 && account_thread_action_cancel(nullptr, 201).error == RestClientSubmitError::invalid_input &&
        account_thread_action_cancel(&rest, 0).error == RestClientSubmitError::invalid_input &&
        capture.cancellations == before_cancel_invalid + 1,
        "thread account: late cancel preserves published receipt and invalid cancel emits no work");

    ThreadMemberSnapshot member{};
    check(thread_member_decode(utf8_literal(
        R"({"id":"20","user_id":"30","join_timestamp":"2024-08-30T12:34:56.123456+00:00","flags":9,"future":{"value":[1,2]}})"),
        20, 30, &member).ok() && member.thread_id == 20 && member.user_id == 30 && member.flags == 9 &&
        member.join_timestamp_ms != 0 && member.fields == 15,
        "thread protocol: strict exact member receipt owns scalar metadata and skips valid unknown subtrees");
    check(thread_member_decode(utf8_literal(
        R"({"join_timestamp":"2024-08-30T12:34:56Z","flags":0})"), 20, 30, &member).ok() &&
        member.thread_id == 20 && member.user_id == 30 && member.flags == 0 && member.fields == 12,
        "thread protocol: omitted IDs bind exact request without claiming wire presence");
    const char invalid_utf8[] = {'{', static_cast<char>(0xff), '}'};
    member = {99, 98, 97, 96, 95};
    check(!thread_member_decode({invalid_utf8, sizeof(invalid_utf8)}, 20, 30, &member).ok() &&
        !thread_member_decode({}, 20, 30, &member).ok() &&
        !thread_member_decode(utf8_literal("{}"), 0, 30, &member).ok() &&
        !thread_member_decode(utf8_literal("{}"), 20, 0, &member).ok() &&
        !thread_member_decode(utf8_literal("{}"), 20, 30, nullptr).ok() &&
        member.thread_id == 99 && member.flags == 96,
        "thread protocol: invalid UTF-8 absent input and unbound actor refuse atomically");
    constexpr Utf8View invalid_members[] = {
        utf8_literal(R"({"id":"21","user_id":"30","join_timestamp":"2024-08-30T12:34:56Z","flags":0})"),
        utf8_literal(R"({"id":"20","user_id":"31","join_timestamp":"2024-08-30T12:34:56Z","flags":0})"),
        utf8_literal(R"({"id":20,"join_timestamp":"2024-08-30T12:34:56Z","flags":0})"),
        utf8_literal(R"({"id":null,"join_timestamp":"2024-08-30T12:34:56Z","flags":0})"),
        utf8_literal(R"({"id":"0","join_timestamp":"2024-08-30T12:34:56Z","flags":0})"),
        utf8_literal(R"({"id":"20","id":"20","join_timestamp":"2024-08-30T12:34:56Z","flags":0})"),
        utf8_literal(R"({"join_timestamp":"2024-02-30T12:34:56Z","flags":0})"),
        utf8_literal(R"({"join_timestamp":null,"flags":0})"),
        utf8_literal(R"({"join_timestamp":"2024-08-30T12:34:56Z"})"),
        utf8_literal(R"({"flags":0})"),
        utf8_literal(R"({"join_timestamp":"2024-08-30T12:34:56Z","flags":4294967296})"),
        utf8_literal(R"({"join_timestamp":"2024-08-30T12:34:56Z","flags":-1})"),
        utf8_literal(R"({"join_timestamp":"2024-08-30T12:34:56Z","flags":false})"),
        utf8_literal(R"({"join_timestamp":"2024-08-30T12:34:56Z","flags":0,"flags":0})"),
        utf8_literal(R"({"join_timestamp":"2024-08-30T12:34:56Z","flags":0} trailing)"),
        utf8_literal("[]"),
    };
    for (Utf8View value : invalid_members) {
        member = {99, 98, 97, 96, 95};
        check(!thread_member_decode(value, 20, 30, &member).ok() &&
            member.thread_id == 99 && member.user_id == 98 && member.join_timestamp_ms == 97 &&
            member.flags == 96 && member.fields == 95,
            "thread protocol: foreign/malformed/duplicate/incomplete receipt preserves previous snapshot");
    }
}
} // namespace thread_action_test

RestClientSubmitResult rest_client_submit(RestClient* rest,
    const RestClientRequest& request) noexcept {
    auto& capture = thread_action_test::capture;
    ++capture.submissions;
    capture.client = rest; capture.request_id = request.request_id; capture.route = request.route;
    capture.target_bytes = request.target.count; capture.body_bytes = request.body.count; capture.json = request.json_body;
    capture.safe = request.target.data && request.target.count <= sizeof(capture.target) &&
        request.body.count <= sizeof(capture.body) && (!request.body.count || request.body.data) &&
        !request.identity_response && request.timeout_ms == kRestClientDefaultTimeoutMs;
    if (!capture.safe) return {RestClientSubmitError::invalid_input};
    copy_bytes(capture.target, request.target.data, request.target.count);
    if (request.body.count) copy_bytes(capture.body, request.body.data, request.body.count);
    return {capture.error};
}
bool rest_client_cancel(RestClient* rest, u64 request_id) noexcept {
    auto& capture = thread_action_test::capture;
    ++capture.cancellations; capture.client = rest; capture.request_id = request_id;
    if (request_id != capture.active_id || capture.published) return false;
    capture.active_id = 0;
    return true;
}
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_account_thread_actions_test_entry() noexcept {
    maicord::thread_action_test::run();
    constexpr auto pass = maicord::utf8_literal("PASS thread actions protocol/account\n");
    constexpr auto fail = maicord::utf8_literal("FAIL thread actions protocol/account\n");
    const bool failed = maicord::thread_action_test::failures != 0;
    const auto result = failed ? fail : pass;
    DWORD written{}; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), result.data, result.bytes, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
