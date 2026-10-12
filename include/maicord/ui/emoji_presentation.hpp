#pragma once
#include "maicord/core/slice.hpp"
namespace maicord {
struct TextEmojiCursor {u32 byte,unit;};
struct TextEmojiRange {u32 first_byte,end_byte,first_unit,end_unit;};
enum class TextEmojiNext:u8 {end,range,invalid_input};
// Font-override policy for emoji. Start with a zero cursor and pass the same
// UTF-8 input on every call. Success returns the next maximal run of adjacent
// emoji in both byte and UTF-16 coordinates; at end the cursor holds the complete
// input lengths. Errors clear this call's output and leave the cursor unchanged,
// while ranges from earlier calls stay valid, so callers needing all-or-nothing
// output validate the input first. Input, cursor and output are disjoint.
// Emoji-property bases keep selectors, modifiers, tags and joined emoji bases;
// any VS15 in that sequence suppresses its override. ASCII digits/#/* need a
// keycap; adjacent regional indicators coalesce without pairing. These are font
// ranges, not graphemes or shaping clusters.
TextEmojiNext text_emoji_next(Utf8View,TextEmojiCursor*,TextEmojiRange*) noexcept;
static_assert(sizeof(TextEmojiCursor)==8&&sizeof(TextEmojiRange)==16);
}
