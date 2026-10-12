#include "maicord/ui/emoji_catalog.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/media/deflate.hpp"

#include <Windows.h>

namespace maicord {

// The generated pools keep internal linkage: only their lengths are used here
// (checked against the packed stream); the shipped bytes are the deflate stream.
#include <unicode_emoji_16_optimized.inc>

namespace emoji_pool_detail {
const char* catalog_names;
const char* catalog_glyphs;
} // namespace emoji_pool_detail

namespace {

#include "emoji_shortcodes.inc"
#include <emoji_pool_pack.inc>

static_assert(kEmojiPoolCatalogNamesOffset == 0);
static_assert(kEmojiPoolCatalogNamePrefixBytes == sizeof(kUnicodeEmojiCatalogNames) - 1);
static_assert(kEmojiPoolShortcodeNamesOffset < kEmojiPoolCatalogNamePrefixBytes);
static_assert(kEmojiPoolCatalogGlyphsOffset >= kEmojiPoolCatalogNamePrefixBytes);
static_assert(kEmojiPoolCatalogGlyphsOffset - kEmojiPoolShortcodeNamesOffset < (1u << 17));
static_assert(kEmojiPoolShortcodeGlyphsOffset == kEmojiPoolCatalogGlyphsOffset);
static_assert(kEmojiPoolBytes - kEmojiPoolCatalogGlyphsOffset == sizeof(kUnicodeEmojiCatalogGlyphs) - 1);
static_assert(sizeof(kEmojiShortcodeGlyphs) <= sizeof(kUnicodeEmojiCatalogGlyphs));
static_assert(sizeof(kEmojiSharedNameShortcodes) == sizeof(kEmojiShortcodes));

constexpr u32 shortcode_count = sizeof(kEmojiSharedNameShortcodes) / sizeof(kEmojiSharedNameShortcodes[0]);
consteval bool shortcode_layout_valid(u32 first, u32 last) noexcept {
    for (u32 i = first; i < last; ++i) {
        const auto& entry = kEmojiSharedNameShortcodes[i];
        const auto& original = kEmojiShortcodes[i];
        if (entry.name_offset() + entry.name_bytes() > kEmojiPoolCatalogGlyphsOffset - kEmojiPoolShortcodeNamesOffset ||
            entry.name_bytes() != original.name_bytes() || entry.glyph_offset != original.glyph_offset ||
            entry.glyph_bytes != original.glyph_bytes)
            return false;
    }
    return true;
}
// Separate ranges keep each constant evaluation within the compiler step limit.
static_assert(shortcode_layout_valid(0, shortcode_count / 4));
static_assert(shortcode_layout_valid(shortcode_count / 4, shortcode_count / 2));
static_assert(shortcode_layout_valid(shortcode_count / 2, shortcode_count * 3 / 4));
static_assert(shortcode_layout_valid(shortcode_count * 3 / 4, shortcode_count));

const char* shortcode_names;
const char* shortcode_glyphs;
// 0 packed, 1 inflating, 2 ready, 3 unavailable. /volatile:ms orders the
// pointer stores before the published state.
volatile LONG pool_state;

bool pool_ready() noexcept {
    for (;;) {
        const LONG state = pool_state;
        if (state == 2)
            return true;
        if (state == 3)
            return false;
        if (state == 0 && InterlockedCompareExchange(&pool_state, 1, 0) == 0) {
            auto* pool = static_cast<char*>(
                VirtualAlloc(nullptr, kEmojiPoolBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            DeflateWorkspace workspace;
            const DeflateInputSpan input{kEmojiPoolPacked, sizeof(kEmojiPoolPacked)};
            const bool ok = pool && zlib_inflate({&input, 1}, {reinterpret_cast<u8*>(pool), kEmojiPoolBytes},
                                                 kEmojiPoolBytes, &workspace).ok();
            if (ok) {
                emoji_pool_detail::catalog_names = pool + kEmojiPoolCatalogNamesOffset;
                emoji_pool_detail::catalog_glyphs = pool + kEmojiPoolCatalogGlyphsOffset;
                shortcode_names = pool + kEmojiPoolShortcodeNamesOffset;
                shortcode_glyphs = pool + kEmojiPoolShortcodeGlyphsOffset;
            } else if (pool) {
                VirtualFree(pool, 0, MEM_RELEASE);
            }
            pool_state = ok ? 2 : 3;
            return ok;
        }
        YieldProcessor();
    }
}

bool range_equal_folded_ascii(Utf8View candidate, u32 first, Utf8View query) noexcept {
    if (!candidate.data || !query.data || first > candidate.bytes ||
        query.bytes > candidate.bytes - first) {
        return false;
    }
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

consteval u32 maximum_name_bytes() noexcept {
    u32 result = 0;
    for (const EmojiCatalogEntry& entry : kUnicodeEmoji16Entries) {
        result = maximum(result, static_cast<u32>(entry.name_length));
    }
    return result;
}

consteval u32 maximum_glyph_bytes() noexcept {
    u32 result = 0;
    for (const EmojiCatalogEntry& entry : kUnicodeEmoji16Entries) {
        result = maximum(result, static_cast<u32>(entry.glyph_bytes));
    }
    return result;
}

inline constexpr u32 kGeneratedEntryCount =
    static_cast<u32>(sizeof(kUnicodeEmoji16Entries) / sizeof(kUnicodeEmoji16Entries[0]));
inline constexpr u32 kGeneratedCategoryBoundaryCount =
    static_cast<u32>(sizeof(kUnicodeEmoji16CategoryRanges) /
                     sizeof(kUnicodeEmoji16CategoryRanges[0]));

consteval bool catalog_layout_valid() noexcept {
    for (const EmojiCatalogEntry& entry : kUnicodeEmoji16Entries) {
        if (entry.name_offset()+entry.name_length > kUnicodeEmojiCatalogNameBytes ||
            entry.glyph_offset+entry.glyph_bytes > kUnicodeEmojiCatalogGlyphBytes) {
            return false;
        }
    }
    return true;
}

static_assert(kGeneratedEntryCount == kUnicodeEmojiCatalogEntryCount);
static_assert(kGeneratedCategoryBoundaryCount == kEmojiCategoryCount);
static_assert(sizeof(kUnicodeEmojiCatalogNames) - 1 == kUnicodeEmojiCatalogNameBytes);
static_assert(sizeof(kUnicodeEmojiCatalogGlyphs) - 1 == kUnicodeEmojiCatalogGlyphBytes);
static_assert(maximum_name_bytes() == kUnicodeEmojiMaximumNameBytes);
static_assert(maximum_glyph_bytes() == kUnicodeEmojiMaximumGlyphBytes);
static_assert(catalog_layout_valid());

} // namespace

Utf8View EmojiAliasEntry::name() const noexcept {
    return {shortcode_names + name_offset(), name_bytes()};
}
Utf8View EmojiAliasEntry::glyph() const noexcept {
    return {shortcode_glyphs + glyph_offset, glyph_bytes};
}
ConstSlice<EmojiAliasEntry> emoji_shortcodes() noexcept {
    if (!pool_ready())
        return {};
    return {kEmojiSharedNameShortcodes, shortcode_count};
}

ConstSlice<EmojiCatalogEntry> unicode_emoji_catalog() noexcept {
    if (!pool_ready())
        return {};
    return ConstSlice<EmojiCatalogEntry>{kUnicodeEmoji16Entries, kGeneratedEntryCount};
}

EmojiCatalogRange emoji_catalog_category_range(EmojiCategory category) noexcept {
    const u32 index = static_cast<u32>(category);
    return index < kEmojiCategoryCount
               ? EmojiCatalogRange{kUnicodeEmoji16CategoryRanges[index][0],
                                   kUnicodeEmoji16CategoryRanges[index][1]}
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
    for (u32 i = 0; i < kGeneratedEntryCount; ++i)
        if (utf8_equal(kUnicodeEmoji16Entries[i].name(), name))
            return kUnicodeEmoji16Entries[i].glyph();
    return {};
}

// emoji-test.txt lists every fully-qualified skin-tone variant directly after
// its untoned base, so the uniform variant is found before the next base.
Utf8View skin_tone_variant(Utf8View base, u8 tone) noexcept {
    const u32 index = emoji_catalog_find_glyph(base);
    if (index == ~0u || first_skin_tone(base))
        return {};
    for (u32 i = index + 1; i < kGeneratedEntryCount; ++i) {
        const auto candidate = kUnicodeEmoji16Entries[i].glyph();
        if (!first_skin_tone(candidate))
            break;
        if (glyph_equal(candidate, base, tone))
            return candidate;
    }
    return {};
}

// Primary rows list emoji whose new settings key is neither the catalog name
// nor the base's key with _toneN.
Utf8View primary_row(u32 index) noexcept {
    const auto glyph = kUnicodeEmoji16Entries[index].glyph();
    for (const u16 row : kEmojiPrimaryShortcodes)
        if (glyph_equal(kEmojiSharedNameShortcodes[row].glyph(), glyph))
            return kEmojiSharedNameShortcodes[row].name();
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

u32 emoji_catalog_find_glyph(Utf8View glyph) noexcept {
    if (!glyph.data || !glyph.bytes || glyph.bytes > kUnicodeEmojiMaximumGlyphBytes + 12 || !pool_ready())
        return ~0u;
    for (u32 i = 0; i < kGeneratedEntryCount; ++i)
        if (glyph_equal(kUnicodeEmoji16Entries[i].glyph(), glyph))
            return i;
    return ~0u;
}

u32 emoji_shortcode_key(Utf8View glyph, Slice<char> output) noexcept {
    const u32 index = output.data ? emoji_catalog_find_glyph(glyph) : ~0u;
    if (index == ~0u)
        return 0;
    const auto entry = kUnicodeEmoji16Entries[index].glyph();
    const u8 tone = first_skin_tone(entry);
    Utf8View name = primary_row(index);
    u8 suffix = 0;
    if (!name.bytes && tone) {
        // The nearest preceding untoned entry is the base; other mixed tones
        // keep their catalog name.
        u32 base = index;
        while (base && first_skin_tone(kUnicodeEmoji16Entries[base].glyph()))
            --base;
        if (glyph_equal(entry, kUnicodeEmoji16Entries[base].glyph(), tone)) {
            name = primary_row(base);
            if (!name.bytes)
                name = kUnicodeEmoji16Entries[base].name();
            suffix = static_cast<u8>('0' + tone - 0xBA);
        }
    }
    if (!name.bytes)
        name = kUnicodeEmoji16Entries[index].name();
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
    return query.bytes == 0 || range_equal_folded_ascii(name, 0, query);
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
