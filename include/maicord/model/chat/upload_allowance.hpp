#pragma once
#include "maicord/core/base.hpp"
namespace maicord {
struct UploadPolicy { u32 floor_bytes; u32 nitro_bytes; bool guild_250_disabled; };
inline constexpr u32 kUploadMiB = 1024u * 1024u;
// The most Discord lets any account upload in one file: Nitro's 1 GB. Bounds what a paste reads into memory.
inline constexpr u64 kUploadLargestAllowanceBytes = 1024ull * kUploadMiB;
// The largest file the account may upload to a channel as far as the client knows (premium_type is the user's, the
// tier and MAX_FILE_SIZE features bits 1/2/4 those of the channel's server). Discord's published rules: 20 MiB for
// everyone since August 2026 (File Attachments FAQ, https://support.discord.com/hc/en-us/articles/25444343291031, and
// the API reference's "Uploading Files"); Nitro Classic and Nitro Basic 50 MiB, Nitro 500 MiB (1 GiB in an
// experiment); a server at boost level 2 gives its members 50 MiB, level 3 100 MiB (Server Boosting FAQ,
// https://support.discord.com/hc/en-us/articles/360028038352). Experiments raise it beyond what the client can see,
// so it names the limit in notices and never refuses a file: the server's answer decides.
constexpr u64 upload_allowance_bytes(u8 premium_type, u8 guild_premium_tier, u8 guild_features,
                                     UploadPolicy policy) noexcept {
    u32 bytes = maximum(20u * kUploadMiB, policy.floor_bytes);
    if (premium_type == 1 || premium_type == 3) bytes = maximum(bytes, 50u * kUploadMiB);
    if (premium_type == 2) bytes = maximum(bytes, policy.nitro_bytes ? policy.nitro_bytes : 500u * kUploadMiB);
    if (guild_premium_tier == 2) bytes = maximum(bytes, 50u * kUploadMiB);
    if (guild_premium_tier >= 3) bytes = maximum(bytes, 100u * kUploadMiB);
    if (guild_features & 1) bytes = maximum(bytes, 50u * kUploadMiB);
    if (guild_features & 2) bytes = maximum(bytes, 100u * kUploadMiB);
    if ((guild_features & 4) && !policy.guild_250_disabled) bytes = maximum(bytes, 250u * kUploadMiB);
    return bytes;
}
}
