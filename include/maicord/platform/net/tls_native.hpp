#pragma once

#include "maicord/core/base.hpp"

// The operating system's part of a TlsClient (tls_client.hpp). Windows keeps Schannel's two handles; Linux keeps
// the OpenSSL connection and the two memory BIOs that carry its records (src/platform/net/linux/tls_client.cpp).

namespace maicord {

#if defined(_WIN32)

// SecHandle (CredHandle, CtxtHandle) layout; src/platform/net/tls_client.cpp checks it against <Security.h>.
struct TlsSecurityHandle {
    usize lower;
    usize upper;
};

struct TlsNative {
    TlsSecurityHandle credentials;
    TlsSecurityHandle context;
};

#else

// Bytes of records OpenSSL produced while decrypting (a key update answer, an alert) that wait for the next
// encrypt call to go out in front of its own records.
inline constexpr u32 kTlsControlCapacity = 512;

struct TlsNative {
    void* ssl;
    void* read_bio;
    void* write_bio;
    u32 control_bytes;
    u8 control[kTlsControlCapacity];
};

#endif

} // namespace maicord
