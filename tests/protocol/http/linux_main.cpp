#include "../../support/test_output.hpp"
#include "../../support/transport_test_shared.hpp"

// The HTTP, WebSocket, REST, TLS stream and network-layer fixtures of tests/protocol/http/core_transport.cpp (which the
// Windows suite runs inside its core test bundle) as one Linux test executable. Left to Windows: the Schannel fixtures
// (tls_client_bootstrap, tls_client_shared_credential, tls_client_alpn): the OpenSSL client is tested by
// maicord_tls_client and maicord_tls_smoke.

namespace maicord {
void test_websocket_frames() noexcept;
void test_websocket_handshake() noexcept;
void test_http1() noexcept;
void test_rest() noexcept;
void test_rest_transaction() noexcept;
void test_tls_stream_contract() noexcept;
void test_network_resolver() noexcept;
void test_network_worker() noexcept;
void test_network_worker_cancel_drain() noexcept;
void test_network_loopback() noexcept;

namespace {
u32 g_failures = 0;
} // namespace

void expect(bool condition, const char* name) noexcept {
    test_check(&g_failures, condition, name);
}
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_transport_entry) {
    using namespace maicord;
    test_websocket_frames();
    test_websocket_handshake();
    test_http1();
    test_rest();
    test_rest_transaction();
    test_tls_stream_contract();
    test_network_resolver();
    test_network_worker();
    test_network_worker_cancel_drain();
    test_network_loopback();
    test_exit(g_failures, "transport");
}
