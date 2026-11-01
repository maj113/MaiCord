#pragma once
// Undo/redo availability and history costs of a secure edit, as the history tests ask for them. The editor
// itself replays history directly, so these queries are not part of the shipping library.
#include "../../src/model/chat/edit_history_state.hpp"
#include "maicord/ui/text/secure_edit.hpp"

namespace maicord {
using SecureEditHistoryCosts = TextEditHistoryCosts;
inline bool text_edit_can_undo(const TextEditHistoryState& edit) noexcept {
    return edit.history && (edit.history->input != TextEditHistoryInput::isolated ||
                            (!edit.history->grouping && edit.history->cursor));
}
inline bool text_edit_can_redo(const TextEditHistoryState& edit) noexcept {
    return edit.history && !edit.history->grouping && edit.history->cursor < edit.history->count;
}
inline TextEditHistoryState secure_edit_history_state(const SecureEdit& edit) noexcept {
    return {{edit.storage.data, edit.storage.capacity, edit.storage.bytes}, edit.anchor, edit.caret, edit.revision, edit.history};
}
inline bool secure_edit_can_undo(const SecureEdit& edit) noexcept { return text_edit_can_undo(secure_edit_history_state(edit)); }
inline bool secure_edit_can_redo(const SecureEdit& edit) noexcept { return text_edit_can_redo(secure_edit_history_state(edit)); }
inline SecureEditHistoryCosts secure_edit_history_costs(const SecureEdit& edit) noexcept {
    return text_edit_history_costs(secure_edit_history_state(edit));
}
} // namespace maicord
