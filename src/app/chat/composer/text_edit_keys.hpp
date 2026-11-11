#pragma once
#include "maicord/core/utf.hpp"
#include "maicord/model/chat/forwarded_input.hpp"
#include "maicord/platform/window/clipboard.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/model/chat/message_composer.hpp"

namespace maicord::app_detail {
// Select all, copy, cut, undo/redo, deletion and caret movement keys of a text field; cut is one undo step.
// False, with *result untouched, for any other key; paste stays with the caller.
__declspec(noinline) inline bool message_composer_edit_key(MessageComposer* model, u32 key, bool control,
                                                           bool shift,
                                                           MessageComposerResult* result) noexcept {
    if (control && (key == 'C' || key == 'X')) {
        const u32 first = minimum(model->anchor, model->caret), last = maximum(model->anchor, model->caret);
        if (last > first && clipboard_write_utf8({model->text + first, last - first}) && key == 'X')
            *result = message_composer_replace_range(model, first, last, {});
    } else if (control && key == 'A')
        *result = message_composer_select_all(model);
    else if (control && key == 'Z')
        *result = shift ? message_composer_redo(model) : message_composer_undo(model);
    else if (control && key == 'Y')
        *result = message_composer_redo(model);
    else if (key == key::backspace)
        *result = control ? message_composer_delete_word(model, false) : message_composer_backspace(model);
    else if (key == key::del)
        *result = control ? message_composer_delete_word(model, true)
                          : message_composer_delete_forward(model);
    else if (key == key::left || key == key::right)
        *result = message_composer_move(
            model,
            control ? (key == key::left ? MessageComposerMove::previous_word : MessageComposerMove::next_word)
                    : (key == key::left ? MessageComposerMove::previous : MessageComposerMove::next),
            shift);
    else if (key == key::home || key == key::end)
        *result = message_composer_move(
            model, key == key::home ? MessageComposerMove::document_home : MessageComposerMove::document_end,
            shift);
    else
        return false;
    return true;
}

// The most message_composer_paste_line reads from the clipboard.
inline constexpr u32 kPasteLineMaximumBytes = 8000;
// The scalars a pasted query refuses: controls below U+0020 and DEL.
inline bool paste_control_scalar(u32 scalar) noexcept {
    return scalar < 0x20 || scalar == 0x7f;
}
// Control+V into a one-line field: the clipboard's text (a clipboard longer than `capacity` bytes pastes nothing).
// Text holding a scalar `forbidden` names is refused whole, or with `keep_prefix` cut before its first one
// (nothing when it opens with one).
__declspec(noinline) inline MessageComposerResult message_composer_paste_line(MessageComposer* model, u32 capacity,
                                                                              bool (*forbidden)(u32 scalar) noexcept,
                                                                              bool keep_prefix) noexcept {
    char text[kPasteLineMaximumBytes];
    const auto read = clipboard_read_utf8({text, minimum(capacity, kPasteLineMaximumBytes)});
    u32 kept = 0;
    while (read.ok() && kept < read.bytes) {
        const auto scalar = utf8_decode_one(text + kept, read.bytes - kept);
        if (!scalar.valid || forbidden(scalar.codepoint))
            break;
        kept += scalar.width;
    }
    return read.ok() && (keep_prefix ? kept != 0 : kept == read.bytes)
               ? message_composer_insert_utf8(model, {text, kept})
               : MessageComposerResult::unchanged;
}
} // namespace maicord::app_detail
