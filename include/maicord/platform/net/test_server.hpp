#pragma once

#include "maicord/core/base.hpp"

// The two environment switches (include/maicord/core/test_hook.hpp) that point a Windows build with test hooks
// (tools/build.ps1 -TestHooks) at tools/fake_discord_server.py, as the Linux harness does with a private hosts file and
// SSL_CERT_FILE. A shipped client has neither: none of this exists without MAICORD_TEST_HOOKS, and the code that reads
// the switches (network.cpp, tls_client.cpp) compiles away.
//
//   MAICORD_TEST_SERVER=HOST:PORT    Every name lookup (network_resolver_start) answers with that one address and port,
//                                    whatever it asked for: the API, the gateway, the CDN names, the catalog host. A
//                                    malformed value fails the lookup instead of falling back to the real network.
//                                    TLS still names the real host (server name, certificate name, Host header), so the
//                                    server's certificate must cover those names. HOST is a name or an address, an IPv6
//                                    address in brackets: 127.0.0.1:8443, [::1]:8443.
//   MAICORD_TEST_CA_FILE=PATH        A file of PEM certificates (a DER certificate also does). Schannel's own validation
//                                    is replaced by chain building that trusts only these: a private chain engine whose
//                                    exclusive root is an in-memory store holding the file's certificates, then the SSL
//                                    chain policy for the name the connection asked for. The system and user certificate
//                                    stores are neither read nor changed, and no other certificate is accepted.
#if defined(MAICORD_TEST_HOOKS)

#include "maicord/sys/crypto.hpp"

namespace maicord {

inline constexpr u32 kTestServerTextCapacity = 280;
inline constexpr u32 kTestServerHostCapacity = 256;

struct TestServerEndpoint {
    wchar_t host[kTestServerHostCapacity];
    wchar_t service[6];
};

// "HOST:PORT" (the port 1 to 65535) into *endpoint; false, leaving it untouched, for anything else.
bool test_server_parse(const wchar_t* text, TestServerEndpoint* endpoint) noexcept;

enum class TestServerState : u8 {
    unset,
    active,
    invalid,
};

// The endpoint MAICORD_TEST_SERVER names. `active` points *endpoint at storage that lives to the end of the process
// (a lookup keeps its host and service strings until it completes); `invalid` is a value that does not parse.
TestServerState test_server_endpoint(const TestServerEndpoint** endpoint) noexcept;

inline constexpr u32 kTestTrustPathCapacity = 520;

enum class TestTrustState : u8 {
    unset,
    active,
    invalid,
};

// MAICORD_TEST_CA_FILE into `path` (NUL-terminated); `invalid` when it does not fit.
TestTrustState test_trust_file(wchar_t (&path)[kTestTrustPathCapacity]) noexcept;

// 0 when `certificate` (a server's leaf, with the intermediates it came with in its own store) chains to one of the
// certificates in `file` and is valid for `server_name`; otherwise the Win32 or CRYPT error that says why (a file that
// cannot be read or holds no certificate included). Nothing outside the file is trusted.
u32 test_trust_verify(const wchar_t* file, PCCERT_CONTEXT certificate, const wchar_t* server_name) noexcept;

} // namespace maicord

#endif
