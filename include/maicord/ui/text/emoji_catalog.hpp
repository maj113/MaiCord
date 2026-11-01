#pragma once

#include "maicord/core/utf.hpp"

namespace maicord {

inline constexpr u32 kUnicodeEmojiCatalogEntryCount = 3'780;
inline constexpr u32 kUnicodeEmojiCatalogNameBytes = 64'461;
// The catalog and shortcode entries use this shared glyph pool.
inline constexpr u32 kUnicodeEmojiCatalogGlyphBytes = 33'578;
// Bounds of the generated, fully-qualified Unicode 16 catalog. The catalog
// translation unit verifies these against every generated entry so consumers
// can size short-lived UI storage without carrying protocol-sized buffers.
inline constexpr u32 kUnicodeEmojiMaximumNameBytes = 76;
inline constexpr u32 kUnicodeEmojiMaximumGlyphBytes = 35;

enum class EmojiCategory : u8 {
    people,
    nature,
    food,
    activity,
    travel,
    objects,
    symbols,
    flags,
};

inline constexpr u32 kEmojiCategoryCount = 8;

// The shared name and glyph pools ship as one deflate stream to keep .rdata small.
// unicode_emoji_catalog(), emoji_shortcodes() and the lookups below inflate it once,
// on first use, into private read-write pages kept for the process lifetime; entries
// are reachable only through those calls, so name()/glyph() never see an uninflated
// pool. If the allocation fails, the catalog and shortcode tables are empty.
namespace emoji_pool_detail {
extern const char* catalog_names;
extern const char* catalog_glyphs;
} // namespace emoji_pool_detail

struct EmojiCatalogEntry {
    u16 name_offset_low;
    u16 glyph_offset;
    u8 name_length;
    u8 glyph_bytes;

    constexpr EmojiCatalogEntry(u32 name_offset, u16 glyph_offset, u8 name_bytes, u8 glyph_bytes) noexcept
        : name_offset_low(static_cast<u16>(name_offset)), glyph_offset(glyph_offset),
          name_length(name_bytes),
          glyph_bytes(glyph_bytes) {}

    constexpr u32 name_offset() const noexcept {
        return name_offset_low;
    }

    Utf8View name() const noexcept {
        return Utf8View{emoji_pool_detail::catalog_names + name_offset(),
                        static_cast<u32>(name_length)};
    }

    Utf8View glyph() const noexcept {
        return Utf8View{emoji_pool_detail::catalog_glyphs + glyph_offset, glyph_bytes};
    }
};

struct EmojiCatalogRange {
    u32 first;
    u32 one_past_last;
};

static_assert(kUnicodeEmojiCatalogNameBytes < (1u << 16u));
static_assert(kUnicodeEmojiCatalogGlyphBytes < (1u << 16u));
static_assert(kUnicodeEmojiMaximumNameBytes < (1u << 7u));
static_assert(kUnicodeEmojiMaximumGlyphBytes < (1u << 6u));
static_assert(kEmojiCategoryCount <= (1u << 3u));
static_assert(sizeof(EmojiCatalogEntry) == 6);

ConstSlice<EmojiCatalogEntry> unicode_emoji_catalog() noexcept;
EmojiCatalogRange emoji_catalog_category_range(EmojiCategory category) noexcept;
bool emoji_catalog_name_starts_with(Utf8View name, Utf8View query) noexcept;
bool emoji_catalog_name_contains(Utf8View name, Utf8View query) noexcept;

void emoji_alias_entry_out_of_range() noexcept; // never defined; rejects a bad constant table
// Six bytes: bit 7 of name_packed is bit 16 of the name offset (names < 128 bytes).
struct EmojiAliasEntry {
    u16 name_offset_low;
    u16 glyph_offset;
    u8 name_packed;
    u8 glyph_bytes;

    constexpr EmojiAliasEntry(u32 name_offset, u16 glyph_offset, u8 name_bytes, u8 glyph_bytes) noexcept
        : name_offset_low(static_cast<u16>(name_offset)), glyph_offset(glyph_offset),
          name_packed(static_cast<u8>(name_bytes | ((name_offset >> 16u) << 7u))), glyph_bytes(glyph_bytes) {
        if (name_offset >= (1u << 17u) || name_bytes >= (1u << 7u))
            emoji_alias_entry_out_of_range();
    }
    constexpr u32 name_offset() const noexcept {
        return name_offset_low | (static_cast<u32>(name_packed >> 7u) << 16u);
    }
    constexpr u32 name_bytes() const noexcept {
        return name_packed & 0x7fu;
    }
    Utf8View name() const noexcept;
    Utf8View glyph() const noexcept;
};
static_assert(sizeof(EmojiAliasEntry) == 6);
// Shortcode aliases (joypixels, gemoji, iamcal and emojibase names, sorted)
// that are not catalog names; see tools/generate_emoji_shortcodes.py.
ConstSlice<EmojiAliasEntry> emoji_shortcodes() noexcept;
// Exact, case-sensitive lookup: an alias, else a catalog (CLDR) name. A
// "::skin-tone-N" or "_toneN" suffix (N = 1..5) selects the base emoji's
// uniform Unicode skin-tone variant. Empty when unknown.
Utf8View emoji_shortcode_glyph(Utf8View name) noexcept;
// Writes the key for a favorite/frecency entry this client adds: the primary
// name (first joypixels, else gemoji), the base's key plus _toneN for a
// uniform skin-tone variant, else the catalog name. Returns the byte count,
// or 0 when the glyph is not in the catalog or the key does not fit.
u32 emoji_shortcode_key(Utf8View glyph, Slice<char> output) noexcept;
// Catalog index of a glyph, ignoring U+FE0F; ~0u when absent.
u32 emoji_catalog_find_glyph(Utf8View glyph) noexcept;
// Catalog index of an exact CLDR name; ~0u when absent.
u32 emoji_catalog_find_name(Utf8View name) noexcept;

} // namespace maicord
