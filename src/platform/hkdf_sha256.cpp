#include "maicord/platform/hkdf_sha256.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/crypto.hpp"

namespace maicord {
namespace {
KdfResult result(KdfError error, u32 bytes = 0, i32 status = 0) noexcept {
    return {bytes, status, error};
}
KdfResult validate(const KdfSha256Workspace& workspace, ConstSlice<u8> key,
                   ConstSlice<ConstSlice<u8>> parts, Slice<u8> output, u32 written) noexcept {
    if (!workspace.storage.data || !workspace.storage.locked)
        return result(KdfError::not_initialized);
    if (!byte_range_valid(key) || !byte_range_valid({output.data, output.count}) ||
        parts.count > ~u32{0} / sizeof(ConstSlice<u8>))
        return result(KdfError::invalid_input);
    const ConstSlice<u8> descriptors{reinterpret_cast<const u8*>(parts.data),
                                     parts.count * static_cast<u32>(sizeof(ConstSlice<u8>))};
    if (!byte_range_valid(descriptors))
        return result(KdfError::invalid_input);
    if (output.count < written)
        return result(KdfError::output_capacity);
    const ConstSlice<u8> destination{output.data, written};
    const ConstSlice<u8> owner{reinterpret_cast<const u8*>(&workspace), sizeof(workspace)};
    const ConstSlice<u8> storage{workspace.storage.data, workspace.storage.capacity};
    const ConstSlice<u8> spans[]{key, descriptors, destination};
    for (const auto span : spans)
        if (byte_ranges_overlap(span, owner) || byte_ranges_overlap(span, storage))
            return result(KdfError::invalid_input);
    if (byte_ranges_overlap(destination, key) || byte_ranges_overlap(destination, descriptors))
        return result(KdfError::invalid_input);
    for (u32 i = 0; i != parts.count; ++i)
        if (!byte_range_valid(parts.data[i]) || byte_ranges_overlap(parts.data[i], owner) ||
            byte_ranges_overlap(parts.data[i], storage) || byte_ranges_overlap(parts.data[i], destination))
            return result(KdfError::invalid_input);
    return result(KdfError::none);
}
NTSTATUS feed(void* hash, ConstSlice<u8> bytes) noexcept {
    return bytes.count ? BCryptHashData(hash, const_cast<u8*>(bytes.data), bytes.count, 0) : 0;
}
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
    void* hash = nullptr;
    // An explicit zero-length key still means HMAC, not an unkeyed hash.
    u8 empty = 0;
    NTSTATUS status = BCryptCreateHash(keyed ? BCRYPT_HMAC_SHA256_ALG_HANDLE : BCRYPT_SHA256_ALG_HANDLE,
                                       &hash, workspace.storage.data, workspace.storage.capacity,
                                       keyed ? (key.count ? const_cast<u8*>(key.data) : &empty) : nullptr,
                                       key.count, BCRYPT_HASH_REUSABLE_FLAG);
    u8 block[32]{};
    u32 done = 0;
    if (status >= 0) {
        for (u32 index = 1; done != wanted; ++index) {
            if (expand && done)
                status = feed(hash, {block, 32});
            if (status >= 0 && prefix.count)
                status = feed(hash, prefix);
            for (u32 part = 0; status >= 0 && part != parts.count; ++part)
                status = feed(hash, parts.data[part]);
            const u8 counter = static_cast<u8>(index);
            if (expand && status >= 0)
                status = feed(hash, {&counter, 1});
            if (status >= 0)
                status = BCryptFinishHash(hash, block, 32, 0);
            if (status < 0)
                break;
            const u32 bytes = minimum(32u, wanted - done);
            copy_bytes(output.data + done, block, bytes);
            done += bytes;
        }
    }
    if (hash)
        BCryptDestroyHash(hash);
    SecureZeroMemory(block, sizeof(block));
    SecureZeroMemory(workspace.storage.data, workspace.storage.capacity);
    if (status < 0) {
        SecureZeroMemory(output.data, wanted);
        return result(KdfError::platform_failure, 0, status);
    }
    return result(KdfError::none, wanted);
}
} // namespace
KdfResult kdf_sha256_initialize(KdfSha256Workspace* workspace) noexcept {
    if (!workspace || workspace->storage.data || workspace->storage.locked)
        return result(KdfError::invalid_input);
    ULONG bytes = 0;
    const BCRYPT_ALG_HANDLE algorithms[]{BCRYPT_HMAC_SHA256_ALG_HANDLE, BCRYPT_SHA256_ALG_HANDLE};
    for (auto algorithm : algorithms) {
        ULONG needed = 0, returned = 0;
        const auto status =
            BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&needed),
                              sizeof(needed), &returned, 0);
        if (status < 0 || returned != sizeof(needed) || !needed)
            return result(KdfError::platform_failure, 0, status);
        bytes = maximum(bytes, needed);
    }
    const auto allocated = secure_buffer_initialize(&workspace->storage, bytes);
    if (!allocated.ok())
        return result(KdfError::storage, 0, static_cast<i32>(allocated.native_error));
    workspace->storage.bytes = bytes;
    return result(KdfError::none);
}
void kdf_sha256_release(KdfSha256Workspace* workspace) noexcept {
    if (workspace)
        secure_buffer_release(&workspace->storage);
}
KdfResult sha256(KdfSha256Workspace& workspace, ConstSlice<ConstSlice<u8>> parts,
                 Slice<u8> output) noexcept {
    return calculate(workspace, {}, parts, output, false, false);
}
KdfResult hmac_sha256(KdfSha256Workspace& workspace, ConstSlice<u8> key, ConstSlice<ConstSlice<u8>> parts,
                      Slice<u8> output) noexcept {
    return calculate(workspace, key, parts, output, false);
}
KdfResult hmac_sha256_prefixed(KdfSha256Workspace& workspace, ConstSlice<u8> key, ConstSlice<u8> prefix,
                               ConstSlice<ConstSlice<u8>> parts, Slice<u8> output) noexcept {
    return calculate(workspace, key, parts, output, false, true, prefix);
}
KdfResult hkdf_sha256_extract(KdfSha256Workspace& workspace, ConstSlice<u8> salt,
                              ConstSlice<u8> input_key_material, Slice<u8> output) noexcept {
    // HMAC with an empty salt is equivalent to HashLen zero bytes (RFC 5869).
    return hmac_sha256(workspace, salt, {&input_key_material, 1}, output);
}
KdfResult hkdf_sha256_expand(KdfSha256Workspace& workspace, ConstSlice<u8> prk,
                             ConstSlice<ConstSlice<u8>> info, Slice<u8> output) noexcept {
    if (!prk.count || output.count > 255 * 32)
        return result(KdfError::invalid_input);
    return calculate(workspace, prk, info, output, true);
}
} // namespace maicord
