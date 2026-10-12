#include "gateway_self_status_test.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/self_status_settings.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_process.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures, submissions;
u8 submitted_body[kSelfStatusSettingsMaximumPatchBytes];
u32 submitted_bytes;
RestClientSubmitError submit_error;
RestMethod submitted_method;
void check(bool pass, const char* label) noexcept {
    test_check(&failures, pass, label);
}
RestClientSubmitResult capture(const RestClientRequest& request) noexcept {
    ++submissions;
    submitted_method = request.route.method;
    submitted_bytes = request.body.count;
    if (request.body.count <= sizeof(submitted_body))
        copy_bytes(submitted_body, request.body.data, request.body.count);
    check(utf8_equal({reinterpret_cast<const char*>(request.target.data), request.target.count},
                     utf8_literal("/api/v10/users/@me/settings-proto/1")),
          "self status runtime: shared primary REST request uses actual settings-proto1 endpoint");
    return {submit_error};
}
Utf8View observation(GatewayUserStatus status, char* output, bool partial = false, bool unrelated = false,
                     bool response = false, bool out_of_date = false) noexcept {
    constexpr const char* statuses[] = {"online", "idle", "dnd", "invisible"};
    constexpr u8 lengths[] = {6, 4, 3, 9};
    u8 proto[128]{};
    u32 size = 0;
    if (unrelated) {
        proto[size++] = 8;
        proto[size++] = 1;
    } else {
        const auto n = lengths[static_cast<u8>(status)];
        proto[size++] = 90;
        proto[size++] = static_cast<u8>(n + 9);
        proto[size++] = 10;
        proto[size++] = static_cast<u8>(n + 2);
        proto[size++] = 10;
        proto[size++] = n;
        copy_bytes(proto + size, statuses[static_cast<u8>(status)], n);
        size += n;
        proto[size++] = 18;
        proto[size++] = 3;
        proto[size++] = 10;
        proto[size++] = 1;
        proto[size++] = 'X';
    }
    const auto prefix = response ? utf8_literal("{\"settings\":\"")
                                 : utf8_literal("{\"settings\":{\"type\":1,\"proto\":\"");
    copy_bytes(output, prefix.data, prefix.bytes);
    const auto encoded = base64_encode({proto, size}, {output + prefix.bytes, 256 - prefix.bytes});
    u32 at = prefix.bytes + encoded.bytes;
    const auto suffix =
        response
            ? (out_of_date ? utf8_literal("\",\"out_of_date\":true}") : utf8_literal("\"}"))
            : (partial ? utf8_literal("\"},\"partial\":true}") : utf8_literal("\"},\"partial\":false}"));
    copy_bytes(output + at, suffix.data, suffix.bytes);
    at += suffix.bytes;
    return {output, at};
}
void run() noexcept {
    GatewayClient client{};
    RestClient rest{};
    char json[512]{};
    check(gateway_self_status_test_create(&client, 7, 9, capture),
          "self status runtime: isolated real backend owner creates");
    if (!client.runtime)
        return;
    auto snapshot = gateway_client_self_status_snapshot(&client);
    check(
        !snapshot.authoritative && snapshot.available && gateway_self_status_test_group_bytes(&client) == 0,
        "self status runtime: configuration seed publishes unknown and allocates no opaque settings bank");
    check(!gateway_client_self_status_submit(&client, &rest, 7, 9, SelfStatusSettingsOperation::update,
                                             GatewayUserStatus::invisible, 1, 123)
                  .ok() &&
              !submissions,
          "self status runtime: update unavailable before actual saved settings are observed");
    check(gateway_client_self_status_submit(&client, &rest, 7, 9, SelfStatusSettingsOperation::fetch,
                                            GatewayUserStatus::online, 1, 123)
                  .ok() &&
              submitted_method == RestMethod::get && !submitted_bytes,
          "self status runtime: missing READY status admits real primary-worker GET fallback");
    RestClientResponse response{};
    response.request_id = 1;
    response.status_code = 200;
    check(gateway_client_self_status_complete(
              &client, 7, 9, 1, &response, observation(GatewayUserStatus::idle, json, false, false, true)),
          "self status runtime: authenticated GET response enters real saved-status projection");
    snapshot = gateway_client_self_status_snapshot(&client);
    check(snapshot.authoritative && snapshot.available && snapshot.status == GatewayUserStatus::idle &&
              snapshot.confirmed_status == GatewayUserStatus::idle &&
              gateway_self_status_test_group_bytes(&client) > 0,
          "self status runtime: saved Idle becomes authoritative only after settings response");
    const u32 before = submissions;
    check(!gateway_client_self_status_submit(&client, &rest, 8, 9, SelfStatusSettingsOperation::update,
                                             GatewayUserStatus::online, 2, 124)
                  .ok() &&
              !gateway_client_self_status_submit(&client, &rest, 7, 10, SelfStatusSettingsOperation::update,
                                                 GatewayUserStatus::online, 2, 124)
                   .ok() &&
              submissions == before,
          "self status runtime: stale account/self cannot enter shared REST worker");
    submit_error = RestClientSubmitError::busy;
    check(!gateway_client_self_status_submit(&client, &rest, 7, 9, SelfStatusSettingsOperation::update,
                                             GatewayUserStatus::invisible, 2, 124)
                  .ok() &&
              gateway_client_self_status_snapshot(&client).status == GatewayUserStatus::idle,
          "self status runtime: rejected REST admission cannot fabricate desired status");
    submit_error = RestClientSubmitError::none;
    check(gateway_client_self_status_submit(&client, &rest, 7, 9, SelfStatusSettingsOperation::update,
                                            GatewayUserStatus::invisible, 2, 124)
                  .ok() &&
              submitted_method == RestMethod::patch,
          "self status runtime: status choice admits actual persistent settings PATCH");
    SelfStatusSettingsProjection patched{};
    u8 group[512]{};
    check(submitted_bytes > 15 &&
              self_status_settings_decode_base64(
                  {reinterpret_cast<const char*>(submitted_body + 13), submitted_bytes - 15},
                  {group, sizeof(group)}, &patched) == SelfStatusSettingsError::none &&
              patched.known && patched.status == GatewayUserStatus::invisible,
          "self status runtime: admitted body contains explicit Invisible and preserved settings group");
    snapshot = gateway_client_self_status_snapshot(&client);
    check(
        snapshot.pending && snapshot.status == GatewayUserStatus::invisible &&
            snapshot.confirmed_status == GatewayUserStatus::idle,
        "self status runtime: desired request and remotely confirmed status remain distinct while pending");
    response.request_id = 2;
    check(!gateway_client_self_status_complete(
              &client, 8, 9, 2, &response,
              observation(GatewayUserStatus::invisible, json, false, false, true)),
          "self status runtime: stale profile cannot consume matching numeric request");
    check(gateway_self_status_test_observe(&client, observation(GatewayUserStatus::dnd, json, true)) &&
              gateway_client_self_status_complete(
                  &client, 7, 9, 2, &response,
                  observation(GatewayUserStatus::invisible, json, false, false, true)) &&
              gateway_client_self_status_snapshot(&client).confirmed_status == GatewayUserStatus::dnd,
          "self status runtime: newer Gateway settings win over older full REST response");
    const auto revision = gateway_client_self_status_snapshot(&client).settings_revision;
    check(gateway_self_status_test_observe(&client,
                                           observation(GatewayUserStatus::online, json, true, true)) &&
              gateway_client_self_status_snapshot(&client).settings_revision == revision,
          "self status runtime: partial unrelated group never destroys saved status");
    HANDLE settings_notification = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(settings_notification != nullptr, "typed settings runtime: isolated notification event creates");
    client.notification_event = settings_notification;
    const auto saved_status = gateway_client_self_status_snapshot(&client);
    const auto frecency_revision = InterlockedCompareExchange64(&client.frecency_settings_revision, 0, 0);
    check(gateway_self_status_test_observe(
              &client, utf8_literal("{\"settings\":{\"type\":2,\"proto\":\"EgA=\"},\"partial\":true}")) &&
              InterlockedCompareExchange64(&client.frecency_settings_revision, 0, 0) ==
                  frecency_revision + 1 &&
              gateway_client_self_status_snapshot(&client).settings_revision ==
                  saved_status.settings_revision &&
              gateway_client_self_status_snapshot(&client).confirmed_status ==
                  saved_status.confirmed_status &&
              (!settings_notification || WaitForSingleObject(settings_notification, 0) == WAIT_OBJECT_0),
          "typed settings runtime: shipping classifier bumps Favorites revision and notifies without "
          "changing self status");
    if (settings_notification)
        ResetEvent(settings_notification);
    constexpr Utf8View malformed_settings[]{
        utf8_literal("{\"settings\":{\"type\":\"2\",\"proto\":\"EgA=\"},\"partial\":true}"),
        utf8_literal("{\"settings\":{\"type\":2},\"partial\":true}"),
        utf8_literal("{\"settings\":{\"type\":2,\"proto\":7},\"partial\":true}"),
        utf8_literal("{\"settings\":{\"type\":2,\"proto\":\"AB==\"},\"partial\":true}"),
        utf8_literal("{\"settings\":{\"type\":2,\"proto\":\"AQ==\"},\"partial\":true}"),
        utf8_literal("{\"settings\":{\"type\":2,\"type\":2,\"proto\":\"EgA=\"},\"partial\":true}"),
        utf8_literal("{\"type\":2,\"proto\":\"EgA=\",\"partial\":true}"),
        utf8_literal("{\"settings\":\"EgA=\",\"type\":2,\"proto\":\"EgA=\",\"partial\":true}"),
        utf8_literal("{\"settings\":{\"proto\":\"EgA=\"},\"partial\":true}")};
    bool malformed_rejected = true;
    for (const auto invalid : malformed_settings)
        malformed_rejected = !gateway_self_status_test_observe(&client, invalid) && malformed_rejected;
    check(malformed_rejected &&
              InterlockedCompareExchange64(&client.frecency_settings_revision, 0, 0) ==
                  frecency_revision + 1 &&
              gateway_client_self_status_snapshot(&client).settings_revision ==
                  saved_status.settings_revision &&
              (!settings_notification || WaitForSingleObject(settings_notification, 0) == WAIT_TIMEOUT),
          "typed settings runtime: missing malformed duplicate or untyped protobuf cannot bump revision or "
          "signal");
    check(!gateway_self_status_test_observe(
              &client, utf8_literal("{\"settings\":{\"type\":3,\"proto\":\"EgA=\"},\"partial\":true}")) &&
              InterlockedCompareExchange64(&client.frecency_settings_revision, 0, 0) ==
                  frecency_revision + 1,
          "typed settings runtime: unrelated settings type leaves Favorites revision unchanged");
    check(gateway_self_status_test_observe(
              &client, utf8_literal("{\"settings\":{\"type\":2,\"proto\":\"GgA=\"},\"partial\":true}")) &&
              InterlockedCompareExchange64(&client.frecency_settings_revision, 0, 0) ==
                  frecency_revision + 1 &&
              (!settings_notification || WaitForSingleObject(settings_notification, 0) == WAIT_TIMEOUT),
          "typed settings runtime: valid unrelated partial frecency group preserves Favorites revision "
          "without notification");
    check(gateway_self_status_test_observe(
              &client,
              utf8_literal("{\"settings\":{\"type\":2,\"proto\":\"CgIYBxIA\"},\"partial\":false}")) &&
              InterlockedCompareExchange64(&client.frecency_settings_revision, 0, 0) ==
                  frecency_revision + 2,
          "typed settings runtime: full typed2 versions and empty Favorites snapshot bumps exactly once");
    if (settings_notification)
        ResetEvent(settings_notification);
    check(gateway_self_status_test_observe(
              &client, utf8_literal("{\"settings\":{\"type\":2,\"proto\":\"GgA=\"},\"partial\":false}")) &&
              InterlockedCompareExchange64(&client.frecency_settings_revision, 0, 0) ==
                  frecency_revision + 3 &&
              (!settings_notification || WaitForSingleObject(settings_notification, 0) == WAIT_OBJECT_0),
          "typed settings runtime: full snapshot without Favorites invalidates cached authority and "
          "signals refresh");
    if (settings_notification)
        ResetEvent(settings_notification);
    check(
        gateway_self_status_test_observe(
            &client, utf8_literal("{\"settings\":{\"type\":2,\"proto\":\"CgIYAg==\"},\"partial\":true}")) &&
            InterlockedCompareExchange64(&client.frecency_settings_revision, 0, 0) ==
                frecency_revision + 4 &&
            (!settings_notification || WaitForSingleObject(settings_notification, 0) == WAIT_OBJECT_0),
        "typed settings runtime: Versions-only partial invalidates conditional-write freshness and "
        "signals refresh");
    client.notification_event = nullptr;
    if (settings_notification)
        CloseHandle(settings_notification);
    check(gateway_client_self_status_submit(&client, &rest, 7, 9, SelfStatusSettingsOperation::update,
                                            GatewayUserStatus::online, 3, 125)
              .ok(),
          "self status runtime: outOfDate fixture admits");
    response.request_id = 3;
    check(gateway_client_self_status_complete(
              &client, 7, 9, 3, &response,
              observation(GatewayUserStatus::online, json, false, false, true, true)) &&
              gateway_client_self_status_snapshot(&client).last_error ==
                  GatewaySelfStatusError::out_of_date &&
              gateway_client_self_status_snapshot(&client).status == GatewayUserStatus::dnd,
          "self status runtime: outOfDate response clears request and restores confirmed desired status");
    check(gateway_client_self_status_submit(&client, &rest, 7, 9, SelfStatusSettingsOperation::update,
                                            GatewayUserStatus::online, 4, 126)
              .ok(),
          "self status runtime: failedTransport fixture admits");
    response.request_id = 4;
    response.error = RestClientError::receive;
    check(gateway_client_self_status_complete(&client, 7, 9, 4, &response, {}) &&
              gateway_client_self_status_snapshot(&client).last_error ==
                  GatewaySelfStatusError::transport &&
              gateway_client_self_status_snapshot(&client).status == GatewayUserStatus::dnd,
          "self status runtime: transport failure cannot become a remote status confirmation");
    SelfCustomStatus custom{};
    check(gateway_client_custom_status_snapshot(&client, &custom) && custom.present && custom.text_bytes == 1 &&
              custom.text[0] == 'X',
          "custom status runtime: snapshot reads the observed status group");
    SelfCustomStatus away{};
    away.present = true;
    copy_bytes(away.text, "Away", 4);
    away.text_bytes = 4;
    away.expires_at_ms = 5000;
    response.error = RestClientError::none;
    check(gateway_client_custom_status_submit(&client, &rest, 7, 9, &away, 6).ok() &&
              submitted_method == RestMethod::patch &&
              !gateway_client_custom_status_submit(&client, &rest, 7, 9, &away, 7).ok() &&
              !gateway_client_self_status_submit(&client, &rest, 7, 9, SelfStatusSettingsOperation::update,
                                                 GatewayUserStatus::online, 7, 128)
                   .ok(),
          "custom status runtime: PATCH admits and holds the shared settings request slot");
    u8 proto[512]{};
    const auto decoded = base64_decode({reinterpret_cast<const char*>(submitted_body + 13), submitted_bytes - 15},
                                       {proto, sizeof(proto)});
    SelfCustomStatus sent{};
    SelfStatusSettingsProjection kept{};
    check(decoded.ok() && decoded.bytes > 2 && proto[0] == 90 &&
              self_status_settings_read_custom({proto + 2, decoded.bytes - 2}, &sent) &&
              self_custom_status_equal(sent, away) &&
              self_status_settings_decode_proto({proto, decoded.bytes}, {group, sizeof(group)}, &kept) ==
                  SelfStatusSettingsError::none &&
              kept.status == GatewayUserStatus::dnd,
          "custom status runtime: body replaces the custom status and keeps the saved status");
    response.request_id = 6;
    check(gateway_client_self_status_complete(&client, 7, 9, 6, &response,
                                              observation(GatewayUserStatus::dnd, json, false, false, true)) &&
              gateway_client_self_status_snapshot(&client).last_error == GatewaySelfStatusError::none,
          "custom status runtime: settings response completes through the shared owner");
    const auto ready = gateway_client_self_status_snapshot(&client);
    gateway_self_status_test_ready(&client, false);
    const auto disconnected = gateway_client_self_status_snapshot(&client);
    check(
        !disconnected.available && disconnected.authoritative && disconnected.revision != ready.revision,
        "self status runtime: disconnected controls become unavailable while saved desired state survives");
    const u32 disconnected_submissions = submissions;
    check(!gateway_client_self_status_submit(&client, &rest, 7, 9, SelfStatusSettingsOperation::update,
                                             GatewayUserStatus::online, 5, 127)
                  .ok() &&
              submissions == disconnected_submissions &&
              gateway_client_self_status_snapshot(&client).status == GatewayUserStatus::dnd,
          "self status runtime: reconnect phase rejects admission before primary REST or desired status "
          "mutation");
    gateway_self_status_test_ready(&client, true);
    check(gateway_client_self_status_snapshot(&client).available &&
              gateway_client_self_status_snapshot(&client).status == GatewayUserStatus::dnd,
          "self status runtime: resumed client retains authoritative desired DND");
    check(!gateway_self_status_test_observe(
              &client, utf8_literal("{\"settings\":{\"type\":1,\"proto\":\"WgA=\"},\"partial\":true}")) &&
              !gateway_client_self_status_snapshot(&client).authoritative,
          "self status runtime: present empty partial group replaces old desired status with unknown");
    check(
        gateway_self_status_test_observe(&client, observation(GatewayUserStatus::invisible, json)) &&
            gateway_client_self_status_snapshot(&client).confirmed_status == GatewayUserStatus::invisible,
        "self status runtime: full saved settings can restore authoritative Invisible after invalidation");
    check(
        !gateway_self_status_test_observe(&client,
                                          observation(GatewayUserStatus::online, json, false, true)) &&
            !gateway_client_self_status_snapshot(&client).authoritative &&
            gateway_self_status_test_group_bytes(&client) == 0,
        "self status runtime: full READY-style missing status never keeps previous account desired status");
    check(gateway_self_status_test_destroy(&client) == kSelfStatusSettingsMaximumGroupBytes &&
              !client.runtime,
          "self status runtime: cancellation retires request and wipes/releases onUse settings bank");
    response.request_id = 4;
    check(!gateway_client_self_status_complete(&client, 7, 9, 4, &response, {}),
          "self status runtime: retired account cannot accept late completion");
}
} // namespace
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_gateway_self_status_runtime_entry() noexcept {
    maicord::run();
    const bool failed = maicord::failures != 0;
    constexpr char pass[] = "PASS gateway self status runtime\n",
                   fail[] = "FAIL gateway self status runtime\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
