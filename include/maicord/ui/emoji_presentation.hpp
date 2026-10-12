#pragma once
#include "maicord/core/slice.hpp"
namespace maicord {
struct TextEmojiCursor {u32 byte,unit;};
struct TextEmojiRange {u32 first_byte,end_byte,first_unit,end_unit;};
enum class TextEmojiNext:u8 {end,range,invalid_input};
// MaiCord's font-override policy, shared by native and legacy layout.
// Start with a zero cursor and keep the same UTF-8 input. Success
// returns maximal adjacent emoji ranges in both byte and UTF-16 coordinates.
// At end the cursor contains the complete input lengths. Errors clear this
// call's output and leave its cursor unchanged. Previously returned ranges
// remain a prefix; callers that need all-or-nothing output validate the input first.
// The input, cursor and output are disjoint. No allocation or OS call is used.
// Emoji-property bases keep selectors, modifiers, tags and joined emoji bases.
// Any VS15 in that sequence suppresses its override. ASCII digits/#/* require a
// keycap; adjacent regional indicators are coalesced without pairing them here.
// These are font ranges, not script/bidi items, graphemes or shaping clusters.
TextEmojiNext text_emoji_next(Utf8View,TextEmojiCursor*,TextEmojiRange*) noexcept;
static_assert(sizeof(TextEmojiCursor)==8&&sizeof(TextEmojiRange)==16);
}
