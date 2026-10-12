// Offline fixture over the shipping REST owner. IOCP and its mailbox are real;
// plaintext responses are supplied at the existing transport callback boundary.
// No credential, worker thread, socket, DNS lookup or live HTTP is created.
#include "../src/app/rest_client.cpp"
#include "maicord/app/account_host.hpp"
#include "maicord/app/account_bulk_message_ack.hpp"
#include <stddef.h>
#include "rest_client_probe.hpp"
#include "test_output.hpp"

namespace maicord {
namespace bulk_read_transport {
u32 failures;
void check(bool pass, const char* label) noexcept {
    test_check(&failures, pass, label);
}

void admission() noexcept {
    RestClientProbe p;
    if (!p.initialize()) {
        check(false, "bulk transport: native owner initialization");
        return;
    }
    BulkMessageAckEntry entries[]{{11, 21, 0}, {12, 22, 0}};
    check(account_bulk_message_ack_submit(&p.client, entries, 2, 501).ok(),
          "bulk transport: typed batch admitted on existing PRIMARY");
    const auto retained = p.retained();
    check(
        retained.route.route_id == kBulkMessageAckRouteId && !retained.route.major_key &&
            retained.route.major == RestMajorParameter::none &&
            retained.route.method == RestMethod::post && retained.json_body &&
            !retained.identity_response && retained.request_id == 501 &&
            utf8_equal({reinterpret_cast<const char*>(retained.target.data), retained.target.count},
                       utf8_literal("/api/v10/read-states/ack-bulk")) &&
            utf8_equal(
                {reinterpret_cast<const char*>(retained.body.data), retained.body.count},
                utf8_literal(
                    "{\"read_states\":[{\"channel_id\":\"11\",\"message_id\":\"21\",\"read_state_type\":0},"
                    "{\"channel_id\":\"12\",\"message_id\":\"22\",\"read_state_type\":0}]}")) &&
            !p.runtime->extended_body && !p.runtime->logout_request_id &&
            !account_logout_request_valid(retained),
        "bulk transport: canonical route/JSON and no logout admission exception or extra body owner");
    entries[0].message_id = 999;
    check(
        retained.body.data == p.runtime->request_body &&
            utf8_equal(
                {reinterpret_cast<const char*>(retained.body.data), retained.body.count},
                utf8_literal(
                    "{\"read_states\":[{\"channel_id\":\"11\",\"message_id\":\"21\",\"read_state_type\":0},"
                    "{\"channel_id\":\"12\",\"message_id\":\"22\",\"read_state_type\":0}]}")),
        "bulk transport: caller storage is synchronously copied before return");
    check(account_bulk_message_ack_submit(&p.client, entries, 2, 502).error ==
                  RestClientSubmitError::busy &&
              p.client.current_request_id == 501 && p.runtime->command.request_id == 501,
          "bulk transport: busy PRIMARY cannot replace in-flight batch");
    check(account_bulk_message_ack_submit(&p.client, nullptr, 0, 502).error ==
                  RestClientSubmitError::busy &&
              account_bulk_message_ack_submit(&p.client, nullptr, 0, 501).ok() && p.signal(),
          "bulk transport: cancellation is fenced by the exact request ID");
    publish_request_error(&p.client, p.runtime, RestClientError::cancelled, 0);
    RestClientResponse response{};
    u8 body[256]{};
    check(rest_client_take_response(&p.client, &response, {body, sizeof(body)}).ok() &&
              response.request_id == 501 && response.error == RestClientError::cancelled &&
              !p.client.current_request_id && !p.runtime->logout_request_id,
          "bulk transport: cancelled operation uses ordinary bounded REST receipt");
    p.client.authentication_rejected = 1;
    check(account_bulk_message_ack_submit(&p.client, entries, 2, 503).error ==
                  RestClientSubmitError::invalid_input &&
              p.client.request_state == request_state_empty,
          "bulk transport: authentication rejection is never bypassed");
    p.client.authentication_rejected = 0;
    rest_client_set_required_action(&p.client, AccountRequirement::captcha);
    check(account_bulk_message_ack_submit(&p.client, entries, 2, 503).error ==
                  RestClientSubmitError::account_restricted &&
              p.client.request_state == request_state_empty,
          "bulk transport: required-action mutation policy is unchanged");
    rest_client_set_required_action(&p.client, AccountRequirement::none);
    check(account_bulk_message_ack_submit(&p.client, entries, 2, 504).ok() && p.signal(),
          "bulk transport: valid ordinary admission before worker restriction changes");
    p.client.request_state = request_state_active;
    rest_client_set_required_action(&p.client, AccountRequirement::captcha);
    check(reject_restricted_request(&p.client, p.runtime) &&
              rest_client_take_response(&p.client, &response, {body, sizeof(body)}).ok() &&
              response.request_id == 504 && response.error == RestClientError::account_restricted,
          "bulk transport: worker pre-send restriction remains authoritative");
    rest_client_set_required_action(&p.client, AccountRequirement::none);
    entries[1].channel_id = entries[0].channel_id;
    check(account_bulk_message_ack_submit(&p.client, entries, 2, 505).error ==
                  RestClientSubmitError::invalid_input &&
              p.client.request_state == request_state_empty,
          "bulk transport: duplicate channels cannot publish a REST command");
    check(account_bulk_message_ack_submit(&p.client, entries, 0, 505).error ==
                  RestClientSubmitError::invalid_input &&
              account_bulk_message_ack_submit(&p.client, nullptr, 1, 505).error ==
                  RestClientSubmitError::invalid_input &&
              account_bulk_message_ack_submit(&p.client, entries, 101, 505).error ==
                  RestClientSubmitError::invalid_input &&
              account_bulk_message_ack_submit(&p.client, entries, 1, 0).error ==
                  RestClientSubmitError::invalid_input,
          "bulk transport: mixed null/count, oversized count and zero ticket reject");
}
void receipts() noexcept {
    constexpr u16 statuses[]{204, 401, 403, 429};
    const Utf8View bodies[]{{},
                            utf8_literal("{\"code\":40002}"),
                            utf8_literal("{\"captcha_key\":[\"challenge required\"]}"),
                            utf8_literal("{\"retry_after\":0.25,\"global\":false}")};
    for (u32 i = 0; i < 4; ++i) {
        RestClientProbe p;
        if (!p.initialize()) {
            check(false, "bulk transport: response owner initialization");
            continue;
        }
        const BulkMessageAckEntry entry{1, 2, 0};
        const u64 id = 600 + i;
        check(account_bulk_message_ack_submit(&p.client, &entry, 1, id).ok() &&
                  p.receive(statuses[i], bodies[i], i == 3),
              "bulk transport: shipping plaintext parser publishes ordinary success/denial/challenge/rate "
              "receipt");
        if (p.client.request_state != request_state_ready)
            continue;
        const auto responses = p.client.responses;
        RestClientResponse response{};
        u8 body[256]{};
        if (bodies[i].bytes) {
            check(rest_client_take_response(&p.client, &response, {body, 1}).error ==
                          RestClientTakeError::output_too_small &&
                      p.client.request_state == request_state_ready,
                  "bulk transport: short response output retains exact receipt without truncation");
        }
        check(rest_client_take_response(&p.client, &response, {body, sizeof(body)}).ok() &&
                  response.request_id == id && response.status_code == statuses[i] &&
                  response.error == RestClientError::none &&
                  utf8_equal({reinterpret_cast<const char*>(body), response.body_bytes}, bodies[i]) &&
                  zeros({p.runtime->response_body, bodies[i].bytes}) && !p.client.current_request_id,
              "bulk transport: generic receipt preserves exact metadata/body and erases worker copy");
        check((statuses[i] != 401 || (response.rejection.has_code && p.client.authentication_rejected)) &&
                  (statuses[i] != 403 || response.rejection.captcha) &&
                  (statuses[i] != 429 || (response.rate_limited && response.retry_at_ms > 1)),
              "bulk transport: refusal and exact server cooldown remain terminal metadata");
        for (u32 n = 0; n < 64; ++n)
            rest_client_status(&p.client);
        check(p.client.responses == responses && p.client.request_state == request_state_empty &&
                  !p.client.current_request_id && !p.runtime->logout_request_id,
              "bulk transport: reading terminal status never resubmits or rotates a token");
    }
}
void maximum() noexcept {
    RestClientProbe p;
    if (!p.initialize()) {
        check(false, "bulk transport: maximum owner initialization");
        return;
    }
    BulkMessageAckEntry entries[100]{};
    for (u32 i = 0; i < 100; ++i)
        entries[i] = {~u64{0} - i, ~u64{0}, 0};
    check(account_bulk_message_ack_submit(&p.client, entries, 100, 701).ok() &&
              p.runtime->command.body_bytes == kBulkMessageAckMaximumBodyBytes &&
              p.runtime->command.body_bytes <= p.runtime->request_body_capacity &&
              !p.runtime->extended_body,
          "bulk transport:100 maximal cursor rows copy within existing request storage");
    check(account_bulk_message_ack_submit(&p.client, nullptr, 0, 701).ok() && p.signal(),
          "bulk transport: maximum pending batch cancels without a new worker");
    publish_request_error(&p.client, p.runtime, RestClientError::cancelled, 0);
}
void abi() noexcept {
    static_assert(kAccountModuleApiVersion == 124 && kAccountModuleApiOrdinal == 124);
    static_assert(sizeof(AccountModuleApi) == 1160 && sizeof(AccountHost) == 1408);
    static_assert(offsetof(AccountModuleApi, bulk_message_ack) == 624);
    static_assert(offsetof(AccountModuleApi, poll) == 632);
    static_assert(sizeof(BulkMessageAckEntry) == 24);
    AccountHost host{};
    check(account_host_load(&host).ok(),
          "bulk transport: ABI94 dormant module loads without account startup");
    if (!host.module)
        return;
    auto missing = host.api;
    missing.bulk_message_ack = nullptr;
    check(!account_host_api_valid(missing), "bulk transport: typed callback is mandatory ABI tail");
    const BulkMessageAckEntry entry{1, 2, 0};
    check(host.api.bulk_message_ack(nullptr, &entry, 1, 1).error ==
                  RestClientSubmitError::invalid_input &&
              host.api.bulk_message_ack(nullptr, nullptr, 0, 1).error ==
                  RestClientSubmitError::invalid_input &&
              !host.instance && !host.login_instance,
          "bulk transport: dormant callback cannot allocate account or acquire credentials");
    const auto get_api =
        reinterpret_cast<AccountModuleGetApiFunction>(GetProcAddress(host.module, MAKEINTRESOURCEA(kAccountModuleApiOrdinal)));
    AccountModuleApi older{};
    older.bytes = 0x12345678;
    check(get_api && !get_api(90, &older) && older.bytes == 0x12345678,
          "bulk transport: previous ABI rejects without touching caller output");
    account_host_release(&host);
}
} // namespace bulk_read_transport
} // namespace maicord
extern "C" void WINAPI maicord_account_bulk_read_transport_test_entry() noexcept {
    maicord::bulk_read_transport::admission();
    maicord::bulk_read_transport::receipts();
    maicord::bulk_read_transport::maximum();
    maicord::bulk_read_transport::abi();
    ExitProcess(maicord::bulk_read_transport::failures ? 1u : 0u);
}
