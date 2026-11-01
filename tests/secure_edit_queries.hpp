#pragma once
// Undo/redo availability of a secure edit, as the history tests ask for it. The editor itself replays
// history directly, so these queries are not part of the shipping library.
#include "maicord/model/edit_history.hpp"
#include "maicord/ui/secure_edit.hpp"

namespace maicord {
inline TextEditHistoryState secure_edit_history_state(const SecureEdit& edit) noexcept {
    return {{edit.storage.data, edit.storage.capacity, edit.storage.bytes}, edit.anchor, edit.caret, edit.revision, edit.history};
}
inline bool secure_edit_can_undo(const SecureEdit& edit) noexcept { return text_edit_can_undo(secure_edit_history_state(edit)); }
inline bool secure_edit_can_redo(const SecureEdit& edit) noexcept { return text_edit_can_redo(secure_edit_history_state(edit)); }
} // namespace maicord
