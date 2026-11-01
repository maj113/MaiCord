#pragma once
// The undo/redo history behind TextEditHistoryState::history; the history tests read its step cursors.
#include "maicord/model/chat/edit_history.hpp"
#include "maicord/platform/crypto/secure_buffer.hpp"

namespace maicord {
inline constexpr u32 kHistorySteps = 64;
struct EditRecord { u32 start, removed, inserted, offset, anchor_before, caret_before, anchor_after, caret_after; };
struct TextEditHistory {
    SecureBuffer payload;
    EditRecord records[kHistorySteps];
    u32 head, count, cursor, tail, used;
    SecureBuffer group;
    u32 group_anchor, group_caret;
    bool grouping;
    TextEditHistoryInput input;
    u32 input_caret;
    u64 input_time_ms;
};
} // namespace maicord
