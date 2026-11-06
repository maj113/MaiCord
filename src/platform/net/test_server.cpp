#include "maicord/platform/net/test_server.hpp"

#if defined(MAICORD_TEST_HOOKS)

#include "maicord/core/memory.hpp"
#include "maicord/core/test_hook.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

SRWLOCK g_endpoint_lock = SRWLOCK_INIT;
TestServerEndpoint g_endpoint;

constexpr u32 kFileCapacity = 64 * 1024;
constexpr u32 kCertificateCapacity = 8 * 1024;
constexpr char kBegin[] = "-----BEGIN CERTIFICATE-----";
constexpr char kEnd[] = "-----END CERTIFICATE-----";

bool hex_or_colon(wchar_t c) noexcept {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F') || c == L':' || c == L'.';
}

// A name or an IPv4 address: printable ASCII that is not part of a URL's structure.
bool name_character(wchar_t c) noexcept {
    return c > L' ' && c < 0x7f && c != L':' && c != L'/' && c != L'\\' && c != L'@' && c != L'?' && c != L'#' &&
           c != L'[' && c != L']';
}

// The offset of `needle` in data[from, bytes), or `bytes` when it is not there.
u32 find_text(const u8* data, u32 bytes, u32 from, const char* needle, u32 needle_bytes) noexcept {
    for (u32 at = from; at + needle_bytes <= bytes; ++at)
        if (bytes_equal(data + at, needle, needle_bytes))
            return at;
    return bytes;
}

// The calling thread's last error, never 0: a failure must not read as "trusted".
u32 failure_code() noexcept {
    const u32 error = GetLastError();
    return error ? error : ERROR_INVALID_PARAMETER;
}

// The file at `path` into data (room for kFileCapacity + 1 bytes, to tell a larger file): the byte count, or 0 when the
// file cannot be read or is too large.
u32 read_file(const wchar_t* path, u8* data) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    DWORD read = 0;
    const bool ok = ReadFile(file, data, kFileCapacity + 1, &read, nullptr) != FALSE && read <= kFileCapacity;
    CloseHandle(file);
    return ok ? read : 0;
}

bool add_certificate(HCERTSTORE store, const u8* der, u32 bytes) noexcept {
    return CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der, bytes,
                                            CERT_STORE_ADD_ALWAYS, nullptr) != FALSE;
}

// Puts every certificate of the file in the store: the PEM blocks, or the whole file as one DER certificate when it
// has none. False when one does not decode or there is none.
bool add_certificates(HCERTSTORE store, const u8* data, u32 bytes) noexcept {
    u32 at = find_text(data, bytes, 0, kBegin, sizeof(kBegin) - 1);
    if (at == bytes)
        return add_certificate(store, data, bytes);
    u32 added = 0;
    while (at != bytes) {
        const u32 end = find_text(data, bytes, at, kEnd, sizeof(kEnd) - 1);
        if (end == bytes)
            return false;
        const u32 block_end = end + sizeof(kEnd) - 1;
        BYTE der[kCertificateCapacity];
        DWORD der_bytes = sizeof(der);
        if (!CryptStringToBinaryA(reinterpret_cast<const char*>(data + at), block_end - at, CRYPT_STRING_BASE64HEADER,
                                  der, &der_bytes, nullptr, nullptr) ||
            !add_certificate(store, der, der_bytes))
            return false;
        ++added;
        at = find_text(data, bytes, block_end, kBegin, sizeof(kBegin) - 1);
    }
    return added != 0;
}

} // namespace

bool test_server_parse(const wchar_t* text, TestServerEndpoint* endpoint) noexcept {
    if (!text || !endpoint)
        return false;
    u32 length = 0;
    u32 colon = kTestServerTextCapacity;
    while (text[length]) {
        if (length == kTestServerTextCapacity)
            return false;
        if (text[length] == L':')
            colon = length;
        ++length;
    }
    if (colon == kTestServerTextCapacity)
        return false;
    const bool bracketed = colon >= 2 && text[0] == L'[' && text[colon - 1] == L']';
    const u32 host_begin = bracketed ? 1 : 0;
    const u32 host_end = bracketed ? colon - 1 : colon;
    if (host_end == host_begin || host_end - host_begin >= kTestServerHostCapacity)
        return false;
    for (u32 i = host_begin; i < host_end; ++i)
        if (!(bracketed ? hex_or_colon(text[i]) : name_character(text[i])))
            return false;
    const u32 port_digits = length - colon - 1;
    u32 port = 0;
    if (port_digits == 0 || port_digits > 5)
        return false;
    for (u32 i = colon + 1; i < length; ++i) {
        if (text[i] < L'0' || text[i] > L'9')
            return false;
        port = port * 10 + static_cast<u32>(text[i] - L'0');
    }
    if (port == 0 || port > 65535)
        return false;
    reset_value<TestServerEndpoint>(endpoint);
    for (u32 i = host_begin; i < host_end; ++i)
        endpoint->host[i - host_begin] = text[i];
    for (u32 i = 0; i < port_digits; ++i)
        endpoint->service[i] = text[colon + 1 + i];
    return true;
}

TestServerState test_server_endpoint(const TestServerEndpoint** endpoint) noexcept {
    wchar_t text[kTestServerTextCapacity + 1];
    const u32 length = test_hook(L"MAICORD_TEST_SERVER", text, kTestServerTextCapacity + 1);
    if (length == 0)
        return TestServerState::unset;
    TestServerEndpoint parsed;
    if (length > kTestServerTextCapacity || !test_server_parse(text, &parsed))
        return TestServerState::invalid;
    // A lookup in flight keeps reading the stored strings, so they are only rewritten when the value changed.
    AcquireSRWLockExclusive(&g_endpoint_lock);
    if (!bytes_equal(&g_endpoint, &parsed, sizeof(parsed)))
        g_endpoint = parsed;
    ReleaseSRWLockExclusive(&g_endpoint_lock);
    *endpoint = &g_endpoint;
    return TestServerState::active;
}

TestTrustState test_trust_file(wchar_t (&path)[kTestTrustPathCapacity]) noexcept {
    const u32 length = test_hook(L"MAICORD_TEST_CA_FILE", path, kTestTrustPathCapacity);
    if (length == 0) {
        path[0] = 0;
        return TestTrustState::unset;
    }
    return length < kTestTrustPathCapacity ? TestTrustState::active : TestTrustState::invalid;
}

u32 test_trust_verify(const wchar_t* file, PCCERT_CONTEXT certificate, const wchar_t* server_name) noexcept {
    if (!file || !certificate || !server_name)
        return ERROR_INVALID_PARAMETER;
    u8 data[kFileCapacity + 1];
    const u32 bytes = read_file(file, data);
    if (bytes == 0)
        return ERROR_FILE_NOT_FOUND;

    HCERTSTORE roots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
    if (!roots)
        return failure_code();
    u32 error = ERROR_INVALID_DATA;
    if (add_certificates(roots, data, bytes)) {
        // The private engine trusts the roots in `roots` and nothing else: not the system's, not the user's.
        CERT_CHAIN_ENGINE_CONFIG configuration{};
        configuration.cbSize = sizeof(configuration);
        configuration.hExclusiveRoot = roots;
        configuration.dwExclusiveFlags = CERT_CHAIN_EXCLUSIVE_ENABLE_CA_FLAG;
        HCERTCHAINENGINE engine = nullptr;
        if (!CertCreateCertificateChainEngine(&configuration, &engine)) {
            error = failure_code();
        } else {
            CERT_CHAIN_PARA parameters{};
            parameters.cbSize = sizeof(parameters);
            parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
            PCCERT_CHAIN_CONTEXT chain = nullptr;
            if (!CertGetCertificateChain(engine, certificate, nullptr, nullptr, &parameters, 0, nullptr, &chain)) {
                error = failure_code();
            } else {
                SSL_EXTRA_CERT_CHAIN_POLICY_PARA server{};
                server.cbSize = sizeof(server);
                server.dwAuthType = AUTHTYPE_SERVER;
                server.pwszServerName = const_cast<WCHAR*>(server_name);
                CERT_CHAIN_POLICY_PARA policy{};
                policy.cbSize = sizeof(policy);
                policy.pvExtraPolicyPara = &server;
                CERT_CHAIN_POLICY_STATUS status{};
                status.cbSize = sizeof(status);
                error = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status) ? status.dwError
                                                                                                         : failure_code();
                CertFreeCertificateChain(chain);
            }
            CertFreeCertificateChainEngine(engine);
        }
    }
    CertCloseStore(roots, 0);
    return error;
}

} // namespace maicord

#endif
