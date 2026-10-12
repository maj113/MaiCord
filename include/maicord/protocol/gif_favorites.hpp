#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// Limit on the raw FavoriteGIFs group before base64, matching the official client.
inline constexpr u32 kGifFavoritesMaximumGroupBytes = 762'880;
// Local cache limits. Exceeding either rejects the whole snapshot; no partial
// result is kept or sent as a cloud write.
// The proto size limit is the raw-byte ceiling that fits the 2 MiB base64 response buffer.
inline constexpr u32 kGifFavoritesMaximumProtoBytes = 1536 * 1024;
inline constexpr u32 kGifFavoritesMaximumEntries = 65'536;
inline constexpr u32 kGifFavoritesMaximumPatchBytes = ((kGifFavoritesMaximumGroupBytes + 8) / 3) * 4 + 64;
enum class GifFavoriteFormat : i32 {
    none = 0,
    image = 1,
    video = 2
};
enum class GifFavoritesError : u8 {
    none,
    input,
    base64,
    protobuf,
    utf8,
    capacity,
    version,
    missing,
    order_overflow,
    envelope
};
// URLs stay as the original UTF-8 strings. URL validation/normalization and UI
// identifiers are handled by the callers, not by the protobuf codec.
struct GifFavoriteRecord {
    Utf8View url, src;
    u32 width, height, order;
    GifFavoriteFormat format;
    u32 entry_offset, entry_bytes, first_entry_offset;
};
struct GifFavoritesResult {
    u32 proto_bytes, group_bytes, entry_count, record_count, index_words;
    u32 client_version, server_version, data_version;
    GifFavoritesError error;
    bool group_present, versions_present, hide_tooltip;
    constexpr bool ok() const noexcept {
        return error == GifFavoritesError::none;
    }
};
constexpr u32 gif_favorites_index_words(u32 entries) noexcept {
    if (!entries || entries > kGifFavoritesMaximumEntries)
        return 0;
    u32 buckets = 1;
    while (buckets < entries * 2)
        buckets <<= 1;
    return buckets * 2;
}
constexpr u32 gif_favorites_write_scratch_bytes(u32 group_bytes, u32 entries) noexcept {
    if (group_bytes > kGifFavoritesMaximumGroupBytes || entries > kGifFavoritesMaximumEntries)
        return 0;
    const u32 index_bytes = gif_favorites_index_words(entries) * static_cast<u32>(sizeof(u32));
    return group_bytes + 6 > index_bytes ? group_bytes + 6 : index_bytes;
}
GifFavoritesResult gif_favorites_measure_proto(ConstSlice<u8>) noexcept;
GifFavoritesResult gif_favorites_measure_base64(Utf8View) noexcept;
// Validates and measures first; copies the favorite group and writes the records
// (last key wins) only after all capacities are sufficient. Record strings borrow
// the caller's owned_group bank. Records are sorted by descending saved order.
// Allocate group_bytes bytes for owned_group, entry_count record slots and
// index_words u32s, all taken from measure. All inputs/outputs must be disjoint.
// Index scratch may change on failure; owned_group and records never do.
GifFavoritesResult gif_favorites_decode_proto(ConstSlice<u8>, Slice<u8> owned_group,
                                              Slice<GifFavoriteRecord>, Slice<u32> index_scratch) noexcept;
GifFavoritesResult gif_favorites_decode_base64(Utf8View, Slice<u8> owned_group, Slice<GifFavoriteRecord>,
                                               Slice<u32> index_scratch) noexcept;
// Decode leaves a hash index in index_scratch for gif_favorites_find. Hashes only
// pick buckets; full UTF-8 equality decides membership. Keep this bank together
// with its records. index.count must be the decode result's index_words, not the bank capacity.
const GifFavoriteRecord* gif_favorites_find(ConstSlice<GifFavoriteRecord>, ConstSlice<u32> index,
                                            Utf8View url) noexcept;
enum class GifFavoriteMutationKind : u8 {
    add,
    remove,
    reorder
};
struct GifFavoriteMutation {
    Utf8View url, src;
    u32 width, height, order;
    GifFavoriteFormat format;
    GifFavoriteMutationKind kind;
};
struct GifFavoritesWriteResult {
    u32 bytes, group_bytes;
    GifFavoritesError error;
    constexpr bool ok() const noexcept {
        return error == GifFavoritesError::none;
    }
};
// Add/re-add uses max(saved order)+1; remove deletes every matching map entry;
// reorder changes only the supplied key's order. Unknown group/entry/value fields
// are preserved. The partial PATCH contains only root group2, so other settings
// are untouched. required_data_version is the server's latest dataVersion and
// guards the write; on a conflict, fetch a fresh snapshot instead of replaying.
// Scratch must hold max(resulting group bytes+6, index_words*sizeof(u32)) and may
// change on failure; JSON output never does.
GifFavoritesWriteResult gif_favorites_write_patch(ConstSlice<u8> observed_group, u32 required_data_version,
                                                  const GifFavoriteMutation&, Slice<u8> proto_scratch,
                                                  Slice<u8> json_output) noexcept;
// Accepts only the conditional envelope written above: one favoriteGifs group2,
// no versions/frecency/other root writes.
GifFavoritesError gif_favorites_validate_patch_json(Utf8View) noexcept;
} // namespace maicord
