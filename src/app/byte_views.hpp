#pragma once
#include "maicord/core/byte_range.hpp"
#include "maicord/platform/hkdf_sha256.hpp"

namespace maicord {
// The object representation of a value, for alias checks against caller buffers.
template <typename T> ConstSlice<u8> object_bytes(const T& value) noexcept {
    return {reinterpret_cast<const u8*>(&value), sizeof(value)};
}
// True when span overlaps the KDF workspace object or its locked storage.
inline bool aliases_workspace(const KdfSha256Workspace& workspace, ConstSlice<u8> span) noexcept {
    return byte_ranges_overlap(span, object_bytes(workspace)) ||
           byte_ranges_overlap(span, {workspace.storage.data, workspace.storage.capacity});
}
// A string literal without its terminator.
template <u32 N> constexpr ConstSlice<u8> text_bytes(const char (&value)[N]) noexcept {
    return {reinterpret_cast<const u8*>(value), N - 1};
}
} // namespace maicord
