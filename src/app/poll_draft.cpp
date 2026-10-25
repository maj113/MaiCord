#include "maicord/app/poll_draft.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
// The JavaScript String.prototype.trim set, which the client applies.
bool trimmed(u32 c) noexcept {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}
Utf8View trim(Utf8View text) noexcept {
    u32 first = 0, last = text.bytes;
    while (first < last) {
        const auto d = utf8_decode_one(text.data + first, last - first);
        if (!d.valid || !trimmed(d.codepoint)) break;
        first += d.width;
    }
    while (last > first) {
        u32 start = last - 1;
        while (start > first && (static_cast<u8>(text.data[start]) & 0xC0) == 0x80)
            --start;
        const auto d = utf8_decode_one(text.data + start, last - start);
        if (!d.valid || start + d.width != last || !trimmed(d.codepoint)) break;
        last = start;
    }
    return {text.data + first, last - first};
}
u32 codepoints(Utf8View text) noexcept {
    const auto validation = utf8_validate(text);
    return validation.valid ? validation.codepoints : ~0u;
}
// <:name:id> or <a:name:id>.
bool custom_emoji(Utf8View text, u64* id) noexcept {
    if (text.bytes < 6 || text.data[0] != '<' || text.data[text.bytes - 1] != '>')
        return false;
    u32 at = text.data[1] == 'a' ? 2 : 1;
    if (at >= text.bytes || text.data[at] != ':')
        return false;
    ++at;
    const u32 name = at;
    while (at < text.bytes && text.data[at] != ':') {
        const char c = text.data[at];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
        ++at;
    }
    if (at == name || at + 2 >= text.bytes)
        return false;
    u64 value = 0;
    for (++at; at + 1 < text.bytes; ++at) {
        const char c = text.data[at];
        if (c < '0' || c > '9' || value > (~0ull - 9) / 10)
            return false;
        value = value * 10 + static_cast<u64>(c - '0');
    }
    *id = value;
    return value != 0;
}
// Unicode emoji are typed or pasted characters; names like "pizza" are not.
bool unicode_emoji(Utf8View text) noexcept {
    if (!text.bytes || text.bytes > kPollEmojiMaximumBytes || !utf8_validate(text).valid)
        return false;
    bool symbol = false;
    for (u32 i = 0; i < text.bytes; ++i) {
        const u8 c = static_cast<u8>(text.data[i]);
        if (c >= 0x80)
            symbol = true;
        else if (!((c >= '0' && c <= '9') || c == '#' || c == '*'))
            return false;
    }
    return symbol;
}
Utf8View answer(const PollDraft& draft, u32 index) noexcept {
    return trim({draft.answers[index], draft.answer_bytes[index]});
}
Utf8View emoji(const PollDraft& draft, u32 index) noexcept {
    return trim({draft.emoji[index], draft.emoji_bytes[index]});
}
} // namespace

void poll_draft_reset(PollDraft* draft) noexcept {
    if (!draft) return;
    clear_bytes(draft, sizeof(*draft));
    draft->answer_count = kPollCreateMinimumAnswers;
    draft->duration_index = kPollDraftDefaultDuration;
}

bool poll_draft_add_answer(PollDraft* draft) noexcept {
    if (!draft || draft->answer_count >= kPollCreateMaximumAnswers) return false;
    const u32 index = draft->answer_count++;
    draft->answer_bytes[index] = 0;
    draft->emoji_bytes[index] = 0;
    return true;
}

bool poll_draft_remove_answer(PollDraft* draft, u32 index) noexcept {
    if (!draft || index >= draft->answer_count || draft->answer_count <= kPollCreateMinimumAnswers)
        return false;
    for (u32 i = index + 1; i < draft->answer_count; ++i) {
        copy_bytes(draft->answers[i - 1], draft->answers[i], draft->answer_bytes[i]);
        draft->answer_bytes[i - 1] = draft->answer_bytes[i];
        copy_bytes(draft->emoji[i - 1], draft->emoji[i], draft->emoji_bytes[i]);
        draft->emoji_bytes[i - 1] = draft->emoji_bytes[i];
    }
    --draft->answer_count;
    draft->answer_bytes[draft->answer_count] = 0;
    draft->emoji_bytes[draft->answer_count] = 0;
    return true;
}

PollDraftIssue poll_draft_check(const PollDraft& draft) noexcept {
    const auto question = trim({draft.question, draft.question_bytes});
    if (!question.bytes) return PollDraftIssue::question_missing;
    if (codepoints(question) > kPollQuestionMaximumCodepoints) return PollDraftIssue::question_long;
    if (draft.answer_count < kPollCreateMinimumAnswers || draft.answer_count > kPollCreateMaximumAnswers ||
        draft.duration_index >= kPollDurationCount)
        return PollDraftIssue::answers_missing;
    u32 filled = 0;
    for (u32 i = 0; i < draft.answer_count; ++i) {
        const auto text = answer(draft, i), mark = emoji(draft, i);
        if (!text.bytes && !mark.bytes) continue;
        if (!text.bytes) return PollDraftIssue::answer_text_missing;
        if (codepoints(text) > kPollAnswerMaximumCodepoints) return PollDraftIssue::answer_long;
        u64 id = 0;
        if (mark.bytes && !custom_emoji(mark, &id) && !unicode_emoji(mark)) return PollDraftIssue::emoji_invalid;
        ++filled;
    }
    return filled >= kPollCreateMinimumAnswers ? PollDraftIssue::none : PollDraftIssue::answers_missing;
}

Utf8View poll_draft_issue_text(PollDraftIssue issue) noexcept {
    switch (issue) {
    case PollDraftIssue::none: return {};
    case PollDraftIssue::question_missing: return utf8_literal("Add a question.");
    case PollDraftIssue::question_long: return utf8_literal("Questions can have up to 300 characters.");
    case PollDraftIssue::answers_missing: return utf8_literal("Add at least two answers.");
    case PollDraftIssue::answer_text_missing: return utf8_literal("Every answer with an emoji needs text.");
    case PollDraftIssue::answer_long: return utf8_literal("Answers can have up to 55 characters.");
    case PollDraftIssue::emoji_invalid: return utf8_literal("Use an emoji character or <:name:id>.");
    }
    return {};
}

bool poll_draft_build(const PollDraft& draft, PollCreate* poll) noexcept {
    if (!poll || poll_draft_check(draft) != PollDraftIssue::none) return false;
    PollCreate built{};
    built.question = trim({draft.question, draft.question_bytes});
    for (u32 i = 0; i < draft.answer_count; ++i) {
        const auto text = answer(draft, i), mark = emoji(draft, i);
        if (!text.bytes) continue;
        auto& out = built.answers[built.answer_count++];
        out.text = text;
        if (mark.bytes && !custom_emoji(mark, &out.emoji_id))
            out.emoji_name = mark;
    }
    built.duration_hours = kPollDurations[draft.duration_index];
    built.allow_multiselect = draft.allow_multiselect;
    if (!poll_create_valid(built)) return false;
    copy_value(poll, built);
    return true;
}
} // namespace maicord
