#include "maicord/app/voice/dave/key_derivation.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/voice/mls_encoding.hpp"

namespace maicord {
// Key derivation runs once per sender key generation or epoch, never per frame: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
KdfResult mls_expand_label(KdfSha256Workspace& workspace, ConstSlice<u8> secret, ConstSlice<u8> label,
                           ConstSlice<u8> context, Slice<u8> output) noexcept {
    static constexpr u8 prefix[]{'M', 'L', 'S', ' ', '1', '.', '0', ' '};
    u8 header[6]{static_cast<u8>(output.count >> 8), static_cast<u8>(output.count)}, context_length[4];
    const u32 header_bytes = 2 + mls_vector_length(label.count + 8, header + 2);
    const u32 context_length_bytes = mls_vector_length(context.count, context_length);
    const ConstSlice<u8> parts[]{{header, header_bytes},
                                 {prefix, sizeof(prefix)},
                                 label,
                                 {context_length, context_length_bytes},
                                 context};
    return hkdf_sha256_expand(workspace, secret, {parts, 5}, output);
}
KdfResult dave_ratchet_step(KdfSha256Workspace& workspace, ConstSlice<u8> seed, u32 generation, u8* key,
                            u8* next_secret) noexcept {
    static constexpr u8 key_label[]{'k', 'e', 'y'}, secret_label[]{'s', 'e', 'c', 'r', 'e', 't'};
    u8 context[4], derived_key[16], next[32];
    byte_store_be32(context, generation);
    auto result =
        mls_expand_label(workspace, seed, {key_label, sizeof(key_label)}, {context, 4}, {derived_key, 16});
    if (result.ok())
        result = mls_expand_label(workspace, seed, {secret_label, sizeof(secret_label)}, {context, 4},
                                  {next, 32});
    if (result.ok()) {
        copy_bytes(key, derived_key, 16);
        copy_bytes(next_secret, next, 32);
        result.bytes = 48;
    }
    secure_clear_bytes(derived_key, sizeof(derived_key));
    secure_clear_bytes(next, sizeof(next));
    return result;
}
#pragma clang attribute pop
} // namespace maicord
