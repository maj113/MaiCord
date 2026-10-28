#include "secure_edit_history.hpp"
namespace maicord {
namespace {
inline TextEditHistoryState project(const SecureEdit& edit) noexcept {
    return {{edit.storage.data, edit.storage.capacity, edit.storage.bytes}, edit.anchor, edit.caret, edit.revision, edit.history};
}
bool valid(const SecureEdit* edit) noexcept { return edit && edit->storage.data && edit->storage.locked && !edit->password; }
SecureEditResult replay(SecureEdit* edit, bool forward) noexcept {
    if (!valid(edit)) return SecureEditResult::invalid_input;
    edit->pending_high_surrogate = 0;
    auto state = project(*edit);
    const auto result = forward ? text_edit_redo(&state) : text_edit_undo(&state);
    edit->storage.bytes = state.storage.bytes; edit->anchor = state.anchor; edit->caret = state.caret; edit->revision = state.revision;
    return result == TextEditHistoryResult::changed ? SecureEditResult::changed :
        result == TextEditHistoryResult::unchanged ? SecureEditResult::unchanged : SecureEditResult::invalid_input;
}
}
bool secure_edit_enable_history(SecureEdit* edit, u32 budget) noexcept {
    if (!valid(edit)) return false;
    auto state = project(*edit); const bool result = text_edit_enable_history(&state, budget); edit->history = state.history; return result;
}
void secure_edit_reset_history(SecureEdit* edit) noexcept {
    if (!edit) return; auto state = project(*edit); text_edit_reset_history(&state);
}
void secure_edit_history_release(SecureEdit* edit) noexcept {
    auto state = project(*edit); text_edit_history_release(&state); edit->history = nullptr;
}
bool secure_edit_begin_history_group(SecureEdit* edit) noexcept {
    if (!valid(edit)) return false; auto state = project(*edit); return text_edit_begin_history_group(&state);
}
void secure_edit_end_history_group(SecureEdit* edit) noexcept {
    if (!edit || !edit->history) return; auto state = project(*edit); text_edit_end_history_group(&state);
}
void secure_edit_break_history_run(SecureEdit* edit) noexcept {
    if (!edit || !edit->history) return; auto state = project(*edit); text_edit_break_history_run(&state);
}
bool secure_edit_can_undo(const SecureEdit& edit) noexcept { return text_edit_can_undo(project(edit)); }
bool secure_edit_can_redo(const SecureEdit& edit) noexcept { return text_edit_can_redo(project(edit)); }
SecureEditResult secure_edit_undo(SecureEdit* edit) noexcept { return replay(edit, false); }
SecureEditResult secure_edit_redo(SecureEdit* edit) noexcept { return replay(edit, true); }
SecureEditHistoryCosts secure_edit_history_costs(const SecureEdit& edit) noexcept { return text_edit_history_costs(project(edit)); }
}
