#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
struct TextEditHistory;
// Borrowed, validated UTF-8 document. No ownership or lock claim is made for
// its storage. Only the history's private payload/snapshot allocations are owned.
struct TextEditBuffer { u8* data; u32 capacity, bytes; };
struct TextEditHistoryState {
    TextEditBuffer storage;
    u32 anchor, caret, revision;
    TextEditHistory* history;
};
enum class TextEditHistoryResult : u8 { unchanged, changed, invalid_input };
enum class TextEditHistoryInput : u8 { isolated, typing, backspace, delete_forward };
struct TextEditHistoryCosts { u32 owner_bytes, payload_bytes, used_bytes, undo_steps, redo_steps, group_bytes; };
bool text_edit_enable_history(TextEditHistoryState* state, u32 byte_budget) noexcept;
void text_edit_reset_history(TextEditHistoryState* state) noexcept;
void text_edit_history_release(TextEditHistoryState* state) noexcept;
// Call after validation, before a non-failing splice, then finish immediately
// afterward. No callbacks or unrelated mutations may interleave the two calls.
void text_edit_history_begin(TextEditHistoryState* state, u32 start, u32 removed, u32 inserted,
    u32 anchor, u32 caret, TextEditHistoryInput input = TextEditHistoryInput::isolated, u64 time_ms = 0) noexcept;
void text_edit_history_finish(TextEditHistory* history, const u8* data, u32 anchor, u32 caret) noexcept;
inline void text_edit_history_finish(TextEditHistoryState* state) noexcept {
    if (state) text_edit_history_finish(state->history, state->storage.data, state->anchor, state->caret);
}
bool text_edit_begin_history_group(TextEditHistoryState* state) noexcept;
void text_edit_end_history_group(TextEditHistoryState* state) noexcept;
void text_edit_break_history_run(TextEditHistoryState* state) noexcept;
bool text_edit_can_undo(const TextEditHistoryState& state) noexcept;
bool text_edit_can_redo(const TextEditHistoryState& state) noexcept;
TextEditHistoryResult text_edit_undo(TextEditHistoryState* state) noexcept;
TextEditHistoryResult text_edit_redo(TextEditHistoryState* state) noexcept;
TextEditHistoryCosts text_edit_history_costs(const TextEditHistoryState& state) noexcept;
}
