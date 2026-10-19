#pragma once

#include "maicord/core/arena.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/edit_history.hpp"
#include "maicord/model/input_target.hpp"

namespace maicord {

inline constexpr u32 kMessageComposerMaximumBytes = 16 * 1024;
inline constexpr u32 kMessageComposerDefaultMaximumCodepoints = 2'000;
inline constexpr u32 kMessageComposerPremiumMaximumCodepoints = 4'000;
static_assert(kMessageComposerPremiumMaximumCodepoints * 4 <= kMessageComposerMaximumBytes);
// Only full Nitro (premium_type 2) raises the message length.
constexpr u32 message_composer_maximum_codepoints(u8 premium_type) noexcept {
    return premium_type == 2 ? kMessageComposerPremiumMaximumCodepoints : kMessageComposerDefaultMaximumCodepoints;
}
inline constexpr float kMessageComposerMinimumHeight = 48.0f;
inline constexpr float kMessageComposerMaximumHeight = 208.0f;

enum class MessageComposerPhase : u8 {
    idle,
    queued,
    sending,
    retry_wait,
    failed,
};

enum class MessageComposerResult : u8 {
    unchanged,
    changed,
    capacity,
    invalid_input,
    invalid_utf8,
};

enum class MessageComposerMove : u8 {
    previous,
    next,
    line_home,
    line_end,
    document_home,
    document_end,
    previous_word,
    next_word,
};

struct MessageComposerAtomicSpan { u32 first, last; };
struct MessageComposer {
    ConstSlice<MessageComposerAtomicSpan> atomic_spans;
    u32 atomic_revision;
    Arena owned_storage;
    char* text;
    u32 capacity;
    u32 bytes;
    u32 anchor;
    u32 caret;
    u32 revision;
    u32 text_revision;
    u64 document_identity;
    u64 channel_id;
    u64 nonce;
    u64 retry_at_ms;
    u16 http_status;
    u16 pending_high_surrogate;
    MessageComposerPhase phase;
    bool focused;
    bool owns_storage;
    TextEditHistory* history;
};

bool message_composer_initialize(MessageComposer* composer,
                                 Slice<char> storage) noexcept;
bool message_composer_initialize_owned(
    MessageComposer* composer,
    u32 capacity = kMessageComposerMaximumBytes) noexcept;
void message_composer_release(MessageComposer* composer) noexcept;
// Owned composers create this lazily on first edit. Borrowed composers opt in.
bool message_composer_enable_history(MessageComposer* composer, u32 byte_budget = 64 * 1024) noexcept;
void message_composer_break_history_run(MessageComposer* composer) noexcept;
MessageComposerResult message_composer_undo(MessageComposer* composer) noexcept;
MessageComposerResult message_composer_redo(MessageComposer* composer) noexcept;
TextEditHistoryCosts message_composer_history_costs(const MessageComposer& composer) noexcept;
TextInputTarget message_composer_input_target(MessageComposer* composer) noexcept;
void message_composer_reset(MessageComposer* composer, u64 channel_id,
                            bool retain_draft) noexcept;
// A zero channel is valid for unsent profile/search documents; send validation
// still requires a resolved destination channel. Restores reset history/identity.
bool message_composer_restore(MessageComposer* composer, u64 channel_id,
                              Utf8View text, u32 anchor,
                              u32 caret) noexcept;
// After an initialized borrowed buffer is overwritten externally, validate and
// adopt its byte length without copying/clearing it. Retires history/identity,
// clears pending input and clamps selection to valid UTF-8 byte boundaries.
bool message_composer_refresh_borrowed(MessageComposer* composer, u32 bytes) noexcept;
Utf8View message_composer_text(const MessageComposer& composer) noexcept;
bool message_composer_can_send(const MessageComposer& composer,
                               bool channel_allows_send,
                               u32 maximum_codepoints =
                                   kMessageComposerDefaultMaximumCodepoints,
                               bool has_attachments = false) noexcept;
u32 message_composer_codepoints(const MessageComposer& composer) noexcept;
MessageComposerResult message_composer_insert_scalar(
    MessageComposer* composer, u32 codepoint, u64 input_time_ms = 0) noexcept;
MessageComposerResult message_composer_insert_utf16(
    MessageComposer* composer, u16 code_unit, u64 input_time_ms = 0) noexcept;
MessageComposerResult message_composer_insert_utf8(
    MessageComposer* composer, Utf8View text) noexcept;
MessageComposerResult message_composer_replace_range(
    MessageComposer* composer, u32 begin, u32 end, Utf8View text) noexcept;
MessageComposerResult message_composer_insert_line_break(
    MessageComposer* composer) noexcept;
MessageComposerResult message_composer_backspace(
    MessageComposer* composer, u64 input_time_ms = 0) noexcept;
MessageComposerResult message_composer_delete_forward(
    MessageComposer* composer, u64 input_time_ms = 0) noexcept;
MessageComposerResult message_composer_delete_word(MessageComposer* composer, bool forward, u64 input_time_ms = 0) noexcept;
MessageComposerResult message_composer_move(MessageComposer* composer,
                                            MessageComposerMove movement,
                                            bool extend_selection) noexcept;
MessageComposerResult message_composer_set_selection(
    MessageComposer* composer, u32 anchor, u32 caret) noexcept;
MessageComposerResult message_composer_select_word(
    MessageComposer* composer, u32 position) noexcept;
// Pointer word/line selections: widens both ends to whole atomic tokens.
MessageComposerResult message_composer_select_range(
    MessageComposer* composer, u32 anchor, u32 caret) noexcept;
MessageComposerResult message_composer_select_all(
    MessageComposer* composer) noexcept;
MessageComposerResult message_composer_clear(MessageComposer* composer) noexcept;
float message_composer_preferred_height(const MessageComposer& composer,
                                        float available_text_width) noexcept;

static_assert(sizeof(MessageComposer) <= 176);

} // namespace maicord
