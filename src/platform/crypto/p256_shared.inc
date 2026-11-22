// The parts of p256.hpp that are the same on both systems, included inside namespace maicord by
// src/platform/crypto/p256.cpp (Windows) and src/platform/crypto/openssl/p256.cpp. Each defines import and export_key
// for its system library first.

// A system-RNG scalar in [1,n) imported as a private key: the library derives the public point once. On Windows,
// BCryptFinalizeKeyPair also self-tests the pair.
P256Result p256_generate(P256Key* key, P256Kind kind) noexcept {
    if (key->handle)
        return {0, 0, P256Error::invalid_input};
    u8 scalar[32];
    P256Result result{0, 0, P256Error::platform_failure};
    // Rejection sampling: an out-of-range scalar has probability 2^-32.
    for (u32 attempt = 0; attempt != 8; ++attempt) {
        const auto random = secure_random_fill({scalar, 32});
        if (!random.ok()) {
            result = {0, random.platform_status, P256Error::platform_failure};
            break;
        }
        if (scalar_valid({scalar, 32})) {
            result = import(key, kind, {}, {scalar, 32}, true);
            break;
        }
    }
    secure_clear_bytes(scalar, sizeof(scalar));
    return result;
}

P256Result p256_import_public(P256Key* key, P256Kind kind, ConstSlice<u8> point) noexcept {
    return import(key, kind, point, {}, false);
}

P256Result p256_import_private(P256Key* key, P256Kind kind, ConstSlice<u8> scalar, ConstSlice<u8> point) noexcept {
    return import(key, kind, point, scalar, true);
}

P256Result p256_export_public(const P256Key& key, Slice<u8> output) noexcept {
    return export_key(key, output, false);
}

P256Result p256_export_private(const P256Key& key, Slice<u8> output) noexcept {
    return export_key(key, output, true);
}
