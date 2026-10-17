#pragma once

#include "maicord/ui/text.hpp"

namespace maicord {

// Shaping results keyed by every shaping input, independent of origin, colour
// and atlas state. A hit replays only glyph placement, so pixel snapping,
// colours and selection paint stay identical to a fresh shape.
struct ShapedTextCacheRequest {
    const TextSystem* system;
    Utf8View text;
    ConstSlice<TextStyleSpan> styles;
    ConstSlice<TextHitSpan> hits;
    float maximum_width;
    float maximum_height;
    u32 dpi;
    u32 typography_revision;
    u8 wrapping;
    u8 overflow;
};

struct ShapedTextCacheStats {
    u32 hits;
    u32 misses;
    u32 inserts;
    u32 promotions;
    u32 flips;
    u32 rejected;
    u32 clears;
};

inline constexpr u32 kShapedTextCacheEntries = 512;
inline constexpr u32 kShapedTextCacheBuckets = 1024;
inline constexpr u32 kShapedTextCacheGlyphs = 8 * 1024;
inline constexpr u32 kShapedTextCacheRuns = 1024;
inline constexpr u32 kShapedTextCacheDecorations = 1024;
inline constexpr u32 kShapedTextCacheHits = 1024;
inline constexpr u32 kShapedTextCacheKeyBytes = 48 * 1024;

struct ShapedTextCacheEntry {
    u64 hash;
    TextSystem system;
    u64 owner_flags; // moved session reference
    float maximum_width, maximum_height;
    float width, width_including_trailing_whitespace, height;
    u32 line_count, dpi, typography_revision;
    u32 key_offset, text_bytes;
    u16 style_count, hit_span_count;
    u32 first_glyph, glyph_count;
    u32 first_run, run_count;
    u32 first_decoration, decoration_count;
    u32 first_hit, hit_count;
    u8 wrapping, overflow;
    bool moved; // references now belong to a newer copy
};

// Two generations: inserts fill the current one; a hit in the older one is
// copied forward, and a full current generation retires the older one.
struct ShapedTextGeneration {
    ShapedTextCacheEntry entries[kShapedTextCacheEntries];
    u16 buckets[kShapedTextCacheBuckets]; // entry index + 1
    TextGlyph glyphs[kShapedTextCacheGlyphs];
    TextRun runs[kShapedTextCacheRuns];
    TextDecoration decorations[kShapedTextCacheDecorations];
    TextHitRegion hits[kShapedTextCacheHits];
    u8 key_bytes[kShapedTextCacheKeyBytes];
    u32 entry_count, glyph_count, run_count, decoration_count, hit_count, key_byte_count;
};

struct ShapedTextCache {
    ShapedTextGeneration generations[2];
    u32 current;
    ShapedTextCacheStats stats;
};

ShapedTextCache* shaped_text_cache_create() noexcept;
// Releases every retained font reference; the text system must still be alive.
void shaped_text_cache_destroy(ShapedTextCache** cache) noexcept;
void shaped_text_cache_clear(ShapedTextCache* cache) noexcept;
// On a hit, *view borrows cache storage until the next insert or clear.
bool shaped_text_cache_find(ShapedTextCache* cache, const ShapedTextCacheRequest& request, u64* hash,
                            ShapedText* view) noexcept;
// Copies the result and moves its run/session references into the cache;
// *shaped keeps its arrays and is left without references.
bool shaped_text_cache_insert(ShapedTextCache* cache, const ShapedTextCacheRequest& request, u64 hash,
                              ShapedText* shaped) noexcept;

} // namespace maicord
