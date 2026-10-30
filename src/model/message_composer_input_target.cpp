#include "maicord/model/message_composer.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
MessageComposer* composer(const TextInputTarget& t) noexcept { return static_cast<MessageComposer*>(t.context); }
// Editing is valid before a DM exists. Channel authorization belongs to the
// send controller and UI host; native text ownership uses document identity.
bool ready(const TextInputTarget& t) noexcept {
    const auto* c = composer(t);
    return c && c->text && c->capacity && c->capacity <= kMessageComposerMaximumBytes &&
        c->bytes <= c->capacity && c->anchor <= c->bytes && c->caret <= c->bytes &&
        c->document_identity && c->document_identity == t.identity;
}
u32 revision(const TextInputTarget& t) noexcept { return composer(t)->text_revision; }
TextInputResult length(const TextInputTarget& t, u32* n) noexcept {
    const auto& c = *composer(t);
    return utf8_utf16_position_for_byte_offset(message_composer_text(c), c.bytes, n) ? TextInputResult::unchanged : TextInputResult::invalid_utf8;
}
TextInputResult selection(const TextInputTarget& t, TextInputSelection* s) noexcept {
    if (!s) return TextInputResult::invalid_input;
    const auto& c = *composer(t); TextInputSelection value{};
    if (!utf8_utf16_position_for_byte_offset(message_composer_text(c), c.anchor, &value.anchor) ||
        !utf8_utf16_position_for_byte_offset(message_composer_text(c), c.caret, &value.caret)) return TextInputResult::invalid_utf8;
    *s = value; return TextInputResult::unchanged;
}
TextInputResult result(MessageComposerResult r) noexcept {
    return r == MessageComposerResult::changed ? TextInputResult::changed : r == MessageComposerResult::unchanged ? TextInputResult::unchanged :
        r == MessageComposerResult::capacity ? TextInputResult::capacity : r == MessageComposerResult::invalid_utf8 ? TextInputResult::invalid_utf8 : TextInputResult::invalid_input;
}
TextInputResult select(const TextInputTarget& t, TextInputSelection s) noexcept {
    auto* c = composer(t); u32 anchor = 0, caret = 0;
    if (!utf8_byte_offset_for_utf16_position(message_composer_text(*c), s.anchor, &anchor) ||
        !utf8_byte_offset_for_utf16_position(message_composer_text(*c), s.caret, &caret)) return TextInputResult::invalid_utf16;
    return result(message_composer_set_selection(c, anchor, caret));
}
TextInputResult copy(const TextInputTarget& t, u32 a, u32 b, Slice<u16> out, u32* n, u32* next) noexcept {
    if (!n || !next || (!out.data && out.count) || a > b) return TextInputResult::invalid_input;
    return utf8_copy_utf16_range(message_composer_text(*composer(t)), a, b, out, n, next) ? TextInputResult::unchanged : TextInputResult::invalid_utf16;
}
TextInputResult replace(const TextInputTarget& t, u32 a, u32 b, ConstSlice<u16> input, TextInputChange* change) noexcept {
    if (!change || (!input.data && input.count) || a > b) return TextInputResult::invalid_input;
    auto* c = composer(t); u32 first = 0, last = 0;
    if (!utf8_byte_offset_for_utf16_position(message_composer_text(*c), a, &first) ||
        !utf8_byte_offset_for_utf16_position(message_composer_text(*c), b, &last)) return TextInputResult::invalid_utf16;
    const u32 available = c->capacity - (c->bytes - (last - first));
    if (input.count > available) return TextInputResult::capacity;
    // Typed and IME input is a few units; only a large paste needs the full-size
    // scratch (UI thread only). A unit encodes to at most three bytes.
    char small[256];
    static char large[kMessageComposerMaximumBytes];
    char* const converted = input.count <= sizeof(small) / 3 ? small : large;
    u32 bytes = 0; TextInputResult converted_result = TextInputResult::unchanged;
    for (u32 i = 0; i < input.count; ++i) {
        u32 scalar = input[i];
        if (scalar >= 0xD800 && scalar <= 0xDBFF) {
            if (i + 1 >= input.count || input[i + 1] < 0xDC00 || input[i + 1] > 0xDFFF) { converted_result = TextInputResult::invalid_utf16; break; }
            scalar = 0x10000 + ((scalar - 0xD800) << 10) + (input[++i] - 0xDC00);
        } else if (scalar >= 0xDC00 && scalar <= 0xDFFF) { converted_result = TextInputResult::invalid_utf16; break; }
        char encoded[4]; const u32 count = utf8_encode(scalar, encoded);
        if (count > available - bytes) { converted_result = TextInputResult::capacity; break; }
        copy_bytes(converted + bytes, encoded, count); bytes += count;
    }
    if (converted_result == TextInputResult::unchanged) converted_result = result(message_composer_replace_range(c, first, last, {converted, bytes}));
    secure_clear_bytes(converted, bytes);
    if (converted_result == TextInputResult::changed || converted_result == TextInputResult::unchanged) *change = {a, b, a + input.count};
    return converted_result;
}
void cancel(const TextInputTarget& t) noexcept { composer(t)->pending_high_surrogate = 0; message_composer_break_history_run(composer(t)); }
TextEditHistoryState history(MessageComposer& c) noexcept { return {{reinterpret_cast<u8*>(c.text), c.capacity, c.bytes}, c.anchor, c.caret, c.revision, c.history}; }
bool begin(const TextInputTarget& t) noexcept {
    auto* c = composer(t);
    if (!c->history && !message_composer_enable_history(c)) return false;
    auto state = history(*c); return text_edit_begin_history_group(&state);
}
void end(const TextInputTarget& t) noexcept { auto state = history(*composer(t)); text_edit_end_history_group(&state); }
const TextInputOperations operations{ready, revision, length, selection, select, copy, replace, cancel, begin, end};
}
TextInputTarget message_composer_input_target(MessageComposer* c) noexcept { return {c, &operations, c ? c->document_identity : 0}; }
}
