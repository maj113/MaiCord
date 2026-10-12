#include "maicord/protocol/message_create.hpp"
#include "maicord/app/poll_draft.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool contains(Utf8View text, Utf8View expected) noexcept {
    for (u32 i = 0; i + expected.bytes <= text.bytes; ++i)
        if (bytes_equal(text.data + i, expected.data, expected.bytes)) return true;
    return false;
}
}

void test_message_create_stickers(void (*check)(bool, const char*) noexcept) noexcept {
    u8 body[1024]{};
    const u64 ids[]{123, 456, 18446744073709551615ull};
    const auto only = write_message_create_body({}, 99, 0, 0, {}, {ids, 3}, {body, sizeof(body)});
    const Utf8View output{reinterpret_cast<const char*>(body), only.bytes};
    check(only.ok() && contains(output, utf8_literal(R"("content":"")")) &&
        contains(output, utf8_literal(R"("sticker_ids":["123","456","18446744073709551615"])")) &&
        contains(output, utf8_literal(R"("nonce":"99")")) &&
        contains(output, utf8_literal(R"("enforce_nonce":true)")),
        "sticker-only message body has empty content, exact snowflakes, and deduplicated nonce");
    UploadedAttachment file{utf8_literal("file.txt"), utf8_literal("upload/file.txt"), {}, 0, true};
    const auto combined = write_message_create_body(utf8_literal("reply"), 100, 7, 8,
        {&file, 1}, {ids, 1}, {body, sizeof(body)});
    const Utf8View combined_output{reinterpret_cast<const char*>(body), combined.bytes};
    check(combined.ok() && contains(combined_output, utf8_literal(R"("message_reference":)")) &&
        contains(combined_output, utf8_literal(R"("attachments":[)")) &&
        contains(combined_output, utf8_literal(R"("sticker_ids":["123"])")) &&
        contains(combined_output, utf8_literal(R"("replied_user":true)")),
        "sticker message supports reply, content, and uploaded attachments together");
    const auto muted = write_message_create_body(utf8_literal("<@81> <@&82> @everyone"), 101, 7, 8,
        {&file, 1}, {ids, 1}, {body, sizeof(body)}, false);
    const Utf8View muted_output{reinterpret_cast<const char*>(body), muted.bytes};
    check(muted.ok() && contains(muted_output, utf8_literal(R"("replied_user":false)")) &&
        !contains(muted_output, utf8_literal(R"("replied_user":true)")) &&
        contains(muted_output, utf8_literal(R"("message_id":"8")")) &&
        contains(muted_output, utf8_literal(R"("parse":["users","roles","everyone"])")) &&
        contains(muted_output, utf8_literal(R"("content":"<@81> <@&82> @everyone")")) &&
        contains(muted_output, utf8_literal(R"("attachments":[)")) &&
        contains(muted_output, utf8_literal(R"("sticker_ids":["123"])")),
        "reply author mention can be disabled without suppressing explicit mentions attachments or stickers");
    const auto muted_text = write_message_create_body(utf8_literal("reply"), 102, 7, 8,
        {body, sizeof(body)}, false);
    check(muted_text.ok() && contains({reinterpret_cast<const char*>(body), muted_text.bytes},
        utf8_literal(R"("replied_user":false)")), "reply-only writer overload carries explicit author mention policy");
    const auto muted_attachment = write_message_create_body({}, 103, 7, 8,
        {&file, 1}, {body, sizeof(body)}, false);
    check(muted_attachment.ok() && contains({reinterpret_cast<const char*>(body), muted_attachment.bytes},
        utf8_literal(R"("replied_user":false)")), "attachment-only reply writer carries explicit author mention policy");
    const u64 invalid[]{0, 123, 123, 456};
    body[0] = 0xa5;
    check(write_message_create_body({}, 99, 0, 0, {}, {invalid, 1}, {body, sizeof(body)}).error ==
            MessageCreateWriteError::invalid_input && body[0] == 0xa5 &&
        write_message_create_body({}, 99, 0, 0, {}, {invalid + 1, 2}, {body, sizeof(body)}).error ==
            MessageCreateWriteError::invalid_input &&
        write_message_create_body({}, 99, 0, 0, {}, {invalid, 4}, {body, sizeof(body)}).error ==
            MessageCreateWriteError::invalid_input &&
        write_message_create_body({}, 99, 0, 0, {}, {nullptr, 1}, {body, sizeof(body)}).error ==
            MessageCreateWriteError::invalid_input,
        "sticker message rejects zero, duplicate, excessive, and missing IDs before writing");
    check(write_message_create_body({}, 99, 0, 0, {}, {}, {body, sizeof(body)}).error ==
        MessageCreateWriteError::invalid_input &&
        write_message_create_body({}, 99, 0, 0, {}, {ids, 1}, {body, 4}).error ==
        MessageCreateWriteError::output_too_small,
        "sticker message preserves empty-payload and output capacity validation");
    u8 legacy[1024]{}, explicit_empty[1024]{};
    const auto a = write_message_create_body(utf8_literal("text"), 1234, 0, 0, {}, {legacy, sizeof(legacy)});
    const auto b = write_message_create_body(utf8_literal("text"), 1234, 0, 0, {}, {}, {explicit_empty, sizeof(explicit_empty)});
    check(a.ok() && b.ok() && a.bytes == b.bytes && bytes_equal(legacy, explicit_empty, a.bytes),
        "legacy message body overload delegates without changing text sends");
    const auto ordinary = write_message_create_body(utf8_literal("text"), 1234, 0, 0, {}, {},
        {explicit_empty, sizeof(explicit_empty)}, false);
    check(ordinary.ok() && ordinary.bytes == a.bytes && bytes_equal(legacy, explicit_empty, a.bytes) &&
        !contains({reinterpret_cast<const char*>(explicit_empty), ordinary.bytes}, utf8_literal("replied_user")) &&
        !contains({reinterpret_cast<const char*>(explicit_empty), ordinary.bytes}, utf8_literal("message_reference")),
        "ordinary send wire bytes ignore reply-only mention policy and never synthesize reply authority");
    const MessageForwardReference private_source{18446744073709551615ull, 18446744073709551614ull, 0};
    const auto forward = write_message_forward_body({}, 99, private_source, {body, sizeof(body)});
    const auto forward_text = Utf8View{reinterpret_cast<const char*>(body), forward.bytes};
    check(forward.ok() && contains(forward_text, utf8_literal(R"("content":"")")) &&
        contains(forward_text, utf8_literal(R"("message_reference":{"type":1,"message_id":"18446744073709551615","channel_id":"18446744073709551614","fail_if_not_exists":true})")) &&
        contains(forward_text, utf8_literal(R"("allowed_mentions":{"parse":[],"replied_user":false})")) &&
        contains(forward_text, utf8_literal(R"("enforce_nonce":true)")) &&
        !contains(forward_text, utf8_literal("guild_id")) &&
        !contains(forward_text, utf8_literal("message_snapshots")) && !contains(forward_text, utf8_literal("attachments")),
        "private forward admits empty caption and exact origin IDs without client snapshots or mention authority");
    const auto captioned = write_message_forward_body(utf8_literal("a\n\"b\\c <@81> @everyone"),
        100, {8, 7, 6}, {body, sizeof(body)});
    check(captioned.ok() && contains({reinterpret_cast<const char*>(body), captioned.bytes},
        utf8_literal(R"("content":"a\n\"b\\c <@81> @everyone")")) &&
        contains({reinterpret_cast<const char*>(body), captioned.bytes}, utf8_literal(R"("guild_id":"6")")),
        "guild forward escapes caption and attributes the origin guild");
    body[0] = 0xa5;
    const char malformed[]{'\xc3', '('};
    check(write_message_forward_body({}, 1, {0, 7, 6}, {body, sizeof(body)}).error ==
            MessageCreateWriteError::invalid_input && body[0] == 0xa5 &&
        write_message_forward_body({}, 1, {8, 0, 6}, {body, sizeof(body)}).error ==
            MessageCreateWriteError::invalid_input &&
        write_message_forward_body({}, 0, {8, 7, 6}, {body, sizeof(body)}).error ==
            MessageCreateWriteError::invalid_input &&
        write_message_forward_body({nullptr, 1}, 1, {8, 7, 6}, {body, sizeof(body)}).error ==
            MessageCreateWriteError::invalid_input &&
        write_message_forward_body({malformed, 2}, 1, {8, 7, 6}, {body, sizeof(body)}).error ==
            MessageCreateWriteError::invalid_utf8 && body[0] == 0xa5,
        "forward writer rejects absent origin nonce caption storage and invalid UTF8 before writing");
    u8 guarded[1026]{};
    guarded[0] = guarded[1025] = 0xa5;
    const auto capacity = write_message_forward_body({}, 99, private_source, {guarded + 1, 1024});
    const auto exact = write_message_forward_body({}, 99, private_source, {guarded + 1, capacity.bytes});
    const auto short_output = write_message_forward_body({}, 99, private_source, {guarded + 1, capacity.bytes - 1});
    check(capacity.ok() && exact.ok() && exact.bytes == capacity.bytes &&
        short_output.error == MessageCreateWriteError::output_too_small && guarded[0] == 0xa5 && guarded[1025] == 0xa5,
        "forward writer admits exact output capacity and rejects one byte shortage without overflow");
}

void test_poll_draft(void (*check)(bool, const char*) noexcept) noexcept {
    static PollDraft draft;
    poll_draft_reset(&draft);
    auto set = [](char* storage, u16* bytes, Utf8View text) noexcept {
        copy_bytes(storage, text.data, text.bytes);
        *bytes = static_cast<u16>(text.bytes);
    };
    auto set_emoji = [](char* storage, u8* bytes, Utf8View text) noexcept {
        copy_bytes(storage, text.data, text.bytes);
        *bytes = static_cast<u8>(text.bytes);
    };
    check(draft.answer_count == 2 && draft.duration_index == kPollDraftDefaultDuration &&
              kPollDurations[draft.duration_index] == 24 && !draft.allow_multiselect &&
              poll_draft_check(draft) == PollDraftIssue::question_missing,
          "poll draft: two empty answers, 24 hours and one choice by default; a question is required");
    set(draft.question, &draft.question_bytes, utf8_literal("  Lunch?\t"));
    set(draft.answers[0], &draft.answer_bytes[0], utf8_literal(" Pizza "));
    check(poll_draft_check(draft) == PollDraftIssue::answers_missing,
          "poll draft: one filled answer is not enough");
    set_emoji(draft.emoji[1], &draft.emoji_bytes[1], utf8_literal("🌮"));
    check(poll_draft_check(draft) == PollDraftIssue::answer_text_missing,
          "poll draft: an emoji without answer text is rejected");
    set(draft.answers[1], &draft.answer_bytes[1], utf8_literal("Tacos"));
    set_emoji(draft.emoji[0], &draft.emoji_bytes[0], utf8_literal("pizza"));
    check(poll_draft_check(draft) == PollDraftIssue::emoji_invalid &&
              !utf8_equal(poll_draft_issue_text(PollDraftIssue::emoji_invalid), {}),
          "poll draft: emoji names are not emoji");
    set_emoji(draft.emoji[0], &draft.emoji_bytes[0], utf8_literal("<:pizza:123>"));
    bool added = true;
    for (u32 i = 2; i < kPollCreateMaximumAnswers; ++i)
        added = added && poll_draft_add_answer(&draft);
    draft.duration_index = 5;
    draft.allow_multiselect = true;
    PollCreate poll{};
    check(added && !poll_draft_add_answer(&draft) && draft.answer_count == 10 && poll_draft_build(draft, &poll) &&
              utf8_equal(poll.question, utf8_literal("Lunch?")) && poll.answer_count == 2 &&
              utf8_equal(poll.answers[0].text, utf8_literal("Pizza")) && poll.answers[0].emoji_id == 123 &&
              !poll.answers[0].emoji_name.bytes && utf8_equal(poll.answers[1].emoji_name, utf8_literal("🌮")) &&
              poll.duration_hours == 168 && poll.allow_multiselect,
          "poll draft: ten rows at most; blank rows are skipped, text trimmed and custom emoji parsed");
    check(poll_draft_remove_answer(&draft, 0) && draft.answer_count == 9 &&
              utf8_equal({draft.answers[0], draft.answer_bytes[0]}, utf8_literal("Tacos")) &&
              poll_draft_check(draft) == PollDraftIssue::answers_missing,
          "poll draft: removing a row moves later rows up");
    while (draft.answer_count > 2)
        poll_draft_remove_answer(&draft, draft.answer_count - 1);
    check(!poll_draft_remove_answer(&draft, 0) && draft.answer_count == 2, "poll draft: two rows always remain");
    char long_answer[kPollAnswerMaximumCodepoints + 1];
    for (auto& c : long_answer)
        c = 'x';
    set(draft.answers[1], &draft.answer_bytes[1], {long_answer, sizeof(long_answer)});
    check(poll_draft_check(draft) == PollDraftIssue::answer_long, "poll draft: answers keep the 55 character limit");
    set(draft.answers[1], &draft.answer_bytes[1], utf8_literal("1"));
    set_emoji(draft.emoji[1], &draft.emoji_bytes[1], utf8_literal("1\xEF\xB8\x8F\xE2\x83\xA3"));
    u8 body[4096]{};
    const bool built = poll_draft_build(draft, &poll);
    const auto written = built ? write_message_poll_body(poll, 55, {body, sizeof(body)})
                               : MessageCreateWriteResult{};
    const Utf8View output{reinterpret_cast<const char*>(body), written.bytes};
    check(written.ok() && contains(output, utf8_literal(R"({"content":"","nonce":"55","tts":false,"flags":0,"poll":)")) &&
              contains(output, utf8_literal(R"("answers":[{"poll_media":{"text":"Tacos","emoji":{"name":"🌮"}}},{"poll_media":{"text":"1",)")) &&
              contains(output, utf8_literal(R"("allow_multiselect":true,"duration":168,"layout_type":1})")),
          "poll draft: keycap emoji are accepted and the body carries the poll object");
    check(write_message_poll_body(poll, 0, {body, sizeof(body)}).error ==
                  MessageCreateWriteError::invalid_input &&
              write_message_poll_body(poll, 55, {body, 40}).error ==
                  MessageCreateWriteError::output_too_small,
          "poll body: nonce is required and short output is reported");
}
} // namespace maicord
