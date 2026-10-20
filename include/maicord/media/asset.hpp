#pragma once

#include "maicord/core/base.hpp"

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

constexpr bool media_attachment_format_audio_supported(MediaAttachmentFormat format) noexcept {
    return format == MediaAttachmentFormat::ogg_opus || format == MediaAttachmentFormat::mp3 ||
           format == MediaAttachmentFormat::wav || format == MediaAttachmentFormat::m4a;
}

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

inline constexpr u32 kMediaAttachmentFormatMask = 0x7u;
// Attachment-only high format bit. It does not overlap the resolution and Search
// revision bits; this flag never belongs to embed identities.
inline constexpr u16 kMediaAttachmentFormatExtensionFlag = 1u << 1;
inline constexpr u32 kMediaAttachmentResolutionShift = 3;
inline constexpr u32 kMediaAttachmentResolutionMask = 0x38u;
inline constexpr u32 kMediaAttachmentVariantMask =
    kMediaAttachmentFormatMask | kMediaAttachmentResolutionMask;
// A Search result never reuses an identity during the process lifetime.
// Attachment format/tier use the low six bits; the remaining bits hold the
// Search revision, without hashing or truncation.
inline constexpr u16 kMediaSearchFlag = 0x4000u;
inline constexpr u32 kMediaSearchRevisionMaximum = 0x03ffffffu;
inline constexpr u32 kMediaSearchAttachmentRevisionShift = 6;

inline constexpr u16 kMediaEmbedIndexShift = 1;
inline constexpr u16 kMediaEmbedIndexMask = 0x1Eu;
inline constexpr u16 kMediaEmbedResourceShift = 5;
inline constexpr u16 kMediaEmbedResourceMask = 0x60u;
inline constexpr u16 kMediaEmbedFormatShift = 7;
inline constexpr u16 kMediaEmbedFormatMask = 0x380u;
inline constexpr u16 kMediaEmbedResolutionShift = 10;
inline constexpr u16 kMediaEmbedResolutionMask = 0x1C00u;
// The same outer message owns independently indexed forwarded embed content.
inline constexpr u16 kMediaEmbedSnapshotFlag = 0x2000u;
inline constexpr u16 kMediaEmbedFlagsMask = media_asset_flag_animated | kMediaEmbedIndexMask |
                                            kMediaEmbedResourceMask | kMediaEmbedFormatMask |
                                            kMediaEmbedResolutionMask | kMediaEmbedSnapshotFlag;
inline constexpr u16 kMediaCatalogFormatShift = 1;
inline constexpr u16 kMediaCatalogFormatMask = 0xEu;

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

constexpr MediaAttachmentFormat media_asset_attachment_format(MediaAssetKey key) noexcept {
    const u32 extension =
        key.kind == MediaAssetKind::attachment && (key.flags & kMediaAttachmentFormatExtensionFlag) ? 8u
                                                                                                    : 0u;
    return static_cast<MediaAttachmentFormat>((key.variant & kMediaAttachmentFormatMask) | extension);
}

constexpr u64 media_asset_hash_mix(u64 value) noexcept {
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

constexpr u64 media_asset_hash(MediaAssetKey key) noexcept {
    const u64 tail = static_cast<u64>(key.variant) | (static_cast<u64>(key.flags) << 32) |
                     (static_cast<u64>(key.kind) << 48);
    return media_asset_hash_mix(key.id ^ media_asset_hash_mix(tail));
}

__declspec(noinline) constexpr bool media_asset_key_valid(MediaAssetKey key) noexcept {
    if (key.id == 0) {
        return false;
    }
    switch (key.kind) {
    case MediaAssetKind::local_draft_preview: {
        const auto format = static_cast<MediaAttachmentFormat>((key.flags & kMediaCatalogFormatMask) >>
                                                               kMediaCatalogFormatShift);
        // MP4/MOV first-frame posters exist only for composer thumbnails,
        // whose variant carries the high thumbnail bit.
        return key.variant != 0 && (key.flags & ~kMediaCatalogFormatMask) == 0 &&
               (format == MediaAttachmentFormat::png || format == MediaAttachmentFormat::webp ||
                format == MediaAttachmentFormat::jpeg || format == MediaAttachmentFormat::gif ||
                (format == MediaAttachmentFormat::mp4 && (key.variant & 0x80000000u) != 0));
    }
    case MediaAssetKind::catalog_preview: {
        const auto format = static_cast<MediaAttachmentFormat>((key.flags & kMediaCatalogFormatMask) >>
                                                               kMediaCatalogFormatShift);
        // MP4 previews are muted looping tiles and always animated.
        return key.variant != 0 &&
               (key.flags & ~(kMediaCatalogFormatMask | media_asset_flag_animated)) == 0 &&
               (format == MediaAttachmentFormat::png || format == MediaAttachmentFormat::webp ||
                format == MediaAttachmentFormat::gif || format == MediaAttachmentFormat::lottie ||
                format == MediaAttachmentFormat::mp4) &&
               (format == MediaAttachmentFormat::png || format == MediaAttachmentFormat::lottie ||
                (key.flags & media_asset_flag_animated) != 0);
    }
    case MediaAssetKind::regional_flag:
        return key.id <= 679 && key.variant == 0 && key.flags == media_asset_flag_none;
    case MediaAssetKind::guild_icon:
    case MediaAssetKind::user_avatar:
    case MediaAssetKind::channel_icon:
        return key.variant != 0 && (key.flags & ~media_asset_flag_animated) == 0;
    case MediaAssetKind::custom_emoji:
        return key.variant == 0 && (key.flags & ~media_asset_flag_animated) == 0;
    case MediaAssetKind::call_video:
        return key.variant != 0 && key.flags == media_asset_flag_none;
    case MediaAssetKind::sticker: {
        const bool animated = key.variant >= 2 && key.variant <= 4;
        return key.variant >= 1 && key.variant <= 4 &&
               ((key.flags & media_asset_flag_animated) != 0) == animated;
    }
    case MediaAssetKind::attachment:
        return ((key.flags & kMediaSearchFlag) ? (key.variant >> kMediaSearchAttachmentRevisionShift) != 0
                                               : (key.variant & ~kMediaAttachmentVariantMask) == 0) &&
               (media_asset_attachment_format(key) <= MediaAttachmentFormat::gif ||
                media_attachment_format_audio_supported(media_asset_attachment_format(key))) &&
               ((key.variant & kMediaAttachmentResolutionMask) >> kMediaAttachmentResolutionShift) <=
                   static_cast<u32>(MediaResolutionTier::px2048) &&
               (!media_attachment_format_audio_supported(media_asset_attachment_format(key)) ||
                (key.variant & kMediaAttachmentResolutionMask) == 0) &&
               (key.flags &
                ~(media_asset_flag_animated | kMediaAttachmentFormatExtensionFlag | kMediaSearchFlag |
                  ((key.flags & kMediaSearchFlag) ? kMediaEmbedSnapshotFlag : 0u))) == 0 &&
               ((key.flags & media_asset_flag_animated) == 0 ||
                media_asset_attachment_format(key) == MediaAttachmentFormat::webp ||
                media_asset_attachment_format(key) == MediaAttachmentFormat::gif ||
                media_asset_attachment_format(key) == MediaAttachmentFormat::mp4);
    case MediaAssetKind::profile_image:
    case MediaAssetKind::embed: {
        const u16 index = static_cast<u16>((key.flags & kMediaEmbedIndexMask) >> kMediaEmbedIndexShift);
        const u16 resource =
            static_cast<u16>((key.flags & kMediaEmbedResourceMask) >> kMediaEmbedResourceShift);
        const u16 format = static_cast<u16>((key.flags & kMediaEmbedFormatMask) >> kMediaEmbedFormatShift);
        const u16 resolution =
            static_cast<u16>((key.flags & kMediaEmbedResolutionMask) >> kMediaEmbedResolutionShift);
        return key.variant != 0 && index < 10 &&
               (key.kind == MediaAssetKind::embed ||
                (key.flags & (kMediaEmbedSnapshotFlag | kMediaSearchFlag)) == 0) &&
               (!(key.flags & kMediaSearchFlag) || key.variant <= kMediaSearchRevisionMaximum) &&
               resource >= static_cast<u16>(MediaEmbedResource::image) &&
               resource <= static_cast<u16>(MediaEmbedResource::video) &&
               format >= static_cast<u16>(MediaAttachmentFormat::png) &&
               format <= static_cast<u16>(MediaAttachmentFormat::gif) &&
               resolution <= static_cast<u16>(MediaResolutionTier::px2048) &&
               (key.flags & ~(kMediaEmbedFlagsMask | kMediaSearchFlag)) == 0;
    }
    default:
        return false;
    }
}

constexpr MediaAssetKey media_asset_guild_icon(u64 guild_id, u32 icon_variant, bool animated) noexcept {
    return MediaAssetKey{guild_id, icon_variant, MediaAssetKind::guild_icon,
                         animated ? media_asset_flag_animated : media_asset_flag_none};
}

constexpr MediaAssetKey media_asset_user_avatar(u64 user_id, u32 avatar_variant, bool animated) noexcept {
    return MediaAssetKey{user_id, avatar_variant, MediaAssetKind::user_avatar,
                         animated ? media_asset_flag_animated : media_asset_flag_none};
}

constexpr MediaAssetKey media_asset_channel_icon(u64 channel_id, u32 icon_variant, bool animated) noexcept {
    return MediaAssetKey{channel_id, icon_variant, MediaAssetKind::channel_icon,
                         animated ? media_asset_flag_animated : media_asset_flag_none};
}

constexpr MediaAssetKey media_asset_custom_emoji(u64 id, bool animated) noexcept {
    return MediaAssetKey{id, 0, MediaAssetKind::custom_emoji,
                         animated ? media_asset_flag_animated : media_asset_flag_none};
}

constexpr MediaAssetKey media_asset_regional_flag(u16 sequence) noexcept {
    return MediaAssetKey{sequence, 0, MediaAssetKind::regional_flag, media_asset_flag_none};
}

// Presented into the preview atlas by the call view; a new SSRC is a new picture.
constexpr MediaAssetKey media_asset_call_video(u64 user_id, u32 ssrc) noexcept {
    return MediaAssetKey{user_id, ssrc, MediaAssetKind::call_video, media_asset_flag_none};
}

// Application-assigned opaque item lifetime and source generation. Provider
// strings and URLs never become cache identities through a hash or coercion.
constexpr MediaAssetKey media_asset_catalog_preview(
    u64 source_item_id, u32 generation, MediaAttachmentFormat format = MediaAttachmentFormat::webp,
    bool animated = false) noexcept {
    return MediaAssetKey{source_item_id, generation, MediaAssetKind::catalog_preview,
                         static_cast<u16>((static_cast<u16>(format) << kMediaCatalogFormatShift) |
                                          ((animated || format == MediaAttachmentFormat::webp ||
                                            format == MediaAttachmentFormat::gif ||
                                            format == MediaAttachmentFormat::mp4)
                                               ? media_asset_flag_animated
                                               : media_asset_flag_none))};
}

constexpr MediaAttachmentFormat media_asset_catalog_format(MediaAssetKey key) noexcept {
    return static_cast<MediaAttachmentFormat>((key.flags & kMediaCatalogFormatMask) >>
                                              kMediaCatalogFormatShift);
}

// Opaque draft handle plus a process-lifetime preview generation. Neither a
// filename nor a filesystem identity is coerced into an atlas identity.
constexpr MediaAssetKey media_asset_local_draft_preview(u64 id, u32 generation,
                                                        MediaAttachmentFormat format) noexcept {
    const MediaAssetKey key{id, generation, MediaAssetKind::local_draft_preview,
                            static_cast<u16>(static_cast<u16>(format) << kMediaCatalogFormatShift)};
    return media_asset_key_valid(key) ? key : MediaAssetKey{};
}

constexpr MediaAssetKey media_asset_sticker(u64 id, u32 format_type) noexcept {
    return MediaAssetKey{id, format_type, MediaAssetKind::sticker,
                         format_type == 2 || format_type == 3 || format_type == 4
                             ? media_asset_flag_animated
                             : media_asset_flag_none};
}

constexpr MediaAssetKey media_asset_attachment(
    u64 id, MediaAttachmentFormat format = MediaAttachmentFormat::unknown,
    MediaResolutionTier resolution = MediaResolutionTier::original, bool animated = false) noexcept {
    // Native MP4 keeps one encoded source and scales its GPU surface at paint
    // time. A viewport change must not create another decoder identity.
    if (format > MediaAttachmentFormat::gif && !media_attachment_format_audio_supported(format))
        return {};
    if (media_attachment_format_audio_supported(format))
        resolution = MediaResolutionTier::original;
    else if (format == MediaAttachmentFormat::mp4)
        resolution = MediaResolutionTier::px512;
    const u32 variant = (static_cast<u32>(format) & kMediaAttachmentFormatMask) |
                        ((static_cast<u32>(resolution) << kMediaAttachmentResolutionShift) &
                         kMediaAttachmentResolutionMask);
    const u16 format_flag = static_cast<u32>(format) >= 8 ? kMediaAttachmentFormatExtensionFlag : 0;
    return MediaAssetKey{id, variant, MediaAssetKind::attachment,
                         static_cast<u16>(format_flag | (animated || format == MediaAttachmentFormat::gif ||
                                                                 format == MediaAttachmentFormat::mp4
                                                             ? media_asset_flag_animated
                                                             : media_asset_flag_none))};
}

constexpr MediaAssetKey media_asset_embed(u64 message_id, u32 message_revision, u8 embed_index,
                                          MediaEmbedResource resource, MediaAttachmentFormat format,
                                          bool animated = false,
                                          MediaResolutionTier resolution = MediaResolutionTier::original,
                                          bool snapshot = false) noexcept {
    if (format == MediaAttachmentFormat::mp4)
        resolution = MediaResolutionTier::px512;
    const u16 flags = static_cast<u16>(
        (animated ? media_asset_flag_animated : media_asset_flag_none) |
        (snapshot ? kMediaEmbedSnapshotFlag : 0u) |
        ((static_cast<u16>(embed_index) << kMediaEmbedIndexShift) & kMediaEmbedIndexMask) |
        ((static_cast<u16>(resource) << kMediaEmbedResourceShift) & kMediaEmbedResourceMask) |
        ((static_cast<u16>(format) << kMediaEmbedFormatShift) & kMediaEmbedFormatMask) |
        ((static_cast<u16>(resolution) << kMediaEmbedResolutionShift) & kMediaEmbedResolutionMask));
    return MediaAssetKey{message_id, message_revision, MediaAssetKind::embed, flags};
}

constexpr MediaAssetKey media_asset_search_attachment(u64 attachment_id, MediaAttachmentFormat format,
                                                      MediaResolutionTier resolution,
                                                      u32 publication_revision,
                                                      bool snapshot = false) noexcept {
    if (!publication_revision || publication_revision > kMediaSearchRevisionMaximum ||
        (static_cast<u32>(format) > static_cast<u32>(MediaAttachmentFormat::gif) &&
         !media_attachment_format_audio_supported(format)) ||
        static_cast<u32>(resolution) > static_cast<u32>(MediaResolutionTier::px2048))
        return {};
    auto key = media_asset_attachment(attachment_id, format, resolution);
    key.variant |= publication_revision << kMediaSearchAttachmentRevisionShift;
    key.flags |= kMediaSearchFlag;
    if (snapshot)
        key.flags |= kMediaEmbedSnapshotFlag;
    return media_asset_key_valid(key) ? key : MediaAssetKey{};
}

constexpr MediaAssetKey media_asset_search_embed(
    u64 message_id, u32 publication_revision, u8 embed_index, MediaEmbedResource resource,
    MediaAttachmentFormat format, bool animated = false,
    MediaResolutionTier resolution = MediaResolutionTier::original, bool snapshot = false) noexcept {
    if (!publication_revision || publication_revision > kMediaSearchRevisionMaximum || embed_index >= 10 ||
        resource < MediaEmbedResource::image || resource > MediaEmbedResource::video ||
        format < MediaAttachmentFormat::png || format > MediaAttachmentFormat::gif ||
        resolution > MediaResolutionTier::px2048)
        return {};
    auto key = media_asset_embed(message_id, publication_revision, embed_index, resource, format, animated,
                                 resolution, snapshot);
    key.flags |= kMediaSearchFlag;
    return media_asset_key_valid(key) ? key : MediaAssetKey{};
}

constexpr bool media_asset_search(MediaAssetKey key) noexcept {
    return (key.kind == MediaAssetKind::attachment || key.kind == MediaAssetKind::embed) &&
           (key.flags & kMediaSearchFlag) != 0;
}

constexpr u32 media_asset_search_revision(MediaAssetKey key) noexcept {
    return !media_asset_search(key)                 ? 0
           : key.kind == MediaAssetKind::attachment ? key.variant >> kMediaSearchAttachmentRevisionShift
                                                    : key.variant;
}

constexpr bool media_asset_search_snapshot(MediaAssetKey key) noexcept {
    return media_asset_search(key) && (key.flags & kMediaEmbedSnapshotFlag) != 0;
}

constexpr u8 media_asset_embed_index(MediaAssetKey key) noexcept {
    return static_cast<u8>((key.flags & kMediaEmbedIndexMask) >> kMediaEmbedIndexShift);
}

constexpr bool media_asset_embed_snapshot(MediaAssetKey key) noexcept {
    return key.kind == MediaAssetKind::embed && (key.flags & kMediaEmbedSnapshotFlag) != 0;
}

constexpr MediaEmbedResource media_asset_embed_resource(MediaAssetKey key) noexcept {
    return static_cast<MediaEmbedResource>((key.flags & kMediaEmbedResourceMask) >>
                                           kMediaEmbedResourceShift);
}

constexpr MediaAttachmentFormat media_asset_embed_format(MediaAssetKey key) noexcept {
    return static_cast<MediaAttachmentFormat>((key.flags & kMediaEmbedFormatMask) >>
                                              kMediaEmbedFormatShift);
}

constexpr MediaResolutionTier media_asset_resolution_tier(MediaAssetKey key) noexcept {
    if (key.kind == MediaAssetKind::catalog_preview || key.kind == MediaAssetKind::local_draft_preview)
        return MediaResolutionTier::px256;
    if (key.kind == MediaAssetKind::attachment) {
        return static_cast<MediaResolutionTier>((key.variant & kMediaAttachmentResolutionMask) >>
                                                kMediaAttachmentResolutionShift);
    }
    if (key.kind == MediaAssetKind::embed || key.kind == MediaAssetKind::profile_image) {
        return static_cast<MediaResolutionTier>((key.flags & kMediaEmbedResolutionMask) >>
                                                kMediaEmbedResolutionShift);
    }
    return MediaResolutionTier::original;
}

constexpr bool media_asset_large_preview(MediaAssetKey key) noexcept {
    return key.kind == MediaAssetKind::attachment || key.kind == MediaAssetKind::embed ||
           key.kind == MediaAssetKind::catalog_preview || key.kind == MediaAssetKind::local_draft_preview;
}

static_assert(sizeof(MediaAssetKey) == 16);

} // namespace maicord
