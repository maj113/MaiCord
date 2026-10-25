#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

inline constexpr u64 kHash64Seed = 14'695'981'039'346'656'037ull;
inline constexpr u64 kHash64Prime = 1'099'511'628'211ull;

__forceinline constexpr u64 hash64_mix(u64 hash, u64 value) noexcept {
    return (hash ^ value) * kHash64Prime;
}

__declspec(noinline) inline u64 hash64_bytes(u64 hash, const void* source, u32 bytes) noexcept {
    if (!source && bytes != 0) {
        return 0;
    }
    const auto* input = static_cast<const u8*>(source);
    for (u32 index = 0; index < bytes; ++index) {
        hash = hash64_mix(hash, input[index]);
    }
    return hash;
}

// 32-bit FNV-1a: the checksum of the small persisted state files.
inline u32 hash32_bytes(const u8* data, u32 bytes) noexcept {
    u32 hash = 2166136261u;
    for (u32 index = 0; index < bytes; ++index) {
        hash = (hash ^ data[index]) * 16777619u;
    }
    return hash;
}

__forceinline u64 hash64_counted_bytes(u64 hash, const void* source, u32 bytes) noexcept {
    return hash64_mix(hash64_bytes(hash, source, bytes), bytes);
}

} // namespace maicord
