#include "maicord/ui/text/emoji_catalog.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/media/compression/inflated_block.hpp"

namespace maicord {

namespace emoji_pool_detail {
const char* catalog_names;
const char* catalog_glyphs;
} // namespace emoji_pool_detail

namespace {

// The generated pool: names, glyphs, then the catalog and alias rows (as plane blocks until published) and
// the primary-shortcode bit set. tools/pack_emoji_pools.py checks every row against the sources.
#include <emoji_pool_pack.inc>

static_assert(kEmojiPoolCatalogNamesOffset == 0);
static_assert(kEmojiPoolCatalogNamePrefixBytes == kUnicodeEmojiCatalogNameBytes);
// Aliases share the catalog's name and glyph pools, so one base serves both.
static_assert(kEmojiPoolShortcodeNamesOffset == kEmojiPoolCatalogNamesOffset);
static_assert(kEmojiPoolShortcodeGlyphsOffset == kEmojiPoolCatalogGlyphsOffset);
static_assert(kEmojiPoolCatalogGlyphsOffset >= kEmojiPoolCatalogNamePrefixBytes);
static_assert(kEmojiPoolCatalogGlyphsOffset < (1u << 17));
static_assert(kEmojiPoolCatalogGlyphBytes == kUnicodeEmojiCatalogGlyphBytes);
static_assert(kEmojiPoolShortcodeGlyphBytes <= kEmojiPoolCatalogGlyphBytes);
static_assert(kEmojiPoolCatalogEntryCount == kUnicodeEmojiCatalogEntryCount);
static_assert(sizeof(kEmojiPoolCategoryRanges) / sizeof(kEmojiPoolCategoryRanges[0]) == kEmojiCategoryCount);
static_assert(kEmojiPoolMaximumNameBytes == kUnicodeEmojiMaximumNameBytes);
static_assert(kEmojiPoolMaximumGlyphBytes == kUnicodeEmojiMaximumGlyphBytes);
static_assert(kEmojiPoolCatalogEntryOffset >= kEmojiPoolCatalogGlyphsOffset + kEmojiPoolCatalogGlyphBytes);
static_assert(kEmojiPoolCatalogEntryOffset % alignof(u16) == 0);
static_assert(kEmojiPoolAliasOffset == kEmojiPoolCatalogEntryOffset + kEmojiPoolCatalogEntryCount * sizeof(EmojiCatalogEntry));
static_assert(kEmojiPoolPrimaryOffset == kEmojiPoolAliasOffset + kEmojiPoolAliasCount * sizeof(EmojiAliasEntry));
static_assert(kEmojiPoolBytes == kEmojiPoolPrimaryOffset + (kEmojiPoolAliasCount + 7) / 8);

InflatedBlock pool_block;

// A table's rows ship as byte planes of 256-row blocks; this interleaves each block back to 6-byte rows in place.
void interleave_rows(u8* rows, u32 count) noexcept {
    u8 block[256 * 6];
    for (u32 first = 0; first < count; first += 256) {
        const u32 n = minimum(count - first, 256u);
        u8* at = rows + first * 6;
        copy_bytes(block, at, n * 6);
        for (u32 column = 0; column < 6; ++column)
            for (u32 i = 0; i < n; ++i)
                at[i * 6 + column] = block[column * n + i];
    }
}

void publish_pool(const u8* bytes) noexcept {
    auto* pool = const_cast<u8*>(bytes);
    emoji_pool_detail::catalog_names = reinterpret_cast<const char*>(pool + kEmojiPoolCatalogNamesOffset);
    emoji_pool_detail::catalog_glyphs = reinterpret_cast<const char*>(pool + kEmojiPoolCatalogGlyphsOffset);
    interleave_rows(pool + kEmojiPoolCatalogEntryOffset, kEmojiPoolCatalogEntryCount);
    interleave_rows(pool + kEmojiPoolAliasOffset, kEmojiPoolAliasCount);
}

bool pool_ready() noexcept {
    return pool_block.state == 2 ||
           inflated_block_load(pool_block, kEmojiPoolPacked, sizeof(kEmojiPoolPacked), kEmojiPoolBytes,
                               publish_pool);
}

// The table views below are valid once pool_ready() has returned true.
const EmojiCatalogEntry* catalog_rows() noexcept {
    return reinterpret_cast<const EmojiCatalogEntry*>(pool_block.data + kEmojiPoolCatalogEntryOffset);
}
const EmojiAliasEntry* alias_rows() noexcept {
    return reinterpret_cast<const EmojiAliasEntry*>(pool_block.data + kEmojiPoolAliasOffset);
}

// Callers validate both pointers and the complete query range.
bool range_equal_folded_ascii(Utf8View candidate, u32 first, Utf8View query) noexcept {
    for (u32 index = 0; index < query.bytes; ++index) {
        const u8 byte = static_cast<u8>(candidate.data[first + index]);
        const u8 difference = byte ^ static_cast<u8>(query.data[index]);
        // Only ASCII letters have an unequal byte that folds to the same value.
        if (difference && (difference != 0x20u ||
            static_cast<u32>(byte | 0x20u) - static_cast<u32>('a') > 25u)) {
            return false;
        }
    }
    return true;
}

} // namespace

Utf8View EmojiAliasEntry::name() const noexcept {
    return {emoji_pool_detail::catalog_names + name_offset(), name_bytes()};
}
Utf8View EmojiAliasEntry::glyph() const noexcept {
    return {emoji_pool_detail::catalog_glyphs + glyph_offset, glyph_bytes};
}
ConstSlice<EmojiAliasEntry> emoji_shortcodes() noexcept {
    if (!pool_ready())
        return {};
    return {alias_rows(), kEmojiPoolAliasCount};
}

ConstSlice<EmojiCatalogEntry> unicode_emoji_catalog() noexcept {
    if (!pool_ready())
        return {};
    return ConstSlice<EmojiCatalogEntry>{catalog_rows(), kEmojiPoolCatalogEntryCount};
}

EmojiCatalogRange emoji_catalog_category_range(EmojiCategory category) noexcept {
    const u32 index = static_cast<u32>(category);
    return index < kEmojiCategoryCount
               ? EmojiCatalogRange{kEmojiPoolCategoryRanges[index][0], kEmojiPoolCategoryRanges[index][1]}
               : EmojiCatalogRange{};
}

namespace {
bool selector_at(Utf8View v, u32 at) noexcept {
    return at + 3 <= v.bytes && static_cast<u8>(v.data[at]) == 0xEF && static_cast<u8>(v.data[at + 1]) == 0xB8 &&
           static_cast<u8>(v.data[at + 2]) == 0x8F;
}
// Skin-tone modifiers U+1F3FB..U+1F3FF are F0 9F 8F BB..BF; returns the last
// byte, or 0. Lead bytes never occur mid-sequence, so any offset is safe.
u8 skin_tone_at(Utf8View v, u32 at) noexcept {
    const bool modifier = at + 4 <= v.bytes && static_cast<u8>(v.data[at]) == 0xF0 &&
                          static_cast<u8>(v.data[at + 1]) == 0x9F && static_cast<u8>(v.data[at + 2]) == 0x8F &&
                          static_cast<u8>(v.data[at + 3]) - 0xBBu <= 4u;
    return modifier ? static_cast<u8>(v.data[at + 3]) : 0;
}
u8 first_skin_tone(Utf8View v) noexcept {
    for (u32 i = 0; i < v.bytes; ++i)
        if (const u8 tone = skin_tone_at(v, i))
            return tone;
    return 0;
}

// Identity ignores U+FE0F: the catalog is fully qualified but message text and
// shortcode sources may omit presentation selectors. With a tone (a modifier's
// last byte), `a` must be `b` plus at least one modifier, all of that tone.
bool glyph_equal(Utf8View a, Utf8View b, u8 tone = 0) noexcept {
    u32 i = 0, j = 0, tones = 0;
    for (;;) {
        for (;;) {
            if (selector_at(a, i)) {
                i += 3;
            } else if (const u8 found = tone ? skin_tone_at(a, i) : 0) {
                if (found != tone)
                    return false;
                i += 4;
                ++tones;
            } else {
                break;
            }
        }
        while (selector_at(b, j))
            j += 3;
        if (i == a.bytes || j == b.bytes)
            return i == a.bytes && j == b.bytes && (!tone || tones);
        if (a.data[i++] != b.data[j++])
            return false;
    }
}

Utf8View alias_glyph(Utf8View name) noexcept {
    const auto aliases = emoji_shortcodes();
    u32 first = 0, last = aliases.count;
    while (first < last) {
        const u32 middle = first + (last - first) / 2;
        const auto candidate = aliases[middle].name();
        i32 comparison = 0;
        for (u32 i = 0; i < minimum(name.bytes, candidate.bytes); ++i) {
            comparison = static_cast<u8>(name.data[i]) - static_cast<u8>(candidate.data[i]);
            if (comparison) break;
        }
        if (!comparison) comparison = name.bytes < candidate.bytes ? -1 : name.bytes > candidate.bytes ? 1 : 0;
        if (!comparison) return aliases[middle].glyph();
        if (comparison < 0) last = middle;
        else first = middle + 1;
    }
    return {};
}

// Aliases first (gemoji names such as "cat" keep their usual emoji), then the
// catalog's CLDR names, which the alias table does not repeat.
Utf8View plain_shortcode_glyph(Utf8View name) noexcept {
    if (const auto glyph = alias_glyph(name); glyph.bytes)
        return glyph;
    const u32 index = emoji_catalog_find_name(name);
    return index == ~0u ? Utf8View{} : catalog_rows()[index].glyph();
}

// emoji-test.txt lists every fully-qualified skin-tone variant directly after
// its untoned base, so the uniform variant is found before the next base.
Utf8View skin_tone_variant(Utf8View base, u8 tone) noexcept {
    const u32 index = emoji_catalog_find_glyph(base);
    if (index == ~0u || first_skin_tone(base))
        return {};
    const EmojiCatalogEntry* entries = catalog_rows();
    for (u32 i = index + 1; i < kEmojiPoolCatalogEntryCount; ++i) {
        const auto candidate = entries[i].glyph();
        if (!first_skin_tone(candidate))
            break;
        if (glyph_equal(candidate, base, tone))
            return candidate;
    }
    return {};
}

// Primary rows are emoji whose shortcode key (favorites, frecency) is neither
// the catalog name nor the base's key with _toneN.
Utf8View primary_row(u32 index) noexcept {
    const auto& glyph = catalog_rows()[index];
    const EmojiAliasEntry* aliases = alias_rows();
    const u8* primary = pool_block.data + kEmojiPoolPrimaryOffset;
    // The packer guarantees at most one primary row per glyph.
    for (u32 row = 0; row < kEmojiPoolAliasCount; ++row) {
        if ((primary[row >> 3] >> (row & 7) & 1) && aliases[row].glyph_offset == glyph.glyph_offset &&
            aliases[row].glyph_bytes == glyph.glyph_bytes)
            return aliases[row].name();
    }
    return {};
}
} // namespace

Utf8View emoji_shortcode_glyph(Utf8View name) noexcept {
    if (!name.data || !name.bytes || !pool_ready())
        return {};
    if (const auto glyph = plain_shortcode_glyph(name); glyph.bytes)
        return glyph;
    const u8 digit = static_cast<u8>(name.data[name.bytes - 1]);
    if (digit < '1' || digit > '5')
        return {};
    u32 suffix = 0;
    if (name.bytes > 13 && range_equal_folded_ascii(name, name.bytes - 13, utf8_literal("::skin-tone-")))
        suffix = 13;
    else if (name.bytes > 6 && range_equal_folded_ascii(name, name.bytes - 6, utf8_literal("_tone")))
        suffix = 6;
    if (!suffix)
        return {};
    const auto base = plain_shortcode_glyph({name.data, name.bytes - suffix});
    return base.bytes ? skin_tone_variant(base, static_cast<u8>(0xBA + digit - '0')) : Utf8View{};
}

u32 emoji_catalog_find_name(Utf8View name) noexcept {
    if (!name.data || !name.bytes || name.bytes > kUnicodeEmojiMaximumNameBytes || !pool_ready())
        return ~0u;
    // Length and first byte reject nearly every entry before any pool access.
    const char* pool = emoji_pool_detail::catalog_names;
    const EmojiCatalogEntry* entries = catalog_rows();
    for (u32 i = 0; i < kEmojiPoolCatalogEntryCount; ++i) {
        const EmojiCatalogEntry& entry = entries[i];
        if (entry.name_length == name.bytes && pool[entry.name_offset()] == name.data[0] &&
            bytes_equal(pool + entry.name_offset(), name.data, name.bytes))
            return i;
    }
    return ~0u;
}

u32 emoji_catalog_find_glyph(Utf8View glyph) noexcept {
    if (!glyph.data || !glyph.bytes || glyph.bytes > kUnicodeEmojiMaximumGlyphBytes + 12 || !pool_ready())
        return ~0u;
    // The first scalar must match (a leading U+FE0F, EF B8 8F, is skipped by
    // glyph_equal, so it gets no prefilter).
    const u8 lead = static_cast<u8>(glyph.data[0]);
    u32 prefix = lead < 0x80u ? 1u : lead >= 0xC0u && lead < 0xE0u ? 2u : lead >= 0xE0u && lead < 0xF0u ? 3u
                                    : lead >= 0xF0u && lead < 0xF8u ? 4u : 0u;
    if (lead == 0xEFu || prefix > glyph.bytes)
        prefix = 0;
    const EmojiCatalogEntry* entries = catalog_rows();
    for (u32 i = 0; i < kEmojiPoolCatalogEntryCount; ++i) {
        const Utf8View candidate = entries[i].glyph();
        if (prefix && (candidate.bytes < prefix || !bytes_equal(candidate.data, glyph.data, prefix)))
            continue;
        if (glyph_equal(candidate, glyph))
            return i;
    }
    return ~0u;
}

u32 emoji_shortcode_key(Utf8View glyph, Slice<char> output) noexcept {
    const u32 index = output.data ? emoji_catalog_find_glyph(glyph) : ~0u;
    if (index == ~0u)
        return 0;
    const EmojiCatalogEntry* entries = catalog_rows();
    const auto entry = entries[index].glyph();
    const u8 tone = first_skin_tone(entry);
    Utf8View name = primary_row(index);
    u8 suffix = 0;
    if (!name.bytes && tone) {
        // The nearest preceding untoned entry is the base; other mixed tones
        // keep their catalog name.
        u32 base = index;
        while (base && first_skin_tone(entries[base].glyph()))
            --base;
        if (glyph_equal(entry, entries[base].glyph(), tone)) {
            name = primary_row(base);
            if (!name.bytes)
                name = entries[base].name();
            suffix = static_cast<u8>('0' + tone - 0xBA);
        }
    }
    if (!name.bytes)
        name = entries[index].name();
    const u32 bytes = name.bytes + (suffix ? 6 : 0);
    if (bytes > output.count)
        return 0;
    copy_bytes(output.data, name.data, name.bytes);
    if (suffix) {
        copy_bytes(output.data + name.bytes, "_tone", 5);
        output.data[name.bytes + 5] = static_cast<char>(suffix);
    }
    return bytes;
}

bool emoji_catalog_name_starts_with(Utf8View name, Utf8View query) noexcept {
    return query.bytes == 0 || (name.data && query.data && query.bytes <= name.bytes &&
                               range_equal_folded_ascii(name, 0, query));
}

bool emoji_catalog_name_contains(Utf8View name, Utf8View query) noexcept {
    if (query.bytes == 0) {
        return true;
    }
    if (!name.data || !query.data || query.bytes > name.bytes) {
        return false;
    }
    for (u32 first = 0; first <= name.bytes - query.bytes; ++first) {
        if (range_equal_folded_ascii(name, first, query)) {
            return true;
        }
    }
    return false;
}

} // namespace maicord
