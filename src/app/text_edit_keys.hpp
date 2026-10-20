#pragma once
#include "maicord/core/win32_user.hpp"
#include "maicord/model/message_composer.hpp"

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
    else if (key == VK_BACK)
        *result = control ? message_composer_delete_word(model, false) : message_composer_backspace(model);
    else if (key == VK_DELETE)
        *result = control ? message_composer_delete_word(model, true)
                          : message_composer_delete_forward(model);
    else if (key == VK_LEFT || key == VK_RIGHT)
        *result = message_composer_move(
            model,
            control ? (key == VK_LEFT ? MessageComposerMove::previous_word : MessageComposerMove::next_word)
                    : (key == VK_LEFT ? MessageComposerMove::previous : MessageComposerMove::next),
            shift);
    else if (key == VK_HOME || key == VK_END)
        *result = message_composer_move(
            model, key == VK_HOME ? MessageComposerMove::document_home : MessageComposerMove::document_end,
            shift);
    else
        return false;
    return true;
}
} // namespace maicord::app_detail
