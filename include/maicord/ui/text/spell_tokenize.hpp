#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {

// Words worth spell checking in composer markdown, as UTF-8 byte ranges.
// Skipped: fenced and inline code, <...> mentions/emoji/timestamps/command
// mentions/autolinks, plain @name and #name tokens, :shortcodes:, a leading
// /command, URLs (scheme://, www., domain-like a.b), e-mail addresses,
// numbers and words with digits or underscores, single letters, words longer
// than kSpellTokenMaximumBytes and scripts written without spaces (Han, Kana,
// Thai, ...). Apostrophes inside words are kept (don't, it's).
inline constexpr u32 kSpellTokenMaximumBytes = 64;

struct SpellToken {
    u32 first, last;
};
struct SpellTokenizer {
    Utf8View text;
    u32 cursor;
};
constexpr SpellTokenizer spell_tokenizer(Utf8View text) noexcept {
    return {text, 0};
}
// Iterates from the start of the text; code-fence state depends on it.
bool spell_tokenizer_next(SpellTokenizer* tokenizer, SpellToken* token) noexcept;

} // namespace maicord
