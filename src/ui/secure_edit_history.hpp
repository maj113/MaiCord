#pragma once
#include "maicord/ui/secure_edit.hpp"

namespace maicord {
using SecureEditHistoryInput = TextEditHistoryInput;
// Internal mutation hooks. Begin only after validation, before touching text;
// finish immediately after the non-failing splice. No callbacks in between.
inline void secure_edit_history_begin(SecureEdit* edit, u32 start, u32 removed, u32 inserted,
    u32 anchor, u32 caret, SecureEditHistoryInput input = SecureEditHistoryInput::isolated, u64 time_ms = 0) noexcept {
    if (!edit->history) return;
    TextEditHistoryState state{{edit->storage.data, edit->storage.capacity, edit->storage.bytes},
        edit->anchor, edit->caret, edit->revision, edit->history};
    text_edit_history_begin(&state, start, removed, inserted, anchor, caret, input, time_ms);
}
inline void secure_edit_history_finish(SecureEdit* edit) noexcept {
    if (edit->history) text_edit_history_finish(edit->history, edit->storage.data, edit->anchor, edit->caret);
}
void secure_edit_history_release(SecureEdit* edit) noexcept;
}
