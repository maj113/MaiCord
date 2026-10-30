// Manual live-network probe: built only with -DMAICORD_BUILD_INTEGRATION_TESTS=ON
// (cmake/integration.cmake) and run by hand or through tools/probe_*.ps1; it is
// never part of the CTest suite because it talks to the real service.
#include "maicord/core/time.hpp"
#include "maicord/protocol/http1.hpp"
#include "probe_transport.hpp"
#include "../test_output.hpp"

namespace maicord {
namespace {

ProbeTransport g_transport{};
u8 g_http_storage[kProbeRecordBufferBytes];

bool request_http(ProbeTransport* transport) noexcept {
    constexpr Utf8View request =
        utf8_literal("HEAD / HTTP/1.1\r\nHost: discord.com\r\nConnection: close\r\n\r\n");
    if (!probe_transport_send(transport,
                              ConstSlice<u8>{reinterpret_cast<const u8*>(request.data), request.bytes})) {
        return false;
    }

    ByteQueue plaintext{};
    if (!byte_queue_initialize(&plaintext, Slice<u8>{g_http_storage, kProbeRecordBufferBytes})) {
        return false;
    }
    for (u32 round = 0; round < 64; ++round) {
        const ProbeReceiveResult received = probe_transport_receive(transport);
        if (received.state != ProbeReceiveState::plaintext ||
            !byte_queue_append(&plaintext,
                               ConstSlice<u8>{received.plaintext.data, received.plaintext.count})) {
            return false;
        }

        const HttpHeadResult parsed =
            http_parse_response_head(byte_queue_readable(plaintext), 32 * 1024, 128, nullptr, nullptr);
        if (parsed.state == HttpParseState::error) {
            return false;
        }
        if (parsed.state == HttpParseState::complete) {
            if (parsed.head.status_code >= 100 && parsed.head.status_code < 200) {
                if (!byte_queue_consume(&plaintext, parsed.head.header_bytes)) {
                    return false;
                }
                continue;
            }
            return parsed.head.status_code >= 200 && parsed.head.status_code < 400 &&
                   http_response_body_kind(parsed.head, true) == HttpBodyKind::none;
        }
    }
    return false;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_tls_probe_entry() noexcept {
    const maicord::Clock clock = maicord::clock_initialize();
    const maicord::i64 begin = maicord::clock_now();
    bool ok = maicord::probe_transport_connect(&maicord::g_transport, L"discord.com");
    const maicord::u64 handshake_us = maicord::clock_microseconds(clock, begin, maicord::clock_now());
    ok = ok && maicord::request_http(&maicord::g_transport);

    if (ok) {
        maicord::test_write_text("PASS tls_http1 handshake_us=");
        maicord::test_write_u64(handshake_us);
        maicord::test_write_text("\r\n");
    } else {
        maicord::test_write_text("FAIL tls_http1\r\n");
    }

    maicord::probe_transport_release(&maicord::g_transport);
    ExitProcess(ok ? 0 : 1);
}
