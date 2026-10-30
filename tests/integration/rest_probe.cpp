// Manual live-network probe: built only with -DMAICORD_BUILD_INTEGRATION_TESTS=ON
// (cmake/integration.cmake) and run by hand or through tools/probe_*.ps1; it is
// never part of the CTest suite because it talks to the real service.
#include "maicord/core/time.hpp"
#include "maicord/protocol/rest.hpp"
#include "maicord/core/ascii.hpp"
#include "probe_transport.hpp"
#include "../test_output.hpp"

namespace maicord {
namespace {

ProbeTransport g_transport{};
u8 g_request[512]{};
u8 g_response_input[kRestMaximumHeaderBytes]{};
u8 g_response_body[4 * 1024]{};

bool request_gateway(ProbeTransport* transport, const Clock& clock,
                     u64* elapsed_us, u32* rate_fields) noexcept {
    const RestRequestWriteResult request =
        write_rest_request_head(
            RestRequestHead{
                ConstSlice<u8>{
                    reinterpret_cast<const u8*>("/api/v10/gateway"),
                    16},
                {}, 0, RestMethod::get,
                RestAuthorization::none, false},
            Slice<u8>{g_request, 512});
    if (!request.ok()) {
        return false;
    }
    RestResponse response{};
    if (!rest_response_initialize(
            &response,
            Slice<u8>{g_response_input,
                      kRestMaximumHeaderBytes},
            Slice<u8>{g_response_body, 4 * 1024})) {
        return false;
    }

    const i64 begin = clock_now();
    if (!probe_transport_send(
            transport, ConstSlice<u8>{g_request, request.bytes})) {
        return false;
    }
    for (u32 round = 0; round < 128; ++round) {
        const ProbeReceiveResult received =
            probe_transport_receive(transport);
        RestResponseStep step{};
        if (received.state == ProbeReceiveState::plaintext) {
            step = rest_response_feed(
                &response,
                ConstSlice<u8>{received.plaintext.data,
                               received.plaintext.count});
        } else if (received.state == ProbeReceiveState::peer_closed) {
            step = rest_response_feed(
                &response, ConstSlice<u8>{nullptr, 0}, true);
        } else {
            return false;
        }
        if (!step.ok()) {
            return false;
        }
        if (step.state != RestResponseState::complete) {
            continue;
        }
        if (response.parsed_head.head.status_code != 200 ||
            !response.http.connection_reusable || step.extra_bytes != 0 ||
            response.http.body_bytes == 0) {
            return false;
        }
        // Get Gateway answers {"url": "wss://..."}.
        if (!ascii_contains_ignore_case(
                Utf8View{reinterpret_cast<const char*>(g_response_body), response.http.body_bytes},
                utf8_literal("wss://"))) {
            return false;
        }
        *elapsed_us = clock_microseconds(clock, begin, clock_now());
        *rate_fields = response.parsed_head.rate.fields;
        return true;
    }
    return false;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_rest_probe_entry() noexcept {
    const maicord::Clock clock = maicord::clock_initialize();
    bool ok = maicord::probe_transport_connect(&maicord::g_transport,
                                                L"discord.com");
    maicord::u64 first_us = 0;
    maicord::u64 reused_us = 0;
    maicord::u32 first_rate_fields = 0;
    maicord::u32 reused_rate_fields = 0;
    ok = ok && maicord::request_gateway(
                   &maicord::g_transport, clock, &first_us,
                   &first_rate_fields);
    ok = ok && maicord::request_gateway(
                   &maicord::g_transport, clock, &reused_us,
                   &reused_rate_fields);

    if (ok) {
        maicord::test_write_text("PASS rest_reuse first_us=");
        maicord::test_write_u64(first_us);
        maicord::test_write_text(" reused_us=");
        maicord::test_write_u64(reused_us);
        maicord::test_write_text(" rate_fields=");
        maicord::test_write_u64(first_rate_fields | reused_rate_fields);
        maicord::test_write_text("\r\n");
    } else {
        maicord::test_write_text("FAIL rest_reuse\r\n");
    }

    maicord::probe_transport_release(&maicord::g_transport);
    ExitProcess(ok ? 0 : 1);
}
