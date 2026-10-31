// Manual live-network probe: built only with -DMAICORD_BUILD_INTEGRATION_TESTS=ON
// (cmake/integration.cmake) and run by hand or through tools/probe_network.ps1; it is
// never part of the CTest suite because it talks to the real service.
//
// Opens, handshakes and closes several TLS streams in a row to discord.com and
// cdn.discordapp.com and reports, per host, the first (cold) handshake, the median of
// the later ones, and how many later ones Schannel reported as reconnects of the
// first one's session (SECPKG_ATTR_SESSION_INFO, SSL_SESSION_RECONNECT).
#include "maicord/core/time.hpp"
#include "maicord/platform/tls_stream.hpp"
#include "maicord/sys/sspi.hpp"
#include "../test_output.hpp"

namespace maicord {
namespace {

inline constexpr u32 kRounds = 5;
inline constexpr u32 kEncryptedBytes = 32 * 1024;
inline constexpr u32 kHandshakeBytes = 64 * 1024;
inline constexpr u32 kRecordBytes = 20 * 1024;

TlsStream g_stream{};
NetworkLoop g_loop{};
u8 g_encrypted[kEncryptedBytes]{};
u8 g_handshake[kHandshakeBytes]{};
u8 g_record[kRecordBytes]{};

struct Sample {
    u64 handshake_microseconds;
    u64 connect_microseconds;
    bool resumed;
    u32 protocol;
};

// Connects g_stream, timing from the TCP connection to ready, and closes it again.
bool connect_once(const wchar_t* host, Sample* sample) noexcept {
    if (!tls_stream_initialize_endpoint(&g_stream, host, 443,
                                        TlsStreamBuffers{Slice<u8>{g_encrypted, kEncryptedBytes},
                                                         Slice<u8>{g_handshake, kHandshakeBytes},
                                                         Slice<u8>{g_record, kRecordBytes}}))
        return false;
    const Clock clock = clock_initialize();
    const i64 begin = clock_now();
    i64 handshake_begin = 0;
    TlsStreamEvent current = tls_stream_begin_connect(&g_stream, &g_loop);
    const u64 deadline = GetTickCount64() + 20'000;
    while (current.ok() && current.kind == TlsStreamEventKind::waiting) {
        if (GetTickCount64() >= deadline)
            return false;
        const NetworkPollResult polled = network_loop_poll(&g_loop, 500);
        if (polled.state == NetworkPollState::timeout)
            continue;
        if (polled.state != NetworkPollState::completion)
            return false;
        current = tls_stream_handle_completion(&g_stream, &g_loop, polled.completion);
        if (!handshake_begin && g_stream.state == TlsStreamState::handshake)
            handshake_begin = clock_now();
    }
    if (!current.ok() || current.kind != TlsStreamEventKind::ready || !handshake_begin)
        return false;
    const i64 end = clock_now();
    sample->handshake_microseconds = clock_microseconds(clock, handshake_begin, end);
    sample->connect_microseconds = clock_microseconds(clock, begin, end);
    CtxtHandle* const context = reinterpret_cast<CtxtHandle*>(&g_stream.tls.context);
    SecPkgContext_SessionInfo session{};
    SecPkgContext_ConnectionInfo connection{};
    if (QueryContextAttributesW(context, SECPKG_ATTR_SESSION_INFO, &session) != SEC_E_OK ||
        QueryContextAttributesW(context, SECPKG_ATTR_CONNECTION_INFO, &connection) != SEC_E_OK)
        return false;
    sample->resumed = (session.dwFlags & SSL_SESSION_RECONNECT) != 0;
    sample->protocol = connection.dwProtocol;
    tls_stream_close(&g_stream);
    return tls_stream_drained(&g_stream);
}

u64 median(u64* values, u32 count) noexcept {
    for (u32 i = 1; i < count; ++i)
        for (u32 j = i; j && values[j - 1] > values[j]; --j) {
            const u64 swap = values[j];
            values[j] = values[j - 1];
            values[j - 1] = swap;
        }
    return values[(count - 1) / 2];
}

bool measure(const wchar_t* host, const char* label) noexcept {
    Sample samples[kRounds]{};
    for (u32 round = 0; round < kRounds; ++round)
        if (!connect_once(host, &samples[round]))
            return false;
    u64 handshake[kRounds - 1], connect[kRounds - 1];
    u32 resumed = 0;
    for (u32 round = 1; round < kRounds; ++round) {
        handshake[round - 1] = samples[round].handshake_microseconds;
        connect[round - 1] = samples[round].connect_microseconds;
        resumed += samples[round].resumed ? 1u : 0u;
    }
    const char* const names[]{"cold_handshake_us=", " later_handshake_us=", " cold_connect_us=",
                              " later_connect_us=", " later_resumed=", " first_resumed=", " protocol="};
    const u64 values[]{samples[0].handshake_microseconds, median(handshake, kRounds - 1),
                       samples[0].connect_microseconds, median(connect, kRounds - 1), resumed,
                       samples[0].resumed ? 1u : 0u, samples[0].protocol};
    for (u32 i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        test_write_text(label);
        test_write_text(names[i] + (i ? 1 : 0));
        test_write_u64(values[i]);
        test_write_text(i + 1 == sizeof(values) / sizeof(values[0]) ? "\r\n" : " ");
    }
    return true;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_tls_resume_probe_entry() noexcept {
    using namespace maicord;
    bool ok = network_loop_initialize(&g_loop).ok();
    if (ok)
        test_write_text("PASS tls_resume ");
    ok = ok && measure(L"discord.com", "discord_") && measure(L"cdn.discordapp.com", "cdn_");
    if (!ok)
        test_write_text("FAIL tls_resume\r\n");
    network_loop_release(&g_loop);
    ExitProcess(ok ? 0 : 1);
}
