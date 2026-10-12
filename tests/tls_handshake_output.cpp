#include "maicord/platform/tls_client.hpp"

namespace maicord {
void test_tls_handshake_output(void (*check)(bool, const char*) noexcept) noexcept {
    static u8 output[256 * 1024 + 1];
    TlsClient client{};
    if (!tls_client_initialize(&client, L"discord.com").ok()) {
        check(false, "TLS output fixture acquires normal outbound credentials");
        return;
    }
    check(client.maximum_token_bytes < sizeof(output), "TLS output fixture covers provider maximum");
    if (client.maximum_token_bytes >= sizeof(output)) { tls_client_release(&client); return; }
    const u32 capacity = client.maximum_token_bytes;
    output[capacity] = 0xa5;
    const auto small = tls_client_handshake(&client, {}, {output, capacity - 1});
    check(small.error == TlsError::output_too_small && !client.context_valid,
          "TLS rejects short caller output before creating a context");
    const auto hello = tls_client_handshake(&client, {}, {output, capacity});
    check(hello.ok() && hello.state == TlsHandshakeState::need_send && hello.produced > 0 &&
          hello.produced <= capacity && output[capacity] == 0xa5,
          "TLS writes ClientHello inside caller capacity");
    if (hello.ok()) {
        const u8 partial[]{22, 3, 3, 0, 80};
        for (u32 length = 0; length <= sizeof(partial); ++length) {
            const auto step = tls_client_handshake(&client, {partial, length}, {output, capacity});
            check(step.ok() && step.state == TlsHandshakeState::need_receive &&
                  step.native_status == SEC_E_INCOMPLETE_MESSAGE && step.produced == 0 &&
                  step.consumed == 0 && output[capacity] == 0xa5,
                  "fragmented TLS input never publishes the untouched output capacity");
        }
        const u8 malformed[]{0, 0, 0, 0, 0};
        const auto failed = tls_client_handshake(&client, {malformed, sizeof(malformed)}, {output, capacity});
        check(!failed.ok() && failed.state == TlsHandshakeState::failed && failed.produced == 0 &&
              output[capacity] == 0xa5, "invalid TLS record cannot publish stale output");
    }
    tls_client_release(&client);
    check(!client.context_valid && !client.credentials_valid, "TLS caller output retains ordinary handle cleanup");
}
}
