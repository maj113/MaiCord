#pragma once

#include "maicord/core/json_reader.hpp"

namespace maicord {

inline constexpr u32 kMediaCatalogMaximumQueryBytes = 256;
inline constexpr u32 kMediaCatalogMinimumGifPageItems = 20;
inline constexpr u32 kMediaCatalogMaximumPageItems = 50;
inline constexpr u32 kMediaCatalogMaximumStickerItems = 4096;
inline constexpr u32 kMediaCatalogMaximumSuggestions = 5;
inline constexpr u32 kMediaCatalogMaximumTargetBytes = 1024;
inline constexpr u32 kMediaCatalogMaximumResponseBytes = 256 * 1024;
inline constexpr u32 kMediaCatalogMaximumStickerResponseBytes = 1024 * 1024;
inline constexpr u32 kMediaCatalogMaximumUrlBytes = 2048;
inline constexpr u32 kMediaCatalogMaximumIdBytes = 128;
inline constexpr u32 kMediaCatalogMaximumNameBytes = 256;
inline constexpr u32 kMediaCatalogMaximumTextBytes = 128 * 1024;
inline constexpr u32 kMediaCatalogMaximumStickerTextBytes = 512 * 1024;
inline constexpr u32 kMediaCatalogMaximumDimension = 16384;
inline constexpr u32 kMediaCatalogStructuralCapacity = 128 * 1024;

enum class MediaCatalogOperation : u8 {
    gif_categories,
    gif_trending,
    gif_search,
    gif_suggestions,
    sticker_packs,
    guild_stickers,
};
// The catalog offers these two formats. Native previews default to
// its tinywebp path; selecting a format does not change the item's send URL.
enum class MediaCatalogFormat : u8 {
    tiny_webp,
    webm
};
enum class MediaCatalogItemKind : u8 {
    category,
    gif,
    suggestion,
    sticker
};

struct MediaCatalogQuery {
    MediaCatalogOperation operation;
    Utf8View query;
    Utf8View locale;
    u64 guild_id;
    u32 limit;
    MediaCatalogFormat format;
};

// These views belong to caller-provided output text, never the response body.
// Provider IDs are opaque strings. Allocate monotonic gallery/source IDs in
// the scoped application owner; a provider ID or URL hash is not a lifetime.
struct MediaCatalogItem {
    Utf8View provider_id;
    Utf8View name;
    Utf8View description;
    Utf8View tags;
    Utf8View pack_name;
    Utf8View preview_url;
    Utf8View gif_url;
    Utf8View send_url;
    u64 sticker_id;
    u64 guild_id;
    u64 pack_id;
    u32 width;
    u32 height;
    MediaCatalogItemKind kind;
    u8 sticker_format;
    bool available;
    // The pack's cover_sticker_id (its icon in the picker rail).
    bool cover;
};

enum class MediaCatalogError : u8 {
    none,
    invalid_input,
    invalid_query,
    capacity,
    scan,
    json,
    schema,
    text_capacity,
};
struct MediaCatalogWriteResult {
    u32 bytes;
    MediaCatalogError error;
    constexpr bool ok() const noexcept {
        return error == MediaCatalogError::none;
    }
};
struct MediaCatalogDecodeResult {
    u32 item_count;
    u32 source_count;
    u32 next_index;
    u32 text_bytes;
    u32 error_offset;
    MediaCatalogError error;
    JsonScanError scan_error;
    JsonReadError read_error;
    bool truncated;
    constexpr bool ok() const noexcept {
        return error == MediaCatalogError::none;
    }
};

// Whether a server sticker can be sent in a channel, as in the official
// picker: its own server always; elsewhere (a DM, or a channel granting Use
// External Stickers) only with Nitro or Nitro Basic, else it shows locked.
// A sticker its server can no longer use (lost boosts) shows unavailable.
enum class StickerSendability : u8 {
    sendable,
    premium,
    unavailable,
    hidden,
};
StickerSendability guild_sticker_sendability(u64 sticker_guild_id, u64 channel_guild_id, bool available,
                                             bool external_permission, u8 premium_type) noexcept;

MediaCatalogWriteResult media_catalog_write_target(const MediaCatalogQuery& query,
                                                   Slice<u8> output) noexcept;
// Measures the validated owned representation without touching item/text
// output. A zero limit selects 50 GIF records or 4096 sticker records, so full
// packs and guild catalogs can be allocated exactly on use.
MediaCatalogDecodeResult media_catalog_measure(MediaCatalogOperation operation, Utf8View input,
                                               Slice<u32> structural_scratch, u32 maximum_depth,
                                               u32 limit = 0, u32 first_item = 0) noexcept;
// One bounded structural index, nested indexed visitors, and two passes:
// validate/measure first, then publish owned records atomically. Excess valid
// records are reported as truncated; unknown provider metadata is ignored.
// Outputs/scratch/input must be disjoint. No URLs are admitted for network
// fetching here; the existing media transport's admission policy still applies.
MediaCatalogDecodeResult media_catalog_decode(MediaCatalogOperation operation, Utf8View input,
                                              Slice<u32> structural_scratch, u32 maximum_depth,
                                              Slice<MediaCatalogItem> items, Slice<char> text_output,
                                              u32 limit = 0, u32 first_item = 0) noexcept;

} // namespace maicord
