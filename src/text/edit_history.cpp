#include "maicord/text/edit_history.hpp"
#include "maicord/platform/secure_buffer.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

namespace maicord {
namespace {
constexpr u32 kHistorySteps = 64;
struct EditRecord { u32 start, removed, inserted, offset, anchor_before, caret_before, anchor_after, caret_after; };
}
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
namespace {
EditRecord& record(TextEditHistory& history, u32 index) noexcept {
    return history.records[(history.head + index) % kHistorySteps];
}
void ring_write(TextEditHistory& history, u32 offset, const u8* input, u32 bytes) noexcept {
    const u32 first = minimum(bytes, history.payload.capacity - offset);
    copy_bytes(history.payload.data + offset, input, first);
    if (bytes > first) copy_bytes(history.payload.data, input + first, bytes - first);
}
void ring_read(const TextEditHistory& history, u32 offset, u8* output, u32 bytes) noexcept {
    const u32 first = minimum(bytes, history.payload.capacity - offset);
    copy_bytes(output, history.payload.data + offset, first);
    if (bytes > first) copy_bytes(output + first, history.payload.data, bytes - first);
}
void erase(TextEditHistory& history, const EditRecord& entry) noexcept {
    const u32 bytes = entry.removed + entry.inserted;
    const u32 first = minimum(bytes, history.payload.capacity - entry.offset);
    SecureZeroMemory(history.payload.data + entry.offset, first);
    if (bytes > first) SecureZeroMemory(history.payload.data, bytes - first);
    history.used -= bytes;
}
TextEditHistoryResult restore(TextEditHistoryState* edit, bool forward) noexcept {
    if (!edit || !edit->storage.data) return TextEditHistoryResult::invalid_input;
    text_edit_break_history_run(edit);

    auto* history = edit->history;
    if (!history || history->grouping || (forward ? history->cursor == history->count : history->cursor == 0))
        return TextEditHistoryResult::unchanged;
    const auto& entry = record(*history, forward ? history->cursor : history->cursor - 1);
    const u32 removed = forward ? entry.removed : entry.inserted;
    const u32 inserted = forward ? entry.inserted : entry.removed;
    const u32 old_bytes = edit->storage.bytes;
    if (entry.start > old_bytes || removed > old_bytes - entry.start ||
        inserted > edit->storage.capacity - (old_bytes - removed)) return TextEditHistoryResult::invalid_input;
    const u32 source = entry.start + removed, destination = entry.start + inserted, tail = old_bytes - source;
    if (destination != source) copy_bytes(edit->storage.data + destination, edit->storage.data + source, tail);
    const u32 offset = (entry.offset + (forward ? entry.removed : 0)) % history->payload.capacity;
    ring_read(*history, offset, edit->storage.data + entry.start, inserted);
    edit->storage.bytes = old_bytes - removed + inserted;
    if (edit->storage.bytes < old_bytes) SecureZeroMemory(edit->storage.data + edit->storage.bytes, old_bytes - edit->storage.bytes);
    edit->anchor = forward ? entry.anchor_after : entry.anchor_before;
    edit->caret = forward ? entry.caret_after : entry.caret_before;

    ++edit->revision;
    if (forward) ++history->cursor; else --history->cursor;
    return TextEditHistoryResult::changed;
}
}
bool text_edit_enable_history(TextEditHistoryState* edit, u32 byte_budget) noexcept {
    if (!edit || !edit->storage.data || !edit->storage.capacity || edit->storage.bytes > edit->storage.capacity ||
        edit->anchor > edit->storage.bytes || edit->caret > edit->storage.bytes || edit->history ||
        byte_budget > 1024 * 1024 || edit->storage.capacity > byte_budget / 2) return false;
    auto* history = static_cast<TextEditHistory*>(VirtualAlloc(nullptr, sizeof(TextEditHistory), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!history) return false;
    if (!secure_buffer_initialize(&history->payload, byte_budget).ok()) { VirtualFree(history, 0, MEM_RELEASE); return false; }
    edit->history = history;
    return true;
}
void text_edit_reset_history(TextEditHistoryState* edit) noexcept {
    auto* history = edit ? edit->history : nullptr;
    if (!history) return;
    secure_buffer_clear(&history->payload);
    secure_buffer_clear(&history->group);
    history->grouping = false;
    history->input = TextEditHistoryInput::isolated;
    SecureZeroMemory(history->records, sizeof(history->records));
    history->head = history->count = history->cursor = history->tail = history->used = 0;
}
void text_edit_history_release(TextEditHistoryState* edit) noexcept {
    auto* history = edit ? edit->history : nullptr;
    if (!history) return;
    edit->history = nullptr;
    secure_buffer_release(&history->payload);
    secure_buffer_release(&history->group);
    SecureZeroMemory(history, sizeof(*history));
    VirtualFree(history, 0, MEM_RELEASE);
}
void text_edit_history_begin(TextEditHistoryState* edit, u32 start, u32 removed, u32 inserted, u32 anchor, u32 caret,
    TextEditHistoryInput input, u64 time_ms) noexcept {
    auto* history = edit->history;
    if (!history || (history->grouping && history->input == TextEditHistoryInput::isolated)) return;
    const auto finish_run = [&]() noexcept {
        // A delete helper may already have selected the scalar to remove.
        // Finish the preceding run at the user's actual pre-operation caret.
        TextEditHistoryState before = *edit;
        before.anchor = anchor; before.caret = caret;
        text_edit_break_history_run(&before);
    };
    if (input != TextEditHistoryInput::isolated && time_ms && anchor == caret) {
        const bool contiguous = history->grouping && history->input == input && history->input_caret == caret &&
            time_ms >= history->input_time_ms && time_ms - history->input_time_ms < 1000;
        if (!contiguous) {
            finish_run();
            if (text_edit_begin_history_group(edit)) {
                history->input = input;
                // Backspace/delete construct a removal selection before this
                // hook; retain the user's original collapsed caret instead.
                history->group_anchor = anchor; history->group_caret = caret;
            }
        }
        if (history->grouping) { history->input_time_ms = time_ms; return; }
        // If optional run scratch cannot be acquired, keep bounded per-splice
        // undo. Text input remains available with the already-owned journal.
    } else finish_run();
    while (history->count > history->cursor) {
        auto& entry = record(*history, --history->count);
        erase(*history, entry); history->tail = entry.offset; entry = {};
    }
    const u32 required = removed + inserted;
    while (history->count && (history->count == kHistorySteps || required > history->payload.capacity - history->used)) {
        auto& first = record(*history, 0);
        erase(*history, first); first = {};
        history->head = (history->head + 1) % kHistorySteps;
        --history->count; --history->cursor;
    }
    auto& entry = record(*history, history->count++);
    entry = {start, removed, inserted, history->tail, anchor, caret, 0, 0};
    ring_write(*history, entry.offset, edit->storage.data + start, removed);
    history->tail = (history->tail + required) % history->payload.capacity;
    history->used += required;
    history->cursor = history->count;
}
void text_edit_history_finish(TextEditHistory* history, const u8* data, u32 anchor, u32 caret) noexcept {
    if (!history) return;
    if (history->grouping) {
        if (history->input != TextEditHistoryInput::isolated) history->input_caret = caret;
        return;
    }
    auto& entry = record(*history, history->count - 1);
    ring_write(*history, (entry.offset + entry.removed) % history->payload.capacity,
        data + entry.start, entry.inserted);
    entry.anchor_after = anchor; entry.caret_after = caret;
}
bool text_edit_begin_history_group(TextEditHistoryState* edit) noexcept {
    if (!edit || !edit->storage.data) return false;
    text_edit_break_history_run(edit);
    auto* history = edit->history;
    if (!history) return true;
    if (history->grouping) return false;
    if (!history->group.data && !secure_buffer_initialize(&history->group, edit->storage.capacity).ok()) return false;
    if (!secure_buffer_assign(&history->group, ConstSlice<u8>{edit->storage.data, edit->storage.bytes}).ok()) return false;
    history->group_anchor = edit->anchor; history->group_caret = edit->caret;
    history->grouping = true;
    return true;
}
void text_edit_end_history_group(TextEditHistoryState* edit) noexcept {
    auto* history = edit ? edit->history : nullptr;
    if (!history || !history->grouping) return;
    history->grouping = false;
    history->input = TextEditHistoryInput::isolated;
    const auto& before = history->group;
    const auto& after = edit->storage;
    u32 first = 0;
    while (first < before.bytes && first < after.bytes && before.data[first] == after.data[first]) ++first;
    if (first != before.bytes || first != after.bytes) {
        // A bytewise common prefix/suffix may end within a UTF-8 scalar.
        while (first && first < before.bytes && (before.data[first] & 0xC0u) == 0x80u) --first;
        u32 old_end = before.bytes, new_end = after.bytes;
        while (old_end > first && new_end > first && before.data[old_end - 1] == after.data[new_end - 1]) { --old_end; --new_end; }
        while (old_end < before.bytes && (before.data[old_end] & 0xC0u) == 0x80u) { ++old_end; ++new_end; }
        // Reserve through the normal eviction path, reading the removed range
        // from a temporary non-owning projection of the validated old document.
        TextEditHistoryState projection = *edit;
        projection.storage = {before.data, before.capacity, before.bytes};
        text_edit_history_begin(&projection, first, old_end - first, new_end - first,
            history->group_anchor, history->group_caret);
        text_edit_history_finish(edit);
    }
    secure_buffer_clear(&history->group);
}
void text_edit_break_history_run(TextEditHistoryState* edit) noexcept {
    if (edit && edit->history && edit->history->input != TextEditHistoryInput::isolated)
        text_edit_end_history_group(edit);
}
bool text_edit_can_undo(const TextEditHistoryState& edit) noexcept {
    return edit.history && (edit.history->input != TextEditHistoryInput::isolated ||
        (!edit.history->grouping && edit.history->cursor));
}
bool text_edit_can_redo(const TextEditHistoryState& edit) noexcept { return edit.history && !edit.history->grouping && edit.history->cursor < edit.history->count; }
TextEditHistoryResult text_edit_undo(TextEditHistoryState* edit) noexcept { return restore(edit, false); }
TextEditHistoryResult text_edit_redo(TextEditHistoryState* edit) noexcept { return restore(edit, true); }
TextEditHistoryCosts text_edit_history_costs(const TextEditHistoryState& edit) noexcept {
    const auto* history = edit.history;
    return history ? TextEditHistoryCosts{sizeof(*history), history->payload.capacity, history->used,
        history->cursor, history->count - history->cursor, history->group.capacity} : TextEditHistoryCosts{};
}
}

