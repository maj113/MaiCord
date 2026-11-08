// hkdf_sha256.hpp over the system OpenSSL: SHA-256 through EVP_MD and HMAC-SHA-256 through EVP_MAC, with the
// argument rules of src/platform/crypto/kdf_internal.hpp and the extract/expand loop of the Windows file. The
// workspace storage holds the three OpenSSL objects of the worker (fetched HMAC, its context, the digest context),
// created once.
#include "../kdf_internal.hpp"
#include "openssl.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
using namespace kdf_internal;
namespace {

struct State {
    void* mac;
    void* hmac;
    void* digest;
};

// One keyed or unkeyed SHA-256 computation on the worker's contexts.
struct Hash {
    const openssl::Crypto& library;
    const State& state;
    bool keyed;
    ConstSlice<u8> key;

    bool begin() noexcept {
        if (!keyed)
            return library.EVP_DigestInit_ex(state.digest, library.EVP_sha256(), nullptr) == 1;
        // An explicit zero-length key still means HMAC, not an unkeyed hash.
        static constexpr u8 empty = 0;
        const openssl::Param parameters[]{openssl::param_utf8("digest", "SHA256", 6), openssl::param_end()};
        return library.EVP_MAC_init(state.hmac, key.count ? key.data : &empty, key.count, parameters) == 1;
    }
    bool feed(ConstSlice<u8> bytes) noexcept {
        if (!bytes.count)
            return true;
        return keyed ? library.EVP_MAC_update(state.hmac, bytes.data, bytes.count) == 1
                     : library.EVP_DigestUpdate(state.digest, bytes.data, bytes.count) == 1;
    }
    bool finish(u8* block) noexcept {
        if (keyed) {
            usize written = 0;
            return library.EVP_MAC_final(state.hmac, block, &written, 32) == 1 && written == 32;
        }
        u32 written = 0;
        return library.EVP_DigestFinal_ex(state.digest, block, &written) == 1 && written == 32;
    }
    // OpenSSL keeps the key's pads in the context after a final: overwrite them with an empty key's.
    void forget() noexcept {
        if (keyed) {
            const openssl::Param parameters[]{openssl::param_utf8("digest", "SHA256", 6), openssl::param_end()};
            static constexpr u8 empty = 0;
            library.EVP_MAC_init(state.hmac, &empty, 0, parameters);
        }
    }
};

KdfResult calculate(KdfSha256Workspace& workspace, ConstSlice<u8> key, ConstSlice<ConstSlice<u8>> parts,
                    Slice<u8> output, bool expand, bool keyed = true, ConstSlice<u8> prefix = {}) noexcept {
    const u32 wanted = expand ? output.count : 32;
    const auto checked = validate(workspace, key, parts, output, wanted);
    if (!checked.ok())
        return checked;
    if (prefix.count) {
        const auto prefix_checked = validate(workspace, key, {&prefix, 1}, output, wanted);
        if (!prefix_checked.ok())
            return prefix_checked;
    }
    if (!wanted)
        return result(KdfError::none);
    u32 failure = 0;
    const openssl::Crypto* library = openssl::crypto(&failure);
    if (!library)
        return result(KdfError::platform_failure, 0, openssl::library_status(failure));
    const State& state = *reinterpret_cast<const State*>(workspace.storage.data);
    Hash hash{*library, state, keyed, key};
    u8 block[32]{};
    u32 done = 0;
    bool good = true;
    for (u32 index = 1; good && done != wanted; ++index) {
        good = hash.begin();
        if (good && expand && done)
            good = hash.feed({block, 32});
        if (good && prefix.count)
            good = hash.feed(prefix);
        for (u32 part = 0; good && part != parts.count; ++part)
            good = hash.feed(parts.data[part]);
        const u8 counter = static_cast<u8>(index);
        if (good && expand)
            good = hash.feed({&counter, 1});
        good = good && hash.finish(block);
        if (!good)
            break;
        const u32 bytes = minimum(32u, wanted - done);
        copy_bytes(output.data + done, block, bytes);
        done += bytes;
    }
    const i32 status = good ? 0 : openssl::error_status(*library);
    hash.forget();
    secure_clear_bytes(block, sizeof(block));
    if (!good) {
        secure_clear_bytes(output.data, wanted);
        return result(KdfError::platform_failure, 0, status);
    }
    return result(KdfError::none, wanted);
}

} // namespace

KdfResult kdf_sha256_initialize(KdfSha256Workspace* workspace) noexcept {
    if (!workspace || workspace->storage.data || workspace->storage.locked)
        return result(KdfError::invalid_input);
    u32 failure = 0;
    const openssl::Crypto* library = openssl::crypto(&failure);
    if (!library)
        return result(KdfError::platform_failure, 0, openssl::library_status(failure));
    State state{};
    state.mac = library->EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    state.hmac = state.mac ? library->EVP_MAC_CTX_new(state.mac) : nullptr;
    state.digest = library->EVP_MD_CTX_new();
    if (!state.mac || !state.hmac || !state.digest) {
        const i32 status = openssl::error_status(*library);
        if (state.hmac)
            library->EVP_MAC_CTX_free(state.hmac);
        if (state.mac)
            library->EVP_MAC_free(state.mac);
        if (state.digest)
            library->EVP_MD_CTX_free(state.digest);
        return result(KdfError::platform_failure, 0, status);
    }
    const auto allocated = secure_buffer_initialize(&workspace->storage, sizeof(State));
    if (!allocated.ok()) {
        library->EVP_MAC_CTX_free(state.hmac);
        library->EVP_MAC_free(state.mac);
        library->EVP_MD_CTX_free(state.digest);
        return result(KdfError::storage, 0, static_cast<i32>(allocated.native_error));
    }
    copy_bytes(workspace->storage.data, &state, sizeof(State));
    workspace->storage.bytes = sizeof(State);
    return result(KdfError::none);
}

void kdf_sha256_release(KdfSha256Workspace* workspace) noexcept {
    if (!workspace)
        return;
    if (workspace->storage.data) {
        const openssl::Crypto* library = openssl::crypto();
        const State& state = *reinterpret_cast<const State*>(workspace->storage.data);
        if (library) {
            library->EVP_MAC_CTX_free(state.hmac);
            library->EVP_MAC_free(state.mac);
            library->EVP_MD_CTX_free(state.digest);
        }
    }
    secure_buffer_release(&workspace->storage);
}

KdfResult sha256(KdfSha256Workspace& workspace, ConstSlice<ConstSlice<u8>> parts, Slice<u8> output) noexcept {
    return calculate(workspace, {}, parts, output, false, false);
}

KdfResult hmac_sha256_prefixed(KdfSha256Workspace& workspace, ConstSlice<u8> key, ConstSlice<u8> prefix,
                               ConstSlice<ConstSlice<u8>> parts, Slice<u8> output) noexcept {
    return calculate(workspace, key, parts, output, false, true, prefix);
}

KdfResult hkdf_sha256_expand(KdfSha256Workspace& workspace, ConstSlice<u8> prk, ConstSlice<ConstSlice<u8>> info,
                             Slice<u8> output) noexcept {
    if (!prk.count || output.count > 255 * 32)
        return result(KdfError::invalid_input);
    return calculate(workspace, prk, info, output, true);
}

} // namespace maicord
