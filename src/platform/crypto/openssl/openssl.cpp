#include "openssl.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace openssl {
namespace {

// Sonames, newest first: OpenSSL 4.x, then 3.x.
constexpr const char* const kSuffixes[] = {"4", "3"};

// Held only while the libraries load.
SRWLOCK g_lock = SRWLOCK_INIT;
SharedLibrary g_crypto_library;
SharedLibrary g_ssl_library;
SharedLibrary g_ssl_crypto_library;
Crypto g_crypto;
Ssl g_ssl;
volatile u32 g_crypto_ready;
volatile u32 g_ssl_ready;

struct LoadGuard {
    LoadGuard() noexcept { AcquireSRWLockExclusive(&g_lock); }
    ~LoadGuard() noexcept { ReleaseSRWLockExclusive(&g_lock); }
};

// "libcrypto.so.4" and the like.
bool soname(char (&name)[32], const char* stem, const char* suffix) noexcept {
    const char* const parts[] = {stem, ".so.", suffix};
    u32 length = 0;
    for (const char* part : parts)
        for (; *part; ++part) {
            if (length + 1 >= sizeof(name))
                return false;
            name[length++] = *part;
        }
    name[length] = 0;
    return true;
}

// The symbol of each member of Crypto and (after its Crypto) of Ssl, in member order.
#define MAICORD_OPENSSL_SYMBOL(ret, name, args) #name "\0"
constexpr char kCryptoSymbols[] = MAICORD_OPENSSL_CRYPTO_FUNCTIONS(MAICORD_OPENSSL_SYMBOL);
constexpr char kSslSymbols[] = MAICORD_OPENSSL_SSL_FUNCTIONS(MAICORD_OPENSSL_SYMBOL);
#undef MAICORD_OPENSSL_SYMBOL
static_assert(shared_library_name_count(kCryptoSymbols) * sizeof(void*) == sizeof(Crypto), "one symbol per Crypto member");
static_assert(shared_library_name_count(kSslSymbols) * sizeof(void*) == sizeof(Ssl) - sizeof(Crypto),
              "one symbol per Ssl member after its Crypto");

// Resolves every function of a table, or none: the first missing name fails the whole load.
bool resolve(const SharedLibrary& library, const char* library_name, void* table, usize table_bytes,
             const char* names) noexcept {
    const char* missing = shared_library_resolve(library, table, table_bytes, names);
    if (missing)
        shared_library_log_missing("openssl", library_name, missing);
    return !missing;
}

bool resolve_crypto(const SharedLibrary& library, const char* library_name, Crypto* table) noexcept {
    return resolve(library, library_name, table, sizeof(*table), kCryptoSymbols);
}

bool resolve_ssl(const SharedLibrary& library, const char* library_name, Ssl* table) noexcept {
    return resolve(library, library_name, &table->crypto + 1, sizeof(*table) - sizeof(table->crypto), kSslSymbols);
}

// Opens libcrypto.so.<suffix> into `library` and fills `table`; *failure says why not. The soname is the version: .3 is
// OpenSSL 3.x and .4 OpenSSL 4.x (1.1 is libcrypto.so.1.1, never opened).
bool open_crypto(const char* suffix, SharedLibrary* library, Crypto* table, u32* failure) noexcept {
    char name[32];
    if (!soname(name, "libcrypto", suffix) || !shared_library_open(library, name))
        return false;
    if (!resolve_crypto(*library, name, table)) {
        shared_library_close(library);
        *failure = kFailureSymbol;
        return false;
    }
    return true;
}

} // namespace

const Crypto* crypto(u32* failure) noexcept {
    if (__atomic_load_n(&g_crypto_ready, __ATOMIC_ACQUIRE))
        return &g_crypto;
    LoadGuard guard;
    if (g_crypto_ready)
        return &g_crypto;
    u32 reason = kFailureLibrary;
    for (const char* suffix : kSuffixes) {
        if (open_crypto(suffix, &g_crypto_library, &g_crypto, &reason)) {
            __atomic_store_n(&g_crypto_ready, 1u, __ATOMIC_RELEASE);
            return &g_crypto;
        }
    }
    if (failure)
        *failure = reason;
    return nullptr;
}

const Ssl* ssl(u32* failure) noexcept {
    if (__atomic_load_n(&g_ssl_ready, __ATOMIC_ACQUIRE))
        return &g_ssl;
    LoadGuard guard;
    if (g_ssl_ready)
        return &g_ssl;
    u32 reason = kFailureLibrary;
    for (const char* suffix : kSuffixes) {
        char name[32];
        if (!soname(name, "libssl", suffix) || !shared_library_open(&g_ssl_library, name))
            continue;
        // libssl pulls its libcrypto in; the second open of the same soname returns that load.
        if (!open_crypto(suffix, &g_ssl_crypto_library, &g_ssl.crypto, &reason)) {
            shared_library_close(&g_ssl_library);
            continue;
        }
        if (!resolve_ssl(g_ssl_library, name, &g_ssl)) {
            shared_library_close(&g_ssl_library);
            shared_library_close(&g_ssl_crypto_library);
            reason = kFailureSymbol;
            continue;
        }
        __atomic_store_n(&g_ssl_ready, 1u, __ATOMIC_RELEASE);
        return &g_ssl;
    }
    if (failure)
        *failure = reason;
    return nullptr;
}

} // namespace openssl
} // namespace maicord
