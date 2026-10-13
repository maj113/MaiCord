#include "maicord/app/account_private_channel.hpp"
#include "maicord/app/account_host.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/memory.hpp"
#include <stddef.h>
#include "test_output.hpp"

namespace maicord {
namespace private_channel_account_test {
u32 failures;
struct Capture {
    RestClient* client;
    RestRouteKey route;
    u64 request_id, active_id;
    u32 submissions, cancellations, target_bytes, body_bytes;
    u8 target[kRestClientMaximumTargetBytes];
    u8 body[1024];
    bool json, safe;
} capture;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
bool target(Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(capture.target), capture.target_bytes}, expected);
}
bool body(Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(capture.body), capture.body_bytes}, expected);
}
bool route(RestMethod method, u64 major_key) noexcept {
    return capture.route.method == method && capture.route.major_key == major_key &&
           capture.route.major == (major_key ? RestMajorParameter::channel : RestMajorParameter::none);
}
PrivateChannelActionRequest action(PrivateChannelActionKind kind, u64 channel = 10, u64 user = 0) noexcept {
    PrivateChannelActionRequest request{};
    request.kind = kind;
    request.channel_id = channel;
    request.user_id = user;
    return request;
}
void transport() noexcept {
    RestClient details{};
    auto create = action(PrivateChannelActionKind::create, 0);
    create.recipients[0] = 2;
    create.recipients[1] = 3;
    create.recipient_count = 2;
    // Opening a private channel: POST /users/@me/channels {recipients:[ids]}.
    check(account_private_channel_submit(&details, &create, 101).ok() && capture.safe && capture.client == &details &&
              capture.request_id == 101 && capture.json && target(utf8_literal("/api/v9/users/@me/channels")) &&
              body(utf8_literal(R"({"recipients":["2","3"]})")) && route(RestMethod::post, 0),
          "private channel: group create posts the recipient list in the shared DM bucket");
    create.recipients[0] = 7;
    create.recipient_count = 1;
    check(account_private_channel_submit(&details, &create, 102).ok() && body(utf8_literal(R"({"recipients":["7"]})")),
          "private channel: a single recipient creates or returns the one-to-one DM");
    auto add = action(PrivateChannelActionKind::add_recipient, 10, 4);
    check(account_private_channel_submit(&details, &add, 103).ok() && capture.safe && !capture.json &&
              !capture.body_bytes && target(utf8_literal("/api/v9/channels/10/recipients/4")) &&
              route(RestMethod::put, 10),
          "private channel: add people is a bodyless PUT on the channel recipient");
    auto remove = action(PrivateChannelActionKind::remove_recipient, 10, 4);
    check(account_private_channel_submit(&details, &remove, 104).ok() && !capture.body_bytes &&
              target(utf8_literal("/api/v9/channels/10/recipients/4")) &&
              route(RestMethod::delete_method, 10),
          "private channel: remove from group is a DELETE on the same recipient");
    auto rename = action(PrivateChannelActionKind::rename);
    const auto name = utf8_literal("Weekend \"plans\" 🎉");
    copy_bytes(rename.name, name.data, name.bytes);
    rename.name_bytes = static_cast<u16>(name.bytes);
    check(account_private_channel_submit(&details, &rename, 105).ok() && capture.json &&
              target(utf8_literal("/api/v9/channels/10")) &&
              body(utf8_literal(R"({"name":"Weekend \"plans\" 🎉"})")) && route(RestMethod::patch, 10),
          "private channel: rename PATCHes an escaped name");
    rename.name_bytes = 0;
    check(account_private_channel_submit(&details, &rename, 106).ok() && body(utf8_literal(R"({"name":""})")),
          "private channel: an empty name clears the custom group name");
    auto leave = action(PrivateChannelActionKind::leave);
    check(account_private_channel_submit(&details, &leave, 107).ok() &&
              target(utf8_literal("/api/v9/channels/10?silent=false")) && !capture.body_bytes &&
              route(RestMethod::delete_method, 10),
          "private channel: leave announces the departure by default");
    leave.silent = true;
    check(account_private_channel_submit(&details, &leave, 108).ok() &&
              target(utf8_literal("/api/v9/channels/10?silent=true")),
          "private channel: silent leave sets the silent query");
    auto ring = action(PrivateChannelActionKind::ring);
    check(account_private_channel_submit(&details, &ring, 109).ok() && capture.json &&
              target(utf8_literal("/api/v9/channels/10/call/ring")) && body(utf8_literal(R"({"recipients":null})")),
          "private channel: ring without recipients rings everyone");
    auto stop = action(PrivateChannelActionKind::stop_ringing);
    check(account_private_channel_submit(&details, &stop, 110).ok() &&
              target(utf8_literal("/api/v9/channels/10/call/stop-ringing")) && body(utf8_literal("{}")),
          "private channel: decline stops ringing with an empty body");
    stop.recipients[0] = 4;
    stop.recipient_count = 1;
    check(account_private_channel_submit(&details, &stop, 111).ok() && body(utf8_literal(R"({"recipients":["4"]})")),
          "private channel: stop ringing can target listed members");

    const u32 before = capture.submissions;
    auto invalid = action(PrivateChannelActionKind::create, 0);
    bool rejected = !account_private_channel_submit(&details, &invalid, 120).ok();
    invalid.recipient_count = kPrivateChannelRecipientMaximum + 1;
    rejected = rejected && !account_private_channel_submit(&details, &invalid, 120).ok();
    invalid.recipients[0] = invalid.recipients[1] = 5;
    invalid.recipient_count = 2;
    rejected = rejected && !account_private_channel_submit(&details, &invalid, 120).ok();
    invalid.recipients[1] = 0;
    rejected = rejected && !account_private_channel_submit(&details, &invalid, 120).ok();
    const PrivateChannelActionRequest missing[]{action(PrivateChannelActionKind::add_recipient, 10, 0),
                                                action(PrivateChannelActionKind::remove_recipient, 0, 4),
                                                action(PrivateChannelActionKind::leave, 0),
                                                action(PrivateChannelActionKind::none)};
    for (const auto& request : missing)
        rejected = rejected && !account_private_channel_submit(&details, &request, 120).ok();
    char long_name[kPrivateChannelNameMaximumCodepoints + 1];
    for (auto& c : long_name)
        c = 'a';
    copy_bytes(rename.name, long_name, sizeof(long_name));
    rename.name_bytes = sizeof(long_name);
    rejected = rejected && !account_private_channel_submit(&details, &rename, 120).ok();
    rename.name_bytes = sizeof(long_name) - 1;
    const bool longest = private_channel_action_valid(rename);
    rename.name[0] = static_cast<char>(0xff);
    rejected = rejected && !account_private_channel_submit(&details, &rename, 120).ok();
    check(rejected && longest && !account_private_channel_submit(nullptr, &leave, 120).ok() &&
              !account_private_channel_submit(&details, nullptr, 120).ok() &&
              !account_private_channel_submit(&details, &leave, 0).ok() && capture.submissions == before,
          "private channel: invalid recipients, names and identities never reach transport");
    capture.active_id = 200;
    check(account_private_channel_cancel(&details, 199).error == RestClientSubmitError::busy &&
              capture.active_id == 200 && account_private_channel_cancel(&details, 200).ok() && !capture.active_id,
          "private channel: exact cancellation only");
}
void dormant_abi() noexcept {
    static_assert(sizeof(AccountModuleApi) == 1168 && sizeof(AccountHost) == 1416);
    static_assert(offsetof(AccountModuleApi, private_channel) == 760);
    AccountHost host{};
    check(account_host_load(&host).ok(), "private channel: current ABI DLL loads without credentials");
    if (!host.module)
        return;
    const auto leave = action(PrivateChannelActionKind::leave);
    check(host.api.version == 126 && host.api.bytes == 1168 && host.api.private_channel &&
              host.api.private_channel(nullptr, &leave, 1).error == RestClientSubmitError::invalid_input &&
              host.api.private_channel(nullptr, nullptr, 1).error == RestClientSubmitError::invalid_input &&
              !host.instance && !host.login_instance,
          "private channel: dormant callback creates no account or worker");
    auto missing = host.api;
    missing.private_channel = nullptr;
    check(!account_host_api_valid(missing), "private channel: new callback is mandatory");
    const auto get_api = reinterpret_cast<AccountModuleGetApiFunction>(
        GetProcAddress(host.module, MAKEINTRESOURCEA(kAccountModuleApiOrdinal)));
    AccountModuleApi previous{};
    previous.bytes = 0x12345678;
    check(get_api && !get_api(101, &previous) && previous.bytes == 0x12345678,
          "private channel: ABI101 request is rejected without touching caller memory");
    account_host_release(&host);
}
} // namespace private_channel_account_test
RestClientSubmitResult rest_client_submit(RestClient* rest,
                                          const RestClientRequest& request) noexcept {
    auto& c = private_channel_account_test::capture;
    ++c.submissions;
    c.client = rest;
    c.route = request.route;
    c.request_id = request.request_id;
    c.target_bytes = request.target.count;
    c.body_bytes = request.body.count;
    c.json = request.json_body;
    c.safe = request.target.data && c.target_bytes <= sizeof(c.target) && c.body_bytes <= sizeof(c.body) &&
             (request.body.data || !c.body_bytes) && !request.identity_response &&
             request.timeout_ms == kRestClientDefaultTimeoutMs;
    if (!c.safe)
        return {RestClientSubmitError::invalid_input};
    copy_bytes(c.target, request.target.data, c.target_bytes);
    if (c.body_bytes)
        copy_bytes(c.body, request.body.data, c.body_bytes);
    return {};
}
bool rest_client_cancel(RestClient* rest, u64 id) noexcept {
    auto& c = private_channel_account_test::capture;
    ++c.cancellations;
    c.client = rest;
    if (id != c.active_id)
        return false;
    c.active_id = 0;
    return true;
}
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_account_private_channel_test_entry() noexcept {
    maicord::private_channel_account_test::transport();
    maicord::private_channel_account_test::dormant_abi();
    ExitProcess(maicord::private_channel_account_test::failures ? 1 : 0);
}
