#pragma once

#include "maicord/protocol/message_create.hpp"

namespace maicord {
// Editable poll creation form. Text fields hold raw input; build trims them.
inline constexpr u32 kPollDraftQuestionBytes = kPollQuestionMaximumCodepoints * 4;
inline constexpr u32 kPollDraftAnswerBytes = kPollAnswerMaximumCodepoints * 4;
inline constexpr u32 kPollDraftEmojiBytes = kPollEmojiMaximumBytes;
inline constexpr u32 kPollDraftDefaultDuration = 3; // 24 hours
struct PollDraft {
    char question[kPollDraftQuestionBytes];
    char answers[kPollCreateMaximumAnswers][kPollDraftAnswerBytes];
    char emoji[kPollCreateMaximumAnswers][kPollDraftEmojiBytes];
    u16 question_bytes;
    u16 answer_bytes[kPollCreateMaximumAnswers];
    u8 emoji_bytes[kPollCreateMaximumAnswers];
    u32 answer_count; // visible answer rows, 2..10
    u8 duration_index;
    bool allow_multiselect;
};
enum class PollDraftIssue : u8 {
    none,
    question_missing,
    question_long,
    answers_missing,
    answer_text_missing,
    answer_long,
    emoji_invalid,
};

void poll_draft_reset(PollDraft*) noexcept;
bool poll_draft_add_answer(PollDraft*) noexcept;
// Keeps at least two rows; later rows move up.
bool poll_draft_remove_answer(PollDraft*, u32 index) noexcept;
// Rows that are entirely blank are skipped; at least two must remain.
PollDraftIssue poll_draft_check(const PollDraft&) noexcept;
Utf8View poll_draft_issue_text(PollDraftIssue) noexcept;
// Borrows the draft's text. A custom emoji is written as <:name:id> or
// <a:name:id>; anything else is sent as a Unicode emoji name.
bool poll_draft_build(const PollDraft&, PollCreate*) noexcept;
} // namespace maicord
