#pragma once
#include "maicord/core/base.hpp"
namespace maicord {
struct UploadPolicy { u32 floor_bytes; u32 nitro_bytes; bool guild_250_disabled; };
inline constexpr u32 kUploadMiB = 1024u * 1024u;
constexpr u64 upload_allowance_bytes(u8 premium_type, u8 guild_features, UploadPolicy policy) noexcept {
    u32 bytes = maximum(10u * kUploadMiB, policy.floor_bytes);
    if (premium_type == 1 || premium_type == 3) bytes = maximum(bytes, 50u * kUploadMiB);
    if (premium_type == 2) bytes = maximum(bytes, policy.nitro_bytes ? policy.nitro_bytes : 500u * kUploadMiB);
    if (guild_features & 1) bytes = maximum(bytes, 50u * kUploadMiB);
    if (guild_features & 2) bytes = maximum(bytes, 100u * kUploadMiB);
    if ((guild_features & 4) && !policy.guild_250_disabled) bytes = maximum(bytes, 250u * kUploadMiB);
    return bytes;
}
}
