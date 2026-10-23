#pragma once
// Internal helpers shared by the DAVE MLS group implementation.
#include "maicord/app/dave_key_derivation.hpp"
#include "maicord/app/mls_tree.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/mls_encoding.hpp"
#include "maicord/protocol/mls_reader.hpp"
#include "byte_views.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {
inline u32 mls_varint_size(u32 value) noexcept {
    return value < 64 ? 1u : value < 16384 ? 2u : 4u;
}
__declspec(noinline) inline bool mls_equal(const void* left, const void* right, u32 bytes) noexcept {
    const volatile u8* a = static_cast<const u8*>(left);
    const volatile u8* b = static_cast<const u8*>(right);
    u32 difference = 0;
    for (u32 i = 0; i != bytes; ++i)
        difference |= a[i] ^ b[i];
    return difference == 0;
}
inline bool mls_done(const MlsReader& reader) noexcept {
    return reader.error == MlsWireError::none && reader.used == reader.input.count;
}
// A P-256 SEC1 uncompressed point vector; null after any failure.
__declspec(noinline) inline const u8* mls_read_point(MlsReader& reader) noexcept {
    const auto point = reader.vector();
    if (reader.error != MlsWireError::none)
        return nullptr;
    if (point.count != kMlsPointBytes || point.data[0] != 4) {
        reader.fail(MlsWireError::malformed, reader.used);
        return nullptr;
    }
    return point.data;
}
inline u64 mls_load_be64(const u8* data) noexcept {
    u64 value = 0;
    for (u32 i = 0; i != 8; ++i)
        value = (value << 8) | data[i];
    return value;
}
inline void mls_store_be64(u64 value, u8* output) noexcept {
    for (u32 i = 0; i != 8; ++i)
        output[7 - i] = static_cast<u8>(value >> (8 * i));
}
// Sorted big-endian u32 list membership (unmerged leaves).
__declspec(noinline) inline bool mls_u32_list_contains(ConstSlice<u8> values, u32 target) noexcept {
    for (u32 i = 0; i + 4 <= values.count; i += 4) {
        const u32 value = byte_load_be32(values.data + i);
        if (value == target)
            return true;
        if (value > target)
            return false;
    }
    return false;
}

// Capacity-checked big-endian writer with a sticky first failure.
struct MlsWriter {
    u8* data;
    u32 capacity, used;
    MlsStatus status;
    MlsWriter(Slice<u8> output) noexcept : data(output.data), capacity(output.count), used(0), status() {
    }
    bool ok() const noexcept {
        return status == MlsStatus::ok;
    }
    __declspec(noinline) void bytes(const void* value, u32 count) noexcept {
        if (status != MlsStatus::ok)
            return;
        if (count > capacity - used) {
            status = MlsStatus::buffer_too_small;
            return;
        }
        copy_bytes(data + used, value, count);
        used += count;
    }
    void skip(u32 count) noexcept {
        if (status == MlsStatus::ok && count > capacity - used)
            status = MlsStatus::buffer_too_small;
        else if (status == MlsStatus::ok)
            used += count;
    }
    __declspec(noinline) void integer(u64 value, u32 count) noexcept {
        u8 encoded[8];
        for (u32 i = 0; i != count; ++i)
            encoded[count - 1 - i] = static_cast<u8>(value >> (8 * i));
        bytes(encoded, count);
    }
    __declspec(noinline) void varint(u32 value) noexcept {
        u8 encoded[4];
        const u32 count = mls_vector_length(value, encoded);
        if (!count && status == MlsStatus::ok)
            status = MlsStatus::invalid_argument;
        bytes(encoded, count);
    }
    void vector(const void* value, u32 count) noexcept {
        varint(count);
        bytes(value, count);
    }
};

// Erases a secret on every exit path.
struct MlsWipe {
    void* data;
    u32 bytes;
    ~MlsWipe() {
        secure_clear_bytes(data, bytes);
    }
};

inline MlsStatus mls_status(const KdfResult& result) noexcept {
    return result.ok() ? MlsStatus::ok : MlsStatus::crypto_failure;
}
inline MlsStatus mls_status(const P256Result& result) noexcept {
    return result.ok()                              ? MlsStatus::ok
           : result.error == P256Error::invalid_key ? MlsStatus::malformed
                                                    : MlsStatus::crypto_failure;
}
inline MlsStatus mls_status(const MlsSignatureResult& result) noexcept {
    return result.ok()                                              ? MlsStatus::ok
           : result.error == MlsSignatureError::malformed_signature ? MlsStatus::malformed
                                                                    : MlsStatus::crypto_failure;
}
// RFC 9420 DeriveSecret: ExpandWithLabel(secret, label, "", 32). Both
// helpers stage the digest, so output may alias an input (tree hashing does).
template <u32 N>
MlsStatus mls_derive_secret(KdfSha256Workspace& kdf, const u8* secret, const char (&label)[N],
                            u8* output) noexcept {
    u8 derived[kMlsHashBytes];
    const auto status = mls_status(
        mls_expand_label(kdf, {secret, kMlsHashBytes}, text_bytes(label), {}, {derived, kMlsHashBytes}));
    if (status == MlsStatus::ok)
        copy_bytes(output, derived, kMlsHashBytes);
    secure_clear_bytes(derived, sizeof(derived));
    return status;
}
inline MlsStatus mls_hash(KdfSha256Workspace& kdf, ConstSlice<ConstSlice<u8>> parts, u8* output) noexcept {
    u8 digest[kMlsHashBytes];
    const auto status = mls_status(sha256(kdf, parts, {digest, kMlsHashBytes}));
    if (status == MlsStatus::ok)
        copy_bytes(output, digest, kMlsHashBytes);
    return status;
}
// VerifyWithLabel against an encoded P-256 point, importing it for one call.
MlsStatus mls_verify_point(KdfSha256Workspace& kdf, const u8* signature_key, ConstSlice<u8> label,
                           ConstSlice<u8> a, ConstSlice<u8> b, ConstSlice<u8> signature) noexcept;
// TreeKEM node key from a path secret: node_secret = DeriveSecret(path, "node").
MlsStatus mls_node_key(KdfSha256Workspace& kdf, const u8* path_secret, P256Key* key,
                       u8* public_key) noexcept;
// The group's eight-byte id and the out-of-band LeafNode member binding.
inline u32 mls_leaf_binding(const u8* group_id, u32 leaf_index, u8* output) noexcept {
    output[0] = kMlsGroupIdBytes;
    copy_bytes(output + 1, group_id, kMlsGroupIdBytes);
    for (u32 i = 0; i != 4; ++i)
        output[9 + i] = static_cast<u8>(leaf_index >> (24 - 8 * i));
    return 13;
}
} // namespace maicord
