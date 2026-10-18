#include "maicord/core/utf.hpp"

#include "maicord/sys/intrin.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord {
namespace {

__forceinline Utf8DecodeResult decode_utf8_scalar(const char* input, u32 available) noexcept {
    if (!input || available == 0) {
        return Utf8DecodeResult{0, 0, false};
    }

    const u8 first = static_cast<u8>(input[0]);
    if (first <= 0x7Fu) {
        return Utf8DecodeResult{first, 1, true};
    }

    u32 codepoint = 0;
    u32 minimum_codepoint = 0;
    u8 width = 0;
    if (first >= 0xC2u && first <= 0xDFu) {
        codepoint = first & 0x1Fu;
        minimum_codepoint = 0x80u;
        width = 2;
    } else if (first >= 0xE0u && first <= 0xEFu) {
        codepoint = first & 0x0Fu;
        minimum_codepoint = 0x800u;
        width = 3;
    } else if (first >= 0xF0u && first <= 0xF4u) {
        codepoint = first & 0x07u;
        minimum_codepoint = 0x10000u;
        width = 4;
    } else {
        return Utf8DecodeResult{0, 1, false};
    }

    if (available < width) {
        return Utf8DecodeResult{0, 1, false};
    }
    for (u8 index = 1; index < width; ++index) {
        const u8 continuation = static_cast<u8>(input[index]);
        if ((continuation & 0xC0u) != 0x80u) {
            return Utf8DecodeResult{0, 1, false};
        }
        codepoint = (codepoint << 6) | (continuation & 0x3Fu);
    }

    if (codepoint < minimum_codepoint || codepoint > 0x10FFFFu ||
        (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
        return Utf8DecodeResult{0, 1, false};
    }
    return Utf8DecodeResult{codepoint, width, true};
}

} // namespace

Utf8DecodeResult utf8_decode_one(const char* input, u32 available) noexcept {
    return decode_utf8_scalar(input, available);
}

Utf8Validation utf8_validate(Utf8View input) noexcept {
    if (!input.data && input.bytes != 0) {
        return Utf8Validation{0, 0, false};
    }

    u32 offset = 0;
    u32 codepoints = 0;
    while (offset < input.bytes) {
        if (static_cast<u8>(input.data[offset]) <= 0x7Fu) {
            if (input.bytes - offset >= 16) {
                // Inspect only caller-owned bytes. The mask gives the first
                // non-ASCII byte, whose scalar still uses the ordinary decoder.
                const __m128i block = _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(input.data + offset));
                const u32 mask = static_cast<u32>(_mm_movemask_epi8(block));
                unsigned long ascii_bytes = 16;
                if (mask != 0) {
                    _BitScanForward(&ascii_bytes, mask);
                }
                offset += static_cast<u32>(ascii_bytes);
                codepoints += static_cast<u32>(ascii_bytes);
            } else {
                ++offset;
                ++codepoints;
            }
            continue;
        }
        const Utf8DecodeResult decoded = decode_utf8_scalar(input.data + offset, input.bytes - offset);
        if (!decoded.valid) {
            return Utf8Validation{codepoints, offset, false};
        }
        offset += decoded.width;
        ++codepoints;
    }
    return Utf8Validation{codepoints, input.bytes, true};
}

namespace {
// Word-selection classes. Combining marks, joiners, variation selectors,
// emoji modifiers and tags extend whatever precedes them.
enum class WordClass : u8 { space, line, word, punctuation, han, hiragana, katakana, emoji, extend };

constexpr bool in(u32 value, u32 first, u32 last) noexcept { return value >= first && value <= last; }

WordClass word_class(u32 cp) noexcept {
    if (cp < 0x80) {
        if (cp == '\n' || cp == '\r' || cp == 0x0B || cp == 0x0C) return WordClass::line;
        if (cp == ' ' || cp == '\t') return WordClass::space;
        if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') || cp == '_')
            return WordClass::word;
        return cp < 0x20 || cp == 0x7F ? WordClass::space : WordClass::punctuation;
    }
    if (cp == 0x85 || cp == 0x2028 || cp == 0x2029) return WordClass::line;
    if (cp == 0xA0 || cp == 0x1680 || in(cp, 0x2000, 0x200B) || cp == 0x202F || cp == 0x205F || cp == 0x3000 ||
        cp == 0xFEFF)
        return WordClass::space;
    if (in(cp, 0x0300, 0x036F) || in(cp, 0x1AB0, 0x1AFF) || in(cp, 0x1DC0, 0x1DFF) || in(cp, 0x20D0, 0x20FF) ||
        cp == 0x200C || cp == 0x200D || in(cp, 0xFE00, 0xFE0F) || in(cp, 0xFE20, 0xFE2F) || in(cp, 0x3099, 0x309A) ||
        in(cp, 0x1F3FB, 0x1F3FF) || in(cp, 0xE0020, 0xE007F) || in(cp, 0xE0100, 0xE01EF))
        return WordClass::extend;
    if (in(cp, 0x3040, 0x309F)) return WordClass::hiragana;
    if (in(cp, 0x30A0, 0x30FF) || in(cp, 0x31F0, 0x31FF) || in(cp, 0xFF66, 0xFF9F)) return WordClass::katakana;
    if (in(cp, 0x3400, 0x4DBF) || in(cp, 0x4E00, 0x9FFF) || in(cp, 0xF900, 0xFAFF) || in(cp, 0x20000, 0x3FFFF) ||
        cp == 0x3005 || cp == 0x3007 || in(cp, 0x3021, 0x3029))
        return WordClass::han;
    if (in(cp, 0x1F000, 0x1FAFF) || in(cp, 0x2600, 0x27BF) || in(cp, 0x2B00, 0x2BFF) || cp == 0x00A9 ||
        cp == 0x00AE || cp == 0x203C || cp == 0x2049 || cp == 0x2122 || cp == 0x2139 || in(cp, 0x2194, 0x21AA) ||
        in(cp, 0x231A, 0x23FF) || in(cp, 0x25AA, 0x25FE) || cp == 0x3030 || cp == 0x303D || cp == 0x3297 ||
        cp == 0x3299)
        return WordClass::emoji;
    if (in(cp, 0x00A1, 0x00BF) && cp != 0x00AA && cp != 0x00B5 && cp != 0x00BA) return WordClass::punctuation;
    if (cp == 0x00D7 || cp == 0x00F7 || in(cp, 0x2010, 0x2027) || in(cp, 0x2030, 0x205E) ||
        in(cp, 0x2070, 0x209F) || in(cp, 0x20A0, 0x20CF) || in(cp, 0x2100, 0x214F) || in(cp, 0x2190, 0x2BFF) ||
        in(cp, 0x2E00, 0x2E7F) || in(cp, 0x3001, 0x303F) || in(cp, 0xFE10, 0xFE1F) || in(cp, 0xFE30, 0xFE6F) ||
        in(cp, 0xFF01, 0xFF0F) || in(cp, 0xFF1A, 0xFF20) || in(cp, 0xFF3B, 0xFF40) || in(cp, 0xFF5B, 0xFF65) ||
        in(cp, 0xFFE0, 0xFFEE))
        return WordClass::punctuation;
    return WordClass::word;
}

struct WordText {
    Utf8View text;
    u32 previous(u32 offset) const noexcept {
        if (offset) --offset;
        while (offset && (static_cast<u8>(text.data[offset]) & 0xC0) == 0x80) --offset;
        return offset;
    }
    u32 codepoint(u32 offset) const noexcept {
        return utf8_decode_one(text.data + offset, text.bytes - offset).codepoint;
    }
    u32 next(u32 offset) const noexcept {
        return offset + utf8_decode_one(text.data + offset, text.bytes - offset).width;
    }
    WordClass raw(u32 offset) const noexcept { return word_class(codepoint(offset)); }
    // Extenders take their base's class; a leading extender acts as a letter.
    WordClass base(u32 offset) const noexcept {
        auto value = raw(offset);
        while (value == WordClass::extend && offset) {
            offset = previous(offset);
            value = raw(offset);
        }
        return value == WordClass::extend ? WordClass::word : value;
    }
};

constexpr bool regional_indicator(u32 cp) noexcept { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }

// Within an emoji run select one cluster: base + extenders, ZWJ sequences,
// regional-indicator pairs.
void emoji_cluster(const WordText& t, u32 position, u32* first, u32* last) noexcept {
    u32 start = *first;
    while (start < *last) {
        u32 end = t.next(start);
        if (regional_indicator(t.codepoint(start)) && end < *last && regional_indicator(t.codepoint(end)))
            end = t.next(end);
        while (end < *last) {
            const u32 cp = t.codepoint(end);
            if (word_class(cp) != WordClass::extend) break;
            end = t.next(end);
            if (cp == 0x200D && end < *last && t.raw(end) == WordClass::emoji) end = t.next(end);
        }
        if (position < end) {
            *first = start;
            *last = end;
            return;
        }
        start = end;
    }
}

// Ctrl+Arrow/Ctrl+Delete classes: ASCII whitespace (including line breaks),
// word characters (ASCII letters/digits/underscore and every non-ASCII scalar)
// or ASCII punctuation.
bool navigation_word_range(Utf8View text, u32 position, u32* first, u32* last) noexcept {
    const WordText t{text};
    const auto classify = [&t](u32 offset) noexcept {
        const auto cp = t.codepoint(offset);
        if (cp == ' ' || cp == 0x09 || cp == 0x0D || cp == 0x0A) return 0;
        return ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') || cp == '_' ||
                cp >= 0x80)
                   ? 1
                   : 2;
    };
    if (position == text.bytes) position = t.previous(position);
    const auto selected = classify(position);
    u32 begin = position, end = position;
    while (begin && classify(t.previous(begin)) == selected) begin = t.previous(begin);
    while (end < text.bytes && classify(end) == selected) end = t.next(end);
    *first = begin;
    *last = end;
    return true;
}

bool word_range(Utf8View text, u32 position, bool selection, u32* first, u32* last) noexcept {
    if (!first || !last || !utf8_is_boundary(text, position) || !utf8_validate(text).valid) return false;
    if (!text.bytes) { *first = *last = 0; return true; }
    if (!selection) return navigation_word_range(text, position, first, last);
    const WordText t{text};
    if (position == text.bytes) position = t.previous(position);
    auto selected = t.base(position);
    if (selected == WordClass::line) {
        // A pointer past a line's end selects that line's last run; an empty
        // line selects nothing. Selection never spans a line break.
        if (!position || t.base(t.previous(position)) == WordClass::line) {
            *first = *last = position;
            return true;
        }
        position = t.previous(position);
        selected = t.base(position);
    }
    u32 begin = position, end = position;
    while (begin && t.base(t.previous(begin)) == selected) begin = t.previous(begin);
    while (end < text.bytes) {
        const auto value = t.raw(end);
        if (value != WordClass::extend && value != selected) break;
        end = t.next(end);
    }
    *first = begin;
    *last = end;
    if (selected == WordClass::emoji) emoji_cluster(t, position, first, last);
    return true;
}
} // namespace

bool utf8_word_range(Utf8View text, u32 position, u32* first, u32* last) noexcept {
    return word_range(text, position, true, first, last);
}

bool utf8_word_range_near(Utf8View text, u32 position, u32* first, u32* last) noexcept {
    if (!utf8_is_boundary(text, position) || !utf8_validate(text).valid) return false;
    if (position && text.bytes) {
        const WordText t{text};
        const u32 prior = t.previous(position);
        const auto before = t.base(prior);
        const auto at = position < text.bytes ? t.base(position) : WordClass::line;
        // A rounded caret position on the right half of a word's last scalar
        // lands on the following space/line break: select the word instead.
        if ((at == WordClass::space || at == WordClass::line) && before != WordClass::space &&
            before != WordClass::line)
            position = prior;
    }
    return word_range(text, position, true, first, last);
}

bool utf8_line_range(Utf8View text, u32 position, u32* first, u32* last) noexcept {
    if (!first || !last || !utf8_is_boundary(text, position) || !utf8_validate(text).valid) return false;
    const WordText t{text};
    u32 begin = position, end = position;
    while (begin && t.raw(t.previous(begin)) != WordClass::line) begin = t.previous(begin);
    while (end < text.bytes && t.raw(end) != WordClass::line) end = t.next(end);
    *first = begin;
    *last = end;
    return true;
}

bool utf8_word_boundary(Utf8View text, u32 position, bool forward, u32* boundary) noexcept {
    u32 first = 0, last = 0;
    if (!boundary || !word_range(text, position, false, &first, &last)) return false;
    const auto space = [&text](u32 p) noexcept {
        const char c = text.data[p]; return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    if (forward) {
        if (position == text.bytes) { *boundary = position; return true; }
        u32 end = last;
        while (end < text.bytes && space(end)) ++end;
        *boundary = end;
    } else {
        u32 end = position;
        while (end && space(end - 1)) --end;
        if (!end) { *boundary = 0; return true; }
        u32 prior = end - 1;
        while (prior && (static_cast<u8>(text.data[prior]) & 0xC0) == 0x80) --prior;
        if (!word_range(text, prior, false, &first, &last)) return false;
        *boundary = first;
    }
    return true;
}

u8 utf8_encode(u32 codepoint, char output[4]) noexcept {
    if (!output || codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
        return 0;
    }
    if (codepoint <= 0x7Fu) {
        output[0] = static_cast<char>(codepoint);
        return 1;
    }
    if (codepoint <= 0x7FFu) {
        output[0] = static_cast<char>(0xC0u | (codepoint >> 6));
        output[1] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
        return 2;
    }
    if (codepoint <= 0xFFFFu) {
        output[0] = static_cast<char>(0xE0u | (codepoint >> 12));
        output[1] = static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
        output[2] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
        return 3;
    }

    output[0] = static_cast<char>(0xF0u | (codepoint >> 18));
    output[1] = static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu));
    output[2] = static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
    output[3] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
    return 4;
}

bool utf8_utf16_position_for_byte_offset(Utf8View text, u32 byte_offset,
                                         u32* utf16_position) noexcept {
    if (!utf16_position || byte_offset > text.bytes ||
        (!text.data && text.bytes != 0)) {
        return false;
    }
    u32 offset = 0;
    u32 utf16_units = 0;
    while (offset < byte_offset) {
        const Utf8DecodeResult decoded =
            utf8_decode_one(text.data + offset, text.bytes - offset);
        if (!decoded.valid || decoded.width > byte_offset - offset) {
            return false;
        }
        utf16_units += decoded.codepoint > 0xFFFFu ? 2u : 1u;
        offset += decoded.width;
    }
    *utf16_position = utf16_units;
    return true;
}

bool utf8_byte_offset_for_utf16_position(Utf8View text, u32 utf16_position,
                                         u32* byte_offset) noexcept {
    if (!byte_offset || (!text.data && text.bytes != 0)) {
        return false;
    }
    u32 offset = 0;
    u32 utf16_units = 0;
    while (offset < text.bytes && utf16_units < utf16_position) {
        const Utf8DecodeResult decoded =
            utf8_decode_one(text.data + offset, text.bytes - offset);
        if (!decoded.valid) {
            return false;
        }
        const u32 width = decoded.codepoint > 0xFFFFu ? 2u : 1u;
        if (utf16_units + width > utf16_position) {
            return false;
        }
        utf16_units += width;
        offset += decoded.width;
    }
    if (utf16_units != utf16_position) {
        return false;
    }
    *byte_offset = offset;
    return true;
}

bool utf8_utf16_position_for_codepoint(Utf8View text, u32 codepoint_index,
                                       u32* utf16_position) noexcept {
    if (!utf16_position || (!text.data && text.bytes != 0)) {
        return false;
    }
    u32 offset = 0;
    u32 codepoints = 0;
    u32 utf16_units = 0;
    while (offset < text.bytes && codepoints < codepoint_index) {
        const Utf8DecodeResult decoded =
            utf8_decode_one(text.data + offset, text.bytes - offset);
        if (!decoded.valid) {
            return false;
        }
        utf16_units += decoded.codepoint > 0xFFFFu ? 2u : 1u;
        offset += decoded.width;
        ++codepoints;
    }
    if (codepoints != codepoint_index) {
        return false;
    }
    *utf16_position = utf16_units;
    return true;
}

bool utf8_codepoint_for_utf16_position(Utf8View text, u32 utf16_position,
                                       u32* codepoint_index) noexcept {
    if (!codepoint_index || (!text.data && text.bytes != 0)) {
        return false;
    }
    u32 offset = 0;
    u32 codepoints = 0;
    u32 utf16_units = 0;
    while (offset < text.bytes && utf16_units < utf16_position) {
        const Utf8DecodeResult decoded =
            utf8_decode_one(text.data + offset, text.bytes - offset);
        if (!decoded.valid) {
            return false;
        }
        const u32 width = decoded.codepoint > 0xFFFFu ? 2u : 1u;
        if (utf16_units + width > utf16_position) {
            return false;
        }
        utf16_units += width;
        offset += decoded.width;
        ++codepoints;
    }
    if (utf16_units != utf16_position) {
        return false;
    }
    *codepoint_index = codepoints;
    return true;
}

u32 utf8_utf16_units(Utf8View text) noexcept {
    u32 units = 0;
    for (u32 i = 0; i < text.bytes; ++i) {
        const u8 byte = static_cast<u8>(text.data[i]);
        if ((byte & 0xC0) != 0x80)
            units += byte >= 0xF0 ? 2 : 1;
    }
    return units;
}

bool utf8_is_boundary(Utf8View text, u32 offset) noexcept {
    return offset <= text.bytes &&
           (offset == 0 || offset == text.bytes || (static_cast<u8>(text.data[offset]) & 0xC0u) != 0x80u);
}

bool utf8_copy_utf16_range(Utf8View text, u32 start, u32 end, Slice<u16> output, u32* written, u32* next) noexcept {
    if (!written || !next || (!output.data && output.count) || start > end) return false;
    u32 first = 0, last = 0;
    if (!utf8_byte_offset_for_utf16_position(text, start, &first) || !utf8_byte_offset_for_utf16_position(text, end, &last)) return false;
    u32 copied = 0, position = start;
    while (first < last) {
        const auto scalar = utf8_decode_one(text.data + first, last - first);
        if (!scalar.valid) return false;
        const u32 needed = scalar.codepoint > 0xFFFF ? 2 : 1;
        if (needed > output.count - copied) break;
        if (needed == 1) output[copied] = static_cast<u16>(scalar.codepoint);
        else {
            const u32 value = scalar.codepoint - 0x10000;
            output[copied] = static_cast<u16>(0xD800 + (value >> 10));
            output[copied + 1] = static_cast<u16>(0xDC00 + (value & 0x3FF));
        }
        copied += needed; position += needed; first += scalar.width;
    }
    *written = copied; *next = position; return true;
}

} // namespace maicord
