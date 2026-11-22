// Offline fixture over the shipping REST owner. IOCP and its mailbox are real;
// plaintext responses are supplied at the existing transport callback boundary.
// No credential, worker thread, socket, DNS lookup or live HTTP is created.
// Part of rest_client_fixtures.cpp.
#include "../../../support/rest_client_probe.hpp"
#include "maicord/app/chat/messages/account_bulk_message_ack.hpp"
#include "../../../support/test_output.hpp"

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
            retained.request_id == 501 &&
            utf8_equal({reinterpret_cast<const char*>(retained.target.data), retained.target.count},
                       utf8_literal("/api/v9/read-states/ack-bulk")) &&
            utf8_equal(
                {reinterpret_cast<const char*>(retained.body.data), retained.body.count},
                utf8_literal(
                    "{\"read_states\":[{\"channel_id\":\"11\",\"message_id\":\"21\",\"read_state_type\":0},"
                    "{\"channel_id\":\"12\",\"message_id\":\"22\",\"read_state_type\":0}]}")) &&
            !p.runtime->extended_body && !p.runtime->logout_request_id &&
            !account_logout_request_valid(retained),
        "bulk transport: canonical route/JSON and no logout admission exception or extra body owner");
    // A batch without entries is the scoped read's cancel of the one in flight.
    check(account_bulk_message_ack_submit(&p.client, nullptr, 0, 502).error ==
                  RestClientSubmitError::busy &&
              account_bulk_message_ack_submit(&p.client, nullptr, 0, 501).ok() && p.signal(),
          "bulk transport: cancellation is fenced by the exact request ID");
    publish_request_error(&p.client, p.runtime, RestClientError::cancelled, 0);
}
u32 run() noexcept {
    admission();
    return failures;
}
} // namespace bulk_read_transport
} // namespace maicord
