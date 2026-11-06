#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/platform/net/network.hpp"
#include "maicord/platform/net/socket.hpp"
#include "maicord/platform/net/test_server.hpp"
#include "../../support/test_output.hpp"

// The test hooks that point a Windows client at tools/fake_discord_server.py (platform/net/test_server.hpp), compiled
// here with MAICORD_TEST_HOOKS: MAICORD_TEST_SERVER is parsed and replaces what a name lookup answers with, and the CA
// file named by MAICORD_TEST_CA_FILE is the only trust there is: a certificate that chains to it is accepted for its
// names, one issued by another authority, or for another name, is rejected. The certificates are made up (a hundred
// years of validity, nothing else about them matters).

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

bool same(const wchar_t* left, const wchar_t* right) noexcept {
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

// CA A: a self-signed certificate authority.
constexpr char kCertificateAuthorityA[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBoDCCAUWgAwIBAgIUCS+dHhZi2PRlbpQ7PrC0a1lE2v4wCgYIKoZIzj0EAwIw\n"
    "HDEaMBgGA1UEAwwRTWFpQ29yZCBUZXN0IENBIEEwIBcNMjYxMTA2MjEyOTUyWhgP\n"
    "MjEyNjEwMTMyMTI5NTJaMBwxGjAYBgNVBAMMEU1haUNvcmQgVGVzdCBDQSBBMFkw\n"
    "EwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEDoVJveg1QuzlYCghixHqio2MPCVSPRQ+\n"
    "HRpUQGOiCQS/Xb5e2joXDFB/1f8UB0KJSRaBSKwrRweqr+pamohRZaNjMGEwHQYD\n"
    "VR0OBBYEFObaHEqPrN4XT1L1KnDN6cgUYMheMB8GA1UdIwQYMBaAFObaHEqPrN4X\n"
    "T1L1KnDN6cgUYMheMA8GA1UdEwEB/wQFMAMBAf8wDgYDVR0PAQH/BAQDAgEGMAoG\n"
    "CCqGSM49BAMCA0kAMEYCIQCvaJBuMwJ84tGZUuI/ge5uOBEEWX5nXYQp12asuUbv\n"
    "HwIhAOBtrjw8QjVOjQ2t4HnqhqEUU35NbeLevmaEUBGphmgI\n"
    "-----END CERTIFICATE-----\n";
// A server certificate for discord.com and gateway.discord.gg issued by CA A.
constexpr char kServerOfA[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIB1DCCAXmgAwIBAgIUe4c9tzUU9RNF1ZlS7jAua+Y3OSUwCgYIKoZIzj0EAwIw\n"
    "HDEaMBgGA1UEAwwRTWFpQ29yZCBUZXN0IENBIEEwIBcNMjYxMTA2MjEyOTUyWhgP\n"
    "MjEyNjEwMTMyMTI5NTJaMBYxFDASBgNVBAMMC2Rpc2NvcmQuY29tMFkwEwYHKoZI\n"
    "zj0CAQYIKoZIzj0DAQcDQgAEV76txiIBIn4B2jY7V8IM6fZhKXuVpLfev1P+xZV2\n"
    "7l2Ia/VXEqT9bDS4CnyRI6vXmW1D5WgUXNNCP1cuGDIfdqOBnDCBmTAJBgNVHRME\n"
    "AjAAMAsGA1UdDwQEAwIHgDATBgNVHSUEDDAKBggrBgEFBQcDATAqBgNVHREEIzAh\n"
    "ggtkaXNjb3JkLmNvbYISZ2F0ZXdheS5kaXNjb3JkLmdnMB0GA1UdDgQWBBSqJniJ\n"
    "ICIuB2TQ0+SYHcqqihlQRjAfBgNVHSMEGDAWgBTm2hxKj6zeF09S9SpwzenIFGDI\n"
    "XjAKBggqhkjOPQQDAgNJADBGAiEA34+V9pyYtai8FcjEbdKlBVE5AdBfrsxejMrd\n"
    "WMDcNX0CIQDLbUZLlt2ApB5xCwV3XCmlMqeWHlTgwmuWcYcMR7SgwA==\n"
    "-----END CERTIFICATE-----\n";
// CA B: another authority.
constexpr char kCertificateAuthorityB[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBoDCCAUWgAwIBAgIUPVw3j9LaW2SKWuG+B5oBaRaagAswCgYIKoZIzj0EAwIw\n"
    "HDEaMBgGA1UEAwwRTWFpQ29yZCBUZXN0IENBIEIwIBcNMjYxMTA2MjEyOTUyWhgP\n"
    "MjEyNjEwMTMyMTI5NTJaMBwxGjAYBgNVBAMMEU1haUNvcmQgVGVzdCBDQSBCMFkw\n"
    "EwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEpQyDm99tsa/40VFY0WcfkiAexAf89XiK\n"
    "0w7YqRVJkDm3nGI5QWu0Txx7l7bcL6EYkaNtNH90Dwhnm4pw6mWngKNjMGEwHQYD\n"
    "VR0OBBYEFDr1Xpa0/8dp3fhpkQqsXZsVaOsfMB8GA1UdIwQYMBaAFDr1Xpa0/8dp\n"
    "3fhpkQqsXZsVaOsfMA8GA1UdEwEB/wQFMAMBAf8wDgYDVR0PAQH/BAQDAgEGMAoG\n"
    "CCqGSM49BAMCA0kAMEYCIQD8PcFHGF6v8CYvgWAfJh0YcdGqHNwDsCgiZvPkecuq\n"
    "PwIhAK29ABq435i9hCgiDh9VP1D39/jc0uyGqJ0ChwykYwXa\n"
    "-----END CERTIFICATE-----\n";
// A server certificate for the same names issued by CA B.
constexpr char kServerOfB[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIB0jCCAXmgAwIBAgIUR+qvv1PO2PwZfuxjRr2pTQpsMFwwCgYIKoZIzj0EAwIw\n"
    "HDEaMBgGA1UEAwwRTWFpQ29yZCBUZXN0IENBIEIwIBcNMjYxMTA2MjEyOTUyWhgP\n"
    "MjEyNjEwMTMyMTI5NTJaMBYxFDASBgNVBAMMC2Rpc2NvcmQuY29tMFkwEwYHKoZI\n"
    "zj0CAQYIKoZIzj0DAQcDQgAEi56isCoa7AubZisje5Xz+cyOBIdZejHI3W1ZUrNS\n"
    "oMmpy1cfFJTuW6hMFw5FmaYzK0mfZA6BcxR+41dcZ4BaO6OBnDCBmTAJBgNVHRME\n"
    "AjAAMAsGA1UdDwQEAwIHgDATBgNVHSUEDDAKBggrBgEFBQcDATAqBgNVHREEIzAh\n"
    "ggtkaXNjb3JkLmNvbYISZ2F0ZXdheS5kaXNjb3JkLmdnMB0GA1UdDgQWBBRy2luU\n"
    "bog25bxo6ao8JzuZR0PsIDAfBgNVHSMEGDAWgBQ69V6WtP/Had34aZEKrF2bFWjr\n"
    "HzAKBggqhkjOPQQDAgNHADBEAiAXuSIm8aXi6l0Lx+mKhMVh10ypU/9vbgoRYhzK\n"
    "Sjw77AIgMU3kE2k4z36IuB/P+SvqS2YJoOzsASe8AthrCjyPsYc=\n"
    "-----END CERTIFICATE-----\n";
// A self-signed certificate for the same names, as the fake server's.
constexpr char kSelfSigned[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBsDCCAVWgAwIBAgIUfZRfZsnSS0Y6yuPA3u4g+d7Tu1AwCgYIKoZIzj0EAwIw\n"
    "FjEUMBIGA1UEAwwLZGlzY29yZC5jb20wIBcNMjYxMTA2MjEyOTUyWhgPMjEyNjEw\n"
    "MTMyMTI5NTJaMBYxFDASBgNVBAMMC2Rpc2NvcmQuY29tMFkwEwYHKoZIzj0CAQYI\n"
    "KoZIzj0DAQcDQgAEgYF6du51LunYH33A+WaAogpR9oEv9m8QpMvNljP0JNZwXJOD\n"
    "NzYcn2cPlkehEJqVaw9I0CKPOseRwbbX3/DEWqN/MH0wHQYDVR0OBBYEFOibxUTK\n"
    "uVtQt/0iEy3IXnq9ZxTXMB8GA1UdIwQYMBaAFOibxUTKuVtQt/0iEy3IXnq9ZxTX\n"
    "MA8GA1UdEwEB/wQFMAMBAf8wKgYDVR0RBCMwIYILZGlzY29yZC5jb22CEmdhdGV3\n"
    "YXkuZGlzY29yZC5nZzAKBggqhkjOPQQDAgNJADBGAiEA5vaFd+DN6OfAEdCpgOBw\n"
    "EnG1rSVIRWcKu6bNXO/aA50CIQC989hzkva1kSSOYcAMjKbda/4GpOVhZx48MjCU\n"
    "4cedmA==\n"
    "-----END CERTIFICATE-----\n";

void test_parse() noexcept {
    TestServerEndpoint endpoint{};
    check(test_server_parse(L"127.0.0.1:8443", &endpoint) && same(endpoint.host, L"127.0.0.1") &&
              same(endpoint.service, L"8443"),
          "an IPv4 address and a port");
    check(test_server_parse(L"[::1]:9", &endpoint) && same(endpoint.host, L"::1") && same(endpoint.service, L"9"),
          "a bracketed IPv6 address");
    check(test_server_parse(L"localhost:65535", &endpoint) && same(endpoint.host, L"localhost") &&
              same(endpoint.service, L"65535"),
          "a name and the highest port");
    const wchar_t* const invalid[]{L"",          L"localhost",   L"localhost:", L":8443",        L"localhost:0",
                                   L"localhost:65536", L"localhost:80a", L"local host:80", L"https://x:80", L"[::1:80",
                                   L"[]:80",     L"a@b:80",      L"host:123456"};
    bool all_refused = true;
    for (const wchar_t* text : invalid) {
        TestServerEndpoint untouched{};
        all_refused = all_refused && !test_server_parse(text, &untouched) && untouched.host[0] == 0;
    }
    check(all_refused, "malformed values are refused and leave the endpoint alone");
    check(!test_server_parse(nullptr, &endpoint) && !test_server_parse(L"a:1", nullptr), "no text, no endpoint");
}

// The first answer of a lookup of `host`, port `service`, on the loop; false when it does not complete.
bool lookup(NetworkLoop* loop, const wchar_t* host, const wchar_t* service, NetworkAddress* answer, u32* count) noexcept {
    NetworkResolver resolver{};
    if (!network_resolver_start(loop, &resolver, host, service).ok())
        return false;
    const NetworkPollResult polled = network_loop_poll(loop, 5000);
    const bool done = polled.state == NetworkPollState::completion && polled.completion.kind == NetworkOperationKind::resolve &&
                      polled.completion.succeeded && !polled.completion.stale && resolver.address_count >= 1;
    if (done) {
        *answer = resolver.addresses[0];
        *count = resolver.address_count;
    }
    return network_resolver_release(&resolver) && done;
}

void test_host_override(NetworkLoop* loop) noexcept {
    const TestServerEndpoint* endpoint = nullptr;
    SetEnvironmentVariableW(L"MAICORD_TEST_SERVER", nullptr);
    check(test_server_endpoint(&endpoint) == TestServerState::unset, "no override without the variable");
    NetworkAddress answer{};
    u32 count = 0;
    check(lookup(loop, L"127.0.0.1", L"8080", &answer, &count) && answer.port == 8080,
          "without the variable the lookup asks for what it was given");

    SetEnvironmentVariableW(L"MAICORD_TEST_SERVER", L"127.0.0.1:18443");
    check(test_server_endpoint(&endpoint) == TestServerState::active && endpoint && same(endpoint->host, L"127.0.0.1") &&
              same(endpoint->service, L"18443"),
          "the variable is parsed");
    // Each of the client's hosts, whatever port it asked for, answers with the one address of the variable: a name the
    // system could not resolve here (the lookup would fail or leave the machine) shows the name is never looked up.
    const wchar_t* const names[]{L"discord.com", L"gateway.discord.gg", L"cdn.discordapp.com", L"media.discordapp.net",
                                 L"unresolvable.invalid"};
    bool all_redirected = true;
    for (const wchar_t* name : names) {
        answer = {};
        count = 0;
        all_redirected = all_redirected && lookup(loop, name, L"443", &answer, &count) && count == 1 && answer.family == 4 &&
                         answer.ip[0] == 127 && answer.ip[1] == 0 && answer.ip[2] == 0 && answer.ip[3] == 1 &&
                         answer.port == 18443;
    }
    check(all_redirected, "API, gateway, CDN and other names are answered with the override");

    SetEnvironmentVariableW(L"MAICORD_TEST_SERVER", L"[::1]:9443");
    answer = {};
    check(lookup(loop, L"discord.com", L"443", &answer, &count) && answer.family == 6 && answer.ip[15] == 1 &&
              answer.port == 9443,
          "an IPv6 override");

    // A value that does not parse must not send a lookup to the real network.
    SetEnvironmentVariableW(L"MAICORD_TEST_SERVER", L"discord.com");
    NetworkResolver resolver{};
    check(test_server_endpoint(&endpoint) == TestServerState::invalid &&
              !network_resolver_start(loop, &resolver, L"discord.com", L"443").ok() && !resolver.pending,
          "a malformed override fails the lookup");
    SetEnvironmentVariableW(L"MAICORD_TEST_SERVER", nullptr);
}

// The temporary file `name` holding `first` and then `second` (may be null); its path in `path`.
bool write_file(wchar_t (&path)[kTestTrustPathCapacity], const wchar_t* name, const char* first, const char* second) noexcept {
    u32 length = GetTempPathW(kTestTrustPathCapacity, path);
    if (length == 0 || length >= kTestTrustPathCapacity || !wide_append(path, kTestTrustPathCapacity, &length, name))
        return false;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const char* const parts[]{first, second};
    bool ok = true;
    for (const char* text : parts) {
        DWORD written = 0;
        const u32 bytes = text ? c_string_length(text) : 0;
        ok = ok && (bytes == 0 || (WriteFile(file, text, bytes, &written, nullptr) != FALSE && written == bytes));
    }
    return CloseHandle(file) != FALSE && ok;
}

PCCERT_CONTEXT certificate(const char* pem) noexcept {
    BYTE der[2048];
    DWORD bytes = sizeof(der);
    if (!CryptStringToBinaryA(pem, 0, CRYPT_STRING_BASE64HEADER, der, &bytes, nullptr, nullptr))
        return nullptr;
    return CertCreateCertificateContext(X509_ASN_ENCODING, der, bytes);
}

void test_trust() noexcept {
    wchar_t only_a[kTestTrustPathCapacity], only_b[kTestTrustPathCapacity], both[kTestTrustPathCapacity],
        self_signed[kTestTrustPathCapacity], garbage[kTestTrustPathCapacity];
    const bool written = write_file(only_a, L"maicord-test-server-a.pem", kCertificateAuthorityA, nullptr) &&
                         write_file(only_b, L"maicord-test-server-b.pem", kCertificateAuthorityB, nullptr) &&
                         write_file(both, L"maicord-test-server-ab.pem", kCertificateAuthorityB, kCertificateAuthorityA) &&
                         write_file(self_signed, L"maicord-test-server-self.pem", kSelfSigned, nullptr) &&
                         write_file(garbage, L"maicord-test-server-garbage.pem", "this is not a certificate\n", nullptr);
    check(written, "the certificate files are written");
    PCCERT_CONTEXT server_a = certificate(kServerOfA);
    PCCERT_CONTEXT server_b = certificate(kServerOfB);
    PCCERT_CONTEXT self = certificate(kSelfSigned);
    check(server_a && server_b && self, "the certificates decode");
    if (written && server_a && server_b && self) {
        check(test_trust_verify(only_a, server_a, L"discord.com") == 0, "a certificate issued by the CA is accepted");
        check(test_trust_verify(only_a, server_a, L"gateway.discord.gg") == 0, "and for each of its names");
        check(test_trust_verify(only_a, server_a, L"example.com") != 0, "but not for another name");
        check(test_trust_verify(only_a, server_b, L"discord.com") != 0, "one issued by another authority is rejected");
        check(test_trust_verify(only_b, server_a, L"discord.com") != 0, "whichever authority the file holds");
        check(test_trust_verify(both, server_a, L"discord.com") == 0 && test_trust_verify(both, server_b, L"discord.com") == 0,
              "a file of several certificates trusts each");
        check(test_trust_verify(self_signed, self, L"discord.com") == 0, "a self-signed certificate is trusted when it is the file");
        check(test_trust_verify(only_a, self, L"discord.com") != 0, "and not otherwise");
        check(test_trust_verify(garbage, server_a, L"discord.com") != 0, "a file with no certificate trusts nothing");
        check(test_trust_verify(L"maicord-test-server-missing.pem", server_a, L"discord.com") != 0, "a missing file trusts nothing");
    }
    if (server_a)
        CertFreeCertificateContext(server_a);
    if (server_b)
        CertFreeCertificateContext(server_b);
    if (self)
        CertFreeCertificateContext(self);

    // The client does not start half-pointed at the fake server: the real network is never the fallback.
    SetEnvironmentVariableW(L"MAICORD_TEST_SERVER", nullptr);
    SetEnvironmentVariableW(L"MAICORD_TEST_CA_FILE", nullptr);
    check(test_hooks_startup_refusal() == nullptr, "neither switch is an ordinary start");
    SetEnvironmentVariableW(L"MAICORD_TEST_SERVER", L"127.0.0.1:18443");
    check(test_hooks_startup_refusal() != nullptr, "the server without the CA file is refused");
    SetEnvironmentVariableW(L"MAICORD_TEST_CA_FILE", only_a);
    check(test_hooks_startup_refusal() == nullptr, "both, with a usable CA file, start");
    SetEnvironmentVariableW(L"MAICORD_TEST_SERVER", nullptr);
    check(test_hooks_startup_refusal() != nullptr, "the CA file without the server is refused");
    SetEnvironmentVariableW(L"MAICORD_TEST_SERVER", L"bogus");
    check(test_hooks_startup_refusal() != nullptr, "a server value that does not parse is refused");
    SetEnvironmentVariableW(L"MAICORD_TEST_SERVER", L"127.0.0.1:18443");
    SetEnvironmentVariableW(L"MAICORD_TEST_CA_FILE", garbage);
    check(test_hooks_startup_refusal() != nullptr, "a CA file with no certificate is refused");
    SetEnvironmentVariableW(L"MAICORD_TEST_CA_FILE", L"maicord-test-server-missing.pem");
    check(test_hooks_startup_refusal() != nullptr, "a missing CA file is refused");
    SetEnvironmentVariableW(L"MAICORD_TEST_SERVER", nullptr);
    SetEnvironmentVariableW(L"MAICORD_TEST_CA_FILE", nullptr);

    const wchar_t* const files[]{only_a, only_b, both, self_signed, garbage};
    for (const wchar_t* path : files)
        DeleteFileW(path);

    wchar_t file[kTestTrustPathCapacity];
    SetEnvironmentVariableW(L"MAICORD_TEST_CA_FILE", nullptr);
    check(test_trust_file(file) == TestTrustState::unset, "no CA file without the variable");
    SetEnvironmentVariableW(L"MAICORD_TEST_CA_FILE", L"C:\\ca.pem");
    check(test_trust_file(file) == TestTrustState::active && same(file, L"C:\\ca.pem"), "the CA file variable is read");
    SetEnvironmentVariableW(L"MAICORD_TEST_CA_FILE", nullptr);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_test_server_entry) {
    using namespace maicord;
    check(socket_startup() == 0, "sockets start");
    test_parse();
    NetworkLoop loop{};
    check(network_loop_initialize(&loop).ok() && network_loop_ready(loop), "initialize the loop");
    test_host_override(&loop);
    network_loop_release(&loop);
    test_trust();
    socket_cleanup();
    test_exit(g_failures, "test_server");
}
