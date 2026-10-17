#include "maicord/ui/secure_edit.hpp"
#include "secure_edit_history.hpp"

#include "maicord/core/memory.hpp"

#include <Windows.h>

namespace maicord {
namespace {

bool ready(const SecureEdit* edit) noexcept {
    return edit && edit->storage.data && edit->storage.locked && edit->storage.capacity != 0 &&
           edit->anchor <= edit->storage.bytes && edit->caret <= edit->storage.bytes;
}

u32 selection_begin(const SecureEdit& edit) noexcept {
    return minimum(edit.anchor, edit.caret);
}

u32 selection_end(const SecureEdit& edit) noexcept {
    return maximum(edit.anchor, edit.caret);
}

u32 previous_boundary(const SecureEdit& edit, u32 position) noexcept {
    if (position == 0) {
        return 0;
    }
    u32 offset = position - 1;
    while (offset != 0 && (edit.storage.data[offset] & 0xC0u) == 0x80u) {
        --offset;
    }
    return offset;
}

u32 next_boundary(const SecureEdit& edit, u32 position) noexcept {
    if (position >= edit.storage.bytes) {
        return edit.storage.bytes;
    }
    const Utf8DecodeResult decoded = utf8_decode_one(
        reinterpret_cast<const char*>(edit.storage.data + position), edit.storage.bytes - position);
    return decoded.valid ? position + decoded.width : edit.storage.bytes;
}

void move_tail(u8* data, u32 destination, u32 source, u32 bytes) noexcept {
    if (destination < source) {
        for (u32 index = 0; index < bytes; ++index) {
            data[destination + index] = data[source + index];
        }
    } else if (destination > source) {
        for (u32 index = bytes; index != 0; --index) {
            data[destination + index - 1] = data[source + index - 1];
        }
    }
}

bool codepoints_fit(const SecureEdit& edit, u32 begin, u32 end, u32 inserted) noexcept {
    if (edit.maximum_codepoints == 0) return true;
    const auto* text = reinterpret_cast<const char*>(edit.storage.data);
    const u32 retained = utf8_validate({text, begin}).codepoints +
                         utf8_validate({text + end, edit.storage.bytes - end}).codepoints;
    return retained <= edit.maximum_codepoints && inserted <= edit.maximum_codepoints - retained;
}

SecureEditResult replace_selection(SecureEdit* edit, const char* inserted, u32 inserted_bytes,
                                    u32 inserted_codepoints = 0, u32 original_anchor = ~0u,
                                    SecureEditHistoryInput input = SecureEditHistoryInput::isolated, u64 time_ms = 0) noexcept {
    if (!ready(edit) || (!inserted && inserted_bytes != 0)) {
        return SecureEditResult::invalid_input;
    }
    const u32 begin = selection_begin(*edit);
    const u32 end = selection_end(*edit);
    const u32 removed = end - begin;
    const u32 old_bytes = edit->storage.bytes;
    const u32 retained = old_bytes - removed;
    if (inserted_bytes > edit->storage.capacity - retained ||
        !codepoints_fit(*edit, begin, end, inserted_codepoints)) {
        return SecureEditResult::capacity;
    }
    if (removed == 0 && inserted_bytes == 0) {
        return SecureEditResult::unchanged;
    }
    secure_edit_history_begin(edit, begin, removed, inserted_bytes,
        original_anchor == ~0u ? edit->anchor : original_anchor, edit->caret, input, time_ms);
    const u32 tail_bytes = old_bytes - end;
    move_tail(edit->storage.data, begin + inserted_bytes, end, tail_bytes);
    for (u32 index = 0; index < inserted_bytes; ++index) {
        edit->storage.data[begin + index] = static_cast<u8>(inserted[index]);
    }
    const u32 new_bytes = retained + inserted_bytes;
    if (new_bytes < old_bytes) {
        SecureZeroMemory(edit->storage.data + new_bytes, old_bytes - new_bytes);
    }
    edit->storage.bytes = new_bytes;
    edit->anchor = begin + inserted_bytes;
    edit->caret = edit->anchor;
    edit->pending_high_surrogate = 0;
    ++edit->revision;
    secure_edit_history_finish(edit);
    return SecureEditResult::changed;
}

bool scalar_allowed(const SecureEdit& edit, u32 codepoint) noexcept {
    return (edit.multiline && (codepoint == '\r' || codepoint == '\n')) ||
           (codepoint >= 0x20u && codepoint != 0x7Fu && codepoint <= 0x10FFFFu &&
            !(codepoint >= 0xD800u && codepoint <= 0xDFFFu));
}

bool text_allowed(const SecureEdit& edit, Utf8View text) noexcept {
    // The caller has already validated UTF-8. C0 controls and DEL are single
    // ASCII bytes and cannot occur inside a valid multibyte scalar; avoid a
    // second Unicode decode just to check the field's control policy.
    for (u32 offset = 0; offset < text.bytes; ++offset) {
        const u8 byte = static_cast<u8>(text.data[offset]);
        if ((byte < 0x20u || byte == 0x7Fu) &&
            !(edit.multiline && (byte == '\r' || byte == '\n'))) return false;
    }
    return true;
}

u32 line_boundary(const SecureEdit& edit, u32 position, bool start) noexcept {
    // Treat a caret inside CRLF as belonging to the preceding line. ACP still
    // represents both code units; no normalization can desynchronize TSF.
    if (position != 0 && position < edit.storage.bytes &&
        edit.storage.data[position - 1] == '\r' && edit.storage.data[position] == '\n') --position;
    if (start) {
        while (position != 0 && edit.storage.data[position - 1] != '\r' &&
               edit.storage.data[position - 1] != '\n') --position;
    } else {
        while (position < edit.storage.bytes && edit.storage.data[position] != '\r' &&
               edit.storage.data[position] != '\n') ++position;
    }
    return position;
}

u32 codepoint_position(const SecureEdit& edit, u32 byte_position) noexcept {
    u32 offset = 0;
    u32 codepoints = 0;
    while (offset < byte_position) {
        const Utf8DecodeResult decoded = utf8_decode_one(
            reinterpret_cast<const char*>(edit.storage.data + offset), edit.storage.bytes - offset);
        if (!decoded.valid || offset + decoded.width > byte_position) {
            return ~0u;
        }
        offset += decoded.width;
        ++codepoints;
    }
    return offset == byte_position ? codepoints : ~0u;
}

u32 byte_position(const SecureEdit& edit, u32 codepoint) noexcept {
    u32 offset = 0;
    while (codepoint != 0 && offset < edit.storage.bytes) {
        const Utf8DecodeResult decoded = utf8_decode_one(
            reinterpret_cast<const char*>(edit.storage.data + offset), edit.storage.bytes - offset);
        if (!decoded.valid) {
            return ~0u;
        }
        offset += decoded.width;
        --codepoint;
    }
    return codepoint == 0 ? offset : ~0u;
}

u32 utf16_position(const SecureEdit& edit, u32 byte_position) noexcept {
    u32 offset = 0;
    u32 units = 0;
    while (offset < byte_position) {
        const Utf8DecodeResult decoded = utf8_decode_one(
            reinterpret_cast<const char*>(edit.storage.data + offset), edit.storage.bytes - offset);
        if (!decoded.valid || offset + decoded.width > byte_position) {
            return ~0u;
        }
        units += decoded.codepoint > 0xFFFFu ? 2u : 1u;
        offset += decoded.width;
    }
    return offset == byte_position ? units : ~0u;
}

u32 byte_position_for_utf16(const SecureEdit& edit, u32 target) noexcept {
    u32 offset = 0;
    u32 units = 0;
    while (offset < edit.storage.bytes && units < target) {
        const Utf8DecodeResult decoded = utf8_decode_one(
            reinterpret_cast<const char*>(edit.storage.data + offset), edit.storage.bytes - offset);
        if (!decoded.valid) {
            return ~0u;
        }
        const u32 width = decoded.codepoint > 0xFFFFu ? 2u : 1u;
        if (units + width > target) {
            return ~0u;
        }
        units += width;
        offset += decoded.width;
    }
    return units == target ? offset : ~0u;
}

bool decode_utf16_one(ConstSlice<u16> input, u32 offset, u32* codepoint, u32* width) noexcept {
    if (!codepoint || !width || offset >= input.count || !input.data) {
        return false;
    }
    const u32 first = input[offset];
    if (first >= 0xD800u && first <= 0xDBFFu) {
        if (offset + 1 >= input.count) {
            return false;
        }
        const u32 second = input[offset + 1];
        if (second < 0xDC00u || second > 0xDFFFu) {
            return false;
        }
        *codepoint = 0x10000u + ((first - 0xD800u) << 10) + (second - 0xDC00u);
        *width = 2;
        return true;
    }
    if (first >= 0xDC00u && first <= 0xDFFFu) {
        return false;
    }
    *codepoint = first;
    *width = 1;
    return true;
}

} // namespace

bool secure_edit_initialize(SecureEdit* edit, u32 capacity, bool password,
                            SecureEditPolicy policy) noexcept {
    if (!edit || capacity == 0 || (password && policy.multiline)) {
        return false;
    }
    *edit = SecureEdit{};
    if (!secure_buffer_initialize(&edit->storage, capacity).ok()) {
        return false;
    }
    edit->password = password;
    edit->multiline = policy.multiline;
    edit->maximum_codepoints = policy.maximum_codepoints;
    return true;
}

void secure_edit_release(SecureEdit* edit) noexcept {
    if (!edit) {
        return;
    }
    secure_edit_history_release(edit);
    secure_buffer_release(&edit->storage);
    *edit = SecureEdit{};
}

SecureEditResult secure_edit_assign(SecureEdit* edit, Utf8View text) noexcept {
    if (!ready(edit) || (!text.data && text.bytes != 0)) {
        return SecureEditResult::invalid_input;
    }
    const auto validation = utf8_validate(text);
    if (!validation.valid) {
        return SecureEditResult::invalid_utf8;
    }
    if (!text_allowed(*edit, text)) return SecureEditResult::invalid_input;
    if (text.bytes > edit->storage.capacity ||
        (edit->maximum_codepoints != 0 && validation.codepoints > edit->maximum_codepoints)) {
        return SecureEditResult::capacity;
    }
    secure_edit_reset_history(edit);
    const bool same_size = text.bytes == edit->storage.bytes;
    bool same = same_size;
    for (u32 index = 0; same && index < text.bytes; ++index) {
        same = edit->storage.data[index] == static_cast<u8>(text.data[index]);
    }
    if (same) {
        edit->anchor = text.bytes;
        edit->caret = text.bytes;
        edit->pending_high_surrogate = 0;
        return SecureEditResult::unchanged;
    }
    if (!secure_buffer_assign(&edit->storage,
                              ConstSlice<u8>{reinterpret_cast<const u8*>(text.data), text.bytes})
             .ok()) {
        return SecureEditResult::invalid_input;
    }
    edit->anchor = text.bytes;
    edit->caret = text.bytes;
    edit->pending_high_surrogate = 0;
    ++edit->revision;
    return SecureEditResult::changed;
}

SecureEditResult secure_edit_clear(SecureEdit* edit) noexcept {
    if (!ready(edit)) {
        return SecureEditResult::invalid_input;
    }
    secure_edit_reset_history(edit);
    const bool text_changed = edit->storage.bytes != 0;
    const bool changed =
        text_changed || edit->anchor != 0 || edit->caret != 0 || edit->pending_high_surrogate != 0;
    secure_buffer_clear(&edit->storage);
    edit->anchor = 0;
    edit->caret = 0;
    edit->pending_high_surrogate = 0;
    if (text_changed) {
        ++edit->revision;
    }
    return changed ? SecureEditResult::changed : SecureEditResult::unchanged;
}

SecureEditResult secure_edit_insert_scalar(SecureEdit* edit, u32 codepoint, u64 time_ms) noexcept {
    if (!ready(edit) || !scalar_allowed(*edit, codepoint)) {
        return SecureEditResult::invalid_input;
    }
    char encoded[4]{};
    const u8 bytes = utf8_encode(codepoint, encoded);
    return bytes != 0 ? replace_selection(edit, encoded, bytes, 1, ~0u,
        codepoint == '\r' || codepoint == '\n' ? SecureEditHistoryInput::isolated : SecureEditHistoryInput::typing,
        time_ms) : SecureEditResult::invalid_input;
}

SecureEditResult secure_edit_insert_utf8(SecureEdit* edit, Utf8View text) noexcept {
    if (!ready(edit) || (!text.data && text.bytes != 0)) {
        return SecureEditResult::invalid_input;
    }
    const Utf8Validation validation = utf8_validate(text);
    if (!validation.valid) {
        return SecureEditResult::invalid_utf8;
    }
    if (!text_allowed(*edit, text)) return SecureEditResult::invalid_input;
    return replace_selection(edit, text.data, text.bytes, validation.codepoints);
}

SecureEditResult secure_edit_insert_utf16(SecureEdit* edit, u16 code_unit, u64 time_ms) noexcept {
    if (!ready(edit)) {
        return SecureEditResult::invalid_input;
    }
    if (code_unit >= 0xD800u && code_unit <= 0xDBFFu) {
        edit->pending_high_surrogate = code_unit;
        return SecureEditResult::pending_surrogate;
    }
    if (code_unit >= 0xDC00u && code_unit <= 0xDFFFu) {
        if (edit->pending_high_surrogate == 0) {
            return SecureEditResult::invalid_input;
        }
        const u32 high = edit->pending_high_surrogate - 0xD800u;
        const u32 low = code_unit - 0xDC00u;
        edit->pending_high_surrogate = 0;
        return secure_edit_insert_scalar(edit, 0x10000u + (high << 10) + low, time_ms);
    }
    edit->pending_high_surrogate = 0;
    return secure_edit_insert_scalar(edit, code_unit, time_ms);
}

SecureEditResult secure_edit_backspace(SecureEdit* edit, u64 time_ms) noexcept {
    if (!ready(edit)) {
        return SecureEditResult::invalid_input;
    }
    if (edit->anchor != edit->caret) {
        return replace_selection(edit, nullptr, 0);
    }
    if (edit->caret == 0) {
        return SecureEditResult::unchanged;
    }
    edit->anchor = previous_boundary(*edit, edit->caret);
    return replace_selection(edit, nullptr, 0, 0, edit->caret, SecureEditHistoryInput::backspace, time_ms);
}

SecureEditResult secure_edit_delete_forward(SecureEdit* edit, u64 time_ms) noexcept {
    if (!ready(edit)) {
        return SecureEditResult::invalid_input;
    }
    if (edit->anchor != edit->caret) {
        return replace_selection(edit, nullptr, 0);
    }
    if (edit->caret == edit->storage.bytes) {
        return SecureEditResult::unchanged;
    }
    edit->anchor = next_boundary(*edit, edit->caret);
    return replace_selection(edit, nullptr, 0, 0, edit->caret, SecureEditHistoryInput::delete_forward, time_ms);
}

SecureEditResult secure_edit_move(SecureEdit* edit, SecureEditMove movement,
                                  bool extend_selection) noexcept {
    if (!ready(edit)) {
        return SecureEditResult::invalid_input;
    }
    u32 position = edit->caret;
    secure_edit_break_history_run(edit);
    if (movement == SecureEditMove::line_home || movement == SecureEditMove::line_end) {
        position = line_boundary(*edit, position, movement == SecureEditMove::line_home);
    } else if (!extend_selection && edit->anchor != edit->caret &&
               (movement == SecureEditMove::previous || movement == SecureEditMove::next)) {
        position = movement == SecureEditMove::previous || movement == SecureEditMove::home
                       ? selection_begin(*edit)
                       : selection_end(*edit);
    } else {
        switch (movement) {
        case SecureEditMove::previous:
            position = previous_boundary(*edit, position);
            break;
        case SecureEditMove::next:
            position = next_boundary(*edit, position);
            break;
        case SecureEditMove::home:
            position = 0;
            break;
        case SecureEditMove::end:
            position = edit->storage.bytes;
            break;
        case SecureEditMove::line_home:
        case SecureEditMove::line_end:
            break;
        }
    }
    const bool changed = position != edit->caret || (!extend_selection && edit->anchor != position);
    edit->caret = position;
    if (!extend_selection) {
        edit->anchor = position;
    }
    edit->pending_high_surrogate = 0;
    return changed ? SecureEditResult::changed : SecureEditResult::unchanged;
}

SecureEditResult secure_edit_set_codepoint(SecureEdit* edit, u32 codepoint,
                                           bool extend_selection) noexcept {
    if (!ready(edit)) {
        return SecureEditResult::invalid_input;
    }
    const u32 position = byte_position(*edit, codepoint);
    if (position == ~0u) {
        return SecureEditResult::invalid_input;
    }
    secure_edit_break_history_run(edit);
    const bool changed = edit->caret != position || (!extend_selection && edit->anchor != position);
    edit->caret = position;
    if (!extend_selection) {
        edit->anchor = position;
    }
    edit->pending_high_surrogate = 0;
    return changed ? SecureEditResult::changed : SecureEditResult::unchanged;
}

SecureEditResult secure_edit_select_all(SecureEdit* edit) noexcept {
    if (!ready(edit)) {
        return SecureEditResult::invalid_input;
    }
    secure_edit_break_history_run(edit);
    const bool changed = edit->anchor != 0 || edit->caret != edit->storage.bytes;
    edit->anchor = 0;
    edit->caret = edit->storage.bytes;
    edit->pending_high_surrogate = 0;
    return changed ? SecureEditResult::changed : SecureEditResult::unchanged;
}

SecureEditResult secure_edit_select_word(SecureEdit* edit, u32 position) noexcept {
    if (!ready(edit)) return SecureEditResult::invalid_input;
    u32 first = 0, last = 0;
    if (!utf8_word_range_near({reinterpret_cast<const char*>(edit->storage.data), edit->storage.bytes}, position, &first, &last))
        return SecureEditResult::invalid_input;
    secure_edit_break_history_run(edit);
    const bool changed = edit->anchor != first || edit->caret != last;
    edit->anchor = first; edit->caret = last; edit->pending_high_surrogate = 0;
    return changed ? SecureEditResult::changed : SecureEditResult::unchanged;
}

SecureEditResult secure_edit_select_line(SecureEdit* edit, u32 position) noexcept {
    if (!ready(edit)) return SecureEditResult::invalid_input;
    u32 first = 0, last = 0;
    if (!utf8_line_range({reinterpret_cast<const char*>(edit->storage.data), edit->storage.bytes}, position, &first, &last))
        return SecureEditResult::invalid_input;
    secure_edit_break_history_run(edit);
    const bool changed = edit->anchor != first || edit->caret != last;
    edit->anchor = first; edit->caret = last; edit->pending_high_surrogate = 0;
    return changed ? SecureEditResult::changed : SecureEditResult::unchanged;
}

void secure_edit_cancel_composition(SecureEdit* edit) noexcept {
    if (edit) {
        edit->pending_high_surrogate = 0;
    }
}

ConstSlice<u8> secure_edit_view(const SecureEdit& edit) noexcept {
    return secure_buffer_view(edit.storage);
}

SecureEditResult secure_edit_utf16_length(const SecureEdit& edit, u32* units) noexcept {
    if (!units || !ready(&edit)) {
        return SecureEditResult::invalid_input;
    }
    const u32 result = utf16_position(edit, edit.storage.bytes);
    if (result == ~0u) {
        return SecureEditResult::invalid_utf8;
    }
    *units = result;
    return SecureEditResult::unchanged;
}

SecureEditResult secure_edit_utf16_selection(const SecureEdit& edit,
                                             SecureEditUtf16Selection* selection) noexcept {
    if (!selection || !ready(&edit)) {
        return SecureEditResult::invalid_input;
    }
    const u32 anchor = utf16_position(edit, edit.anchor);
    const u32 caret = utf16_position(edit, edit.caret);
    if (anchor == ~0u || caret == ~0u) {
        return SecureEditResult::invalid_utf8;
    }
    *selection = SecureEditUtf16Selection{anchor, caret};
    return SecureEditResult::unchanged;
}

SecureEditResult secure_edit_codepoint_from_utf16(const SecureEdit& edit, u32 position,
                                                  u32* codepoint) noexcept {
    if (!codepoint || !ready(&edit)) {
        return SecureEditResult::invalid_input;
    }
    const u32 byte = byte_position_for_utf16(edit, position);
    if (byte == ~0u) {
        return SecureEditResult::invalid_utf16;
    }
    const u32 value = codepoint_position(edit, byte);
    if (value == ~0u) {
        return SecureEditResult::invalid_utf8;
    }
    *codepoint = value;
    return SecureEditResult::unchanged;
}

SecureEditResult secure_edit_set_utf16_selection(SecureEdit* edit,
                                                 SecureEditUtf16Selection selection) noexcept {
    if (!ready(edit)) {
        return SecureEditResult::invalid_input;
    }
    const u32 anchor = byte_position_for_utf16(*edit, selection.anchor);
    const u32 caret = byte_position_for_utf16(*edit, selection.caret);
    if (anchor == ~0u || caret == ~0u) {
        return SecureEditResult::invalid_utf16;
    }
    secure_edit_break_history_run(edit);
    const bool changed = edit->anchor != anchor || edit->caret != caret;
    edit->anchor = anchor;
    edit->caret = caret;
    edit->pending_high_surrogate = 0;
    return changed ? SecureEditResult::changed : SecureEditResult::unchanged;
}

SecureEditResult secure_edit_copy_utf16(const SecureEdit& edit, u32 start, u32 end, Slice<u16> output,
                                        u32* written, u32* next) noexcept {
    if (!written || !next || (!output.data && output.count != 0) || !ready(&edit) || start > end)
        return SecureEditResult::invalid_input;
    return utf8_copy_utf16_range({reinterpret_cast<const char*>(edit.storage.data), edit.storage.bytes},
        start, end, output, written, next) ? SecureEditResult::unchanged : SecureEditResult::invalid_utf16;
}
SecureEditResult secure_edit_replace_utf16(SecureEdit* edit, u32 start, u32 end, ConstSlice<u16> input,
                                           SecureEditUtf16Change* change) noexcept {
    if (!change || (!input.data && input.count != 0) || !ready(edit) || start > end) {
        return SecureEditResult::invalid_input;
    }
    if (input.count > edit->storage.capacity * 2u) {
        return SecureEditResult::capacity;
    }
    const u32 begin_byte = byte_position_for_utf16(*edit, start);
    const u32 end_byte = byte_position_for_utf16(*edit, end);
    if (begin_byte == ~0u || end_byte == ~0u) {
        return SecureEditResult::invalid_utf16;
    }
    u32 input_offset = 0;
    u32 inserted_bytes = 0;
    u32 inserted_codepoints = 0;
    while (input_offset < input.count) {
        u32 codepoint = 0;
        u32 width = 0;
        if (!decode_utf16_one(input, input_offset, &codepoint, &width) || !scalar_allowed(*edit, codepoint)) {
            return SecureEditResult::invalid_utf16;
        }
        char encoded[4]{};
        const u8 bytes = utf8_encode(codepoint, encoded);
        if (bytes == 0 || inserted_bytes > ~0u - bytes) {
            return SecureEditResult::invalid_utf16;
        }
        inserted_bytes += bytes;
        ++inserted_codepoints;
        input_offset += width;
    }
    const u32 old_bytes = edit->storage.bytes;
    const u32 removed_bytes = end_byte - begin_byte;
    const u32 retained_bytes = old_bytes - removed_bytes;
    if (inserted_bytes > edit->storage.capacity - retained_bytes ||
        !codepoints_fit(*edit, begin_byte, end_byte, inserted_codepoints)) {
        return SecureEditResult::capacity;
    }
    *change = SecureEditUtf16Change{start, end, start + input.count};
    if (removed_bytes == 0 && inserted_bytes == 0) {
        return SecureEditResult::unchanged;
    }
    secure_edit_history_begin(edit, begin_byte, removed_bytes, inserted_bytes, edit->anchor, edit->caret);
    const u32 tail_bytes = old_bytes - end_byte;
    move_tail(edit->storage.data, begin_byte + inserted_bytes, end_byte, tail_bytes);
    input_offset = 0;
    u32 destination = begin_byte;
    while (input_offset < input.count) {
        u32 codepoint = 0;
        u32 width = 0;
        if (!decode_utf16_one(input, input_offset, &codepoint, &width)) {
            return SecureEditResult::invalid_utf16;
        }
        char encoded[4]{};
        const u8 bytes = utf8_encode(codepoint, encoded);
        for (u32 index = 0; index < bytes; ++index) {
            edit->storage.data[destination + index] = static_cast<u8>(encoded[index]);
        }
        destination += bytes;
        input_offset += width;
    }
    const u32 new_bytes = retained_bytes + inserted_bytes;
    if (new_bytes < old_bytes) {
        SecureZeroMemory(edit->storage.data + new_bytes, old_bytes - new_bytes);
    }
    edit->storage.bytes = new_bytes;
    edit->anchor = destination;
    edit->caret = destination;
    edit->pending_high_surrogate = 0;
    ++edit->revision;
    secure_edit_history_finish(edit);
    return SecureEditResult::changed;
}

SecureEditResult secure_edit_render_window(const SecureEdit& edit, u32 maximum_codepoints,
                                           Slice<char> output, u32* output_bytes,
                                           SecureEditRenderWindow* window) noexcept {
    if (!output_bytes || !window || maximum_codepoints == 0 || (!output.data && output.count != 0) ||
        !ready(&edit)) {
        return SecureEditResult::invalid_input;
    }
    const Utf8Validation validation =
        utf8_validate(Utf8View{reinterpret_cast<const char*>(edit.storage.data), edit.storage.bytes});
    if (!validation.valid) {
        return SecureEditResult::invalid_utf8;
    }
    const u32 anchor = codepoint_position(edit, edit.anchor);
    const u32 caret = codepoint_position(edit, edit.caret);
    if (anchor == ~0u || caret == ~0u) {
        return SecureEditResult::invalid_utf8;
    }
    u32 first = 0;
    if (validation.codepoints > maximum_codepoints) {
        const u32 selection_first = minimum(anchor, caret);
        const u32 selection_last = maximum(anchor, caret);
        first = anchor != caret && selection_last - selection_first <= maximum_codepoints
                    ? selection_last > maximum_codepoints ? selection_last - maximum_codepoints : 0
                : caret > maximum_codepoints ? caret - maximum_codepoints
                                             : 0;
        if (first + maximum_codepoints > validation.codepoints) {
            first = validation.codepoints - maximum_codepoints;
        }
    }
    const u32 visible = minimum(maximum_codepoints, validation.codepoints - first);
    const u32 last = first + visible;
    const u32 begin_byte = byte_position(edit, first);
    const u32 end_byte = byte_position(edit, last);
    if (begin_byte == ~0u || end_byte == ~0u) {
        return SecureEditResult::invalid_utf8;
    }
    const u32 required = edit.password ? visible * 3u : end_byte - begin_byte;
    if (required > output.count || (required != 0 && !output.data)) {
        return SecureEditResult::capacity;
    }
    if (edit.password) {
        for (u32 index = 0; index < visible; ++index) {
            output.data[index * 3u] = static_cast<char>(0xE2u);
            output.data[index * 3u + 1u] = static_cast<char>(0x80u);
            output.data[index * 3u + 2u] = static_cast<char>(0xA2u);
        }
    } else {
        for (u32 index = begin_byte; index < end_byte; ++index) {
            output.data[index - begin_byte] = static_cast<char>(edit.storage.data[index]);
        }
    }
    const auto relative = [first, last](u32 position) noexcept {
        const u32 clamped = minimum(maximum(position, first), last);
        return clamped - first;
    };
    *output_bytes = required;
    *window = SecureEditRenderWindow{first, visible, relative(anchor), relative(caret)};
    return SecureEditResult::unchanged;
}

} // namespace maicord

