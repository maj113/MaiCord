#pragma once

#include "maicord/platform/crypto/hkdf_sha256.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
namespace kdf_internal {
namespace {

// Internal linkage and no inline hint: the functions are what the Windows file's own anonymous namespace held, so a
// compiler sees them exactly as before.
KdfResult result(KdfError error, u32 bytes = 0, i32 status = 0) noexcept {
    return {bytes, status, error};
}

// The argument and alias rules of every hash call, shared by the Windows and Linux backends: they run before
// any provider is touched, and the backends add only the keyed-hash primitive.
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

} // namespace
} // namespace kdf_internal
} // namespace maicord
