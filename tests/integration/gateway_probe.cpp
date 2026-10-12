// Manual live-network probe: built only with -DMAICORD_BUILD_INTEGRATION_TESTS=ON
// (cmake/integration.cmake) and run by hand or through tools/probe_*.ps1; it is
// never part of the CTest suite because it talks to the real service.
#include "maicord/app/gateway_client.hpp"
#include "../test_output.hpp"

namespace maicord {
namespace {

GatewayClient g_client{};

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_gateway_probe_entry() noexcept {
    maicord::GatewayClientResult status = maicord::gateway_client_start(&maicord::g_client);
    if (status.ok()) {
        status = maicord::gateway_client_wait_hello(&maicord::g_client, 20'000);
    }
    maicord::u64 opened_ms = 0;
    maicord::u64 hello_ms = 0;
    bool saw_opened = false;
    bool saw_hello = false;
    maicord::GatewayClientEvent event{};
    while (maicord::gateway_client_pop_event(&maicord::g_client, &event)) {
        if (event.kind == maicord::GatewayClientEventKind::websocket_opened) {
            opened_ms = event.timestamp_ms;
        } else if (event.kind == maicord::GatewayClientEventKind::hello) {
            hello_ms = event.timestamp_ms;
        }
        saw_opened = saw_opened || event.kind == maicord::GatewayClientEventKind::websocket_opened;
        saw_hello = saw_hello ||
                    (event.kind == maicord::GatewayClientEventKind::hello &&
                     event.heartbeat_interval_ms != 0);
    }
    const bool ok = status.ok() && status.phase == maicord::GatewayClientPhase::hello &&
                    saw_opened && saw_hello &&
                    maicord::gateway_client_dropped_events(&maicord::g_client) == 0;
    const maicord::u64 gateway_us = status.hello_elapsed_us;

    if (ok) {
        maicord::test_write_text("PASS gateway_websocket hello_op=10 connect_us=");
        maicord::test_write_u64(gateway_us);
        maicord::test_write_text(" opened_to_hello_ms=");
        maicord::test_write_u64(hello_ms - opened_ms);
        maicord::test_write_text(" runtime_bytes=");
        maicord::test_write_u64(maicord::gateway_client_runtime_bytes());
        maicord::test_write_text("\r\n");
    } else {
        maicord::test_write_text("FAIL gateway_websocket\r\n");
    }

    if (maicord::g_client.worker.thread) {
        maicord::gateway_client_stop(&maicord::g_client, 5'000);
    }
    ExitProcess(ok ? 0 : 1);
}
