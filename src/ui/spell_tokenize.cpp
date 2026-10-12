#include "maicord/ui/spell_tokenize.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/text/unicode.hpp"

namespace maicord {
namespace {
struct Scalar {
    u32 cp;
    u32 width;
};
struct Text {
    const char* data;
    u32 bytes;

    u8 byte(u32 at) const noexcept {
        return at < bytes ? static_cast<u8>(data[at]) : 0;
    }
    Scalar at(u32 offset) const noexcept {
        if (offset >= bytes)
            return {0, 0};
        const u8 lead = static_cast<u8>(data[offset]);
        if (lead < 0x80)
            return {lead, 1};
        const auto decoded = utf8_decode_one(data + offset, bytes - offset);
        return decoded.valid ? Scalar{decoded.codepoint, decoded.width} : Scalar{0xFFFD, 1};
    }
};

UnicodeGeneralCategory category(u32 cp) noexcept {
    const auto* properties = unicode_properties(cp);
    return properties ? properties->general_category() : UnicodeGeneralCategory::cn;
}
// Scripts written without spaces between words are left to the IME/user.
bool unspaced_script(u32 cp) noexcept {
    return (cp >= 0x0E00 && cp <= 0x0EFF) || (cp >= 0x1000 && cp <= 0x109F) || (cp >= 0x1780 && cp <= 0x17FF) ||
           (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF66 && cp <= 0xFF9F) ||
           cp >= 0x20000;
}
bool letter(u32 cp) noexcept {
    if (cp < 0x80)
        return (cp | 0x20) >= 'a' && (cp | 0x20) <= 'z';
    if (unspaced_script(cp))
        return false;
    switch (category(cp)) {
    case UnicodeGeneralCategory::lu:
    case UnicodeGeneralCategory::ll:
    case UnicodeGeneralCategory::lt:
    case UnicodeGeneralCategory::lm:
    case UnicodeGeneralCategory::lo:
        return true;
    default:
        return false;
    }
}
bool mark(u32 cp) noexcept {
    if (cp < 0x300)
        return false;
    if (cp == 0x200C || cp == 0x200D)
        return true;
    const auto value = category(cp);
    return value == UnicodeGeneralCategory::mn || value == UnicodeGeneralCategory::mc ||
           value == UnicodeGeneralCategory::me;
}
bool digit(u32 cp) noexcept {
    if (cp < 0x80)
        return cp >= '0' && cp <= '9';
    const auto value = category(cp);
    return value == UnicodeGeneralCategory::nd || value == UnicodeGeneralCategory::nl ||
           value == UnicodeGeneralCategory::no;
}
bool space(u32 cp) noexcept {
    if (cp < 0x80)
        return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v';
    const auto value = category(cp);
    return value == UnicodeGeneralCategory::zs || value == UnicodeGeneralCategory::zl ||
           value == UnicodeGeneralCategory::zp;
}
bool apostrophe(u32 cp) noexcept {
    return cp == '\'' || cp == 0x2019;
}
bool word_part(u32 cp) noexcept {
    return letter(cp) || digit(cp) || mark(cp) || cp == '_';
}

u32 skip_to_space(const Text& t, u32 at) noexcept {
    while (at < t.bytes) {
        const auto s = t.at(at);
        if (space(s.cp))
            break;
        at += s.width;
    }
    return at;
}
// Mention-style names: letters, digits, marks, '_', '.', '-'.
u32 skip_name(const Text& t, u32 at) noexcept {
    while (at < t.bytes) {
        const auto s = t.at(at);
        if (!word_part(s.cp) && s.cp != '.' && s.cp != '-')
            break;
        at += s.width;
    }
    return at;
}
u32 skip_alphanumeric(const Text& t, u32 at) noexcept {
    while (at < t.bytes) {
        const auto s = t.at(at);
        if (!word_part(s.cp) && !(apostrophe(s.cp) && letter(t.at(at + s.width).cp)))
            break;
        at += s.width;
    }
    return at;
}
u32 previous_scalar(const Text& t, u32 at) noexcept {
    if (!at)
        return 0;
    --at;
    while (at && (t.byte(at) & 0xC0) == 0x80)
        --at;
    return at;
}
bool token_start(const Text& t, u32 at) noexcept {
    return !at || !word_part(t.at(previous_scalar(t, at)).cp);
}
bool only_space_before(const Text& t, u32 at) noexcept {
    for (u32 i = 0; i < at; ++i)
        if (!space(t.byte(i)))
            return false;
    return true;
}
bool starts_with(const Text& t, u32 at, const char* literal) noexcept {
    for (u32 i = 0; literal[i]; ++i)
        if (t.byte(at + i) != static_cast<u8>(literal[i]))
            return false;
    return true;
}
u32 backtick_run(const Text& t, u32 at) noexcept {
    u32 end = at;
    while (t.byte(end) == '`')
        ++end;
    return end - at;
}
// Fenced blocks run to the closing fence (or the end while still being typed);
// inline code needs a closing run of the same length, else the ticks are text.
u32 skip_code(const Text& t, u32 at) noexcept {
    const u32 run = backtick_run(t, at);
    for (u32 i = at + run; i < t.bytes;) {
        if (t.byte(i) != '`') {
            ++i;
            continue;
        }
        const u32 closing = backtick_run(t, i);
        if (run >= 3 ? closing >= 3 : closing == run)
            return i + closing;
        i += closing;
    }
    return run >= 3 ? t.bytes : at + run;
}
// <@id> <@!id> <@&id> <#id> <:name:id> <a:name:id> <t:...> </command:id>
// <id:...> and <scheme://...> autolinks, all on one line.
bool angle_token(const Text& t, u32 at, u32* end) noexcept {
    constexpr u32 kLimit = 256;
    u32 close = at + 1;
    bool link = false;
    while (close < t.bytes && close - at < kLimit) {
        const u8 c = t.byte(close);
        if (c == '>' || c == '\n' || c == '<')
            break;
        link = link || starts_with(t, close, "://");
        ++close;
    }
    if (t.byte(close) != '>' || close == at + 1)
        return false;
    const u8 first = t.byte(at + 1);
    const bool known = first == '@' || first == '#' || first == ':' || first == '/' || starts_with(t, at + 1, "a:") ||
                       starts_with(t, at + 1, "t:") || starts_with(t, at + 1, "id:") || link;
    if (known)
        *end = close + 1;
    return known;
}
bool shortcode_part(u8 c) noexcept {
    return ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '+' ||
           c == '~';
}
bool shortcode(const Text& t, u32 at, u32* end) noexcept {
    u32 i = at + 1;
    while (i < t.bytes && i - at <= 64 && shortcode_part(t.byte(i)))
        ++i;
    if (i == at + 1 || t.byte(i) != ':')
        return false;
    *end = i + 1;
    return true;
}
} // namespace

bool spell_tokenizer_next(SpellTokenizer* tokenizer, SpellToken* token) noexcept {
    if (!tokenizer || !token || (!tokenizer->text.data && tokenizer->text.bytes))
        return false;
    const Text t{tokenizer->text.data, tokenizer->text.bytes};
    u32 cursor = tokenizer->cursor;
    while (cursor < t.bytes) {
        const u8 c = t.byte(cursor);
        u32 end = 0;
        if (c == '`') {
            cursor = skip_code(t, cursor);
            continue;
        }
        if (c == '<') {
            cursor = angle_token(t, cursor, &end) ? end : cursor + 1;
            continue;
        }
        if (c == '@' || c == '#') {
            cursor = token_start(t, cursor) ? skip_name(t, cursor + 1) : cursor + 1;
            continue;
        }
        if (c == ':') {
            cursor = shortcode(t, cursor, &end) ? end : cursor + 1;
            continue;
        }
        if (c == '/' && only_space_before(t, cursor)) {
            cursor = skip_to_space(t, cursor);
            continue;
        }
        const auto scalar = t.at(cursor);
        if (digit(scalar.cp)) {
            cursor = skip_alphanumeric(t, cursor);
            continue;
        }
        if (!letter(scalar.cp)) {
            cursor += scalar.width;
            continue;
        }
        const u32 first = cursor;
        u32 letters = 0;
        bool rejected = false;
        while (cursor < t.bytes) {
            const auto s = t.at(cursor);
            if (letter(s.cp))
                ++letters;
            else if (digit(s.cp) || (s.cp == '_' && word_part(t.at(cursor + 1).cp)))
                rejected = true; // identifiers and words with digits; a trailing '_' is markdown
            else if (!mark(s.cp) && !(apostrophe(s.cp) && letter(t.at(cursor + s.width).cp)))
                break;
            cursor += s.width;
        }
        const u32 last = cursor;
        const u32 next = t.at(cursor).cp;
        const u32 after = t.at(cursor + 1).cp;
        // URLs, e-mail addresses and domain-like tokens are skipped whole.
        if (starts_with(t, cursor, "://") || (next == '@' && word_part(after)) ||
            (next == '.' && letter(after))) {
            cursor = skip_to_space(t, cursor);
            continue;
        }
        if (rejected || letters < 2 || last - first > kSpellTokenMaximumBytes)
            continue;
        tokenizer->cursor = cursor;
        *token = {first, last};
        return true;
    }
    tokenizer->cursor = t.bytes;
    return false;
}

} // namespace maicord
