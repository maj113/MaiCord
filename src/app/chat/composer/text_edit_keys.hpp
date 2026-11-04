#pragma once
#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/model/chat/message_composer.hpp"

namespace maicord::app_detail {
// Select all, undo/redo, deletion and caret movement keys of a text field.
// False, with *result untouched, for any other key; clipboard keys stay with the caller.
__declspec(noinline) inline bool message_composer_edit_key(MessageComposer* model, u32 key, bool control,
                                                           bool shift,
                                                           MessageComposerResult* result) noexcept {
    if (control && key == 'A')
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
} // namespace maicord::app_detail
