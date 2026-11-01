#pragma once

#include "maicord/core/base.hpp"

// Identity types only; the key constructors and validation are in asset.hpp.
namespace maicord {

enum class MediaAssetKind : u16 {
    none,
    guild_icon,
    custom_emoji,
    sticker,
    attachment,
    embed,
    user_avatar,
    profile_image,
    regional_flag,
    catalog_preview,
    local_draft_preview,
    // Group DM icon; same hash-variant identity as guild icons and user avatars.
    channel_icon,
    // Latest decoded call video picture: user ID and RTP SSRC, never fetched.
    call_video,
};

enum MediaAssetFlags : u16 {
    media_asset_flag_none = 0,
    media_asset_flag_animated = 1u << 0,
};

enum class MediaAttachmentFormat : u32 {
    unknown,
    png,
    webp,
    jpeg,
    mp4,
    gif,
    lottie,
    ogg_opus = 7,
    mp3 = 8,
    wav = 9,
    m4a = 10, // AAC in an MP4/QuickTime audio-only container
};

enum class MediaEmbedResource : u8 {
    none,
    image,
    thumbnail,
    video,
};

// Presentation-scale identity for large static message media. `original` is the
// identity for callers that make no viewport-aware requests; the other tiers are
// 64 through 2048 physical pixels, limited by presentation policy to the
// codec's supported extent.
enum class MediaResolutionTier : u8 {
    original,
    px64,
    px128,
    px256,
    px512,
    px1024,
    px2048,
};

// Stable identity shared by network, encoded-cache, decode, and GPU-atlas
// stages. Equality is exact; no truncated hash is an identity.
struct MediaAssetKey {
    u64 id;
    u32 variant;
    MediaAssetKind kind;
    u16 flags;
};

constexpr bool media_asset_key_equal(MediaAssetKey left, MediaAssetKey right) noexcept {
    return left.id == right.id && left.variant == right.variant && left.kind == right.kind &&
           left.flags == right.flags;
}

} // namespace maicord
