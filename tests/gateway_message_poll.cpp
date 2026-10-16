#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/message_history.hpp"
#include "maicord/protocol/message_search.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/app/gateway_client.hpp"
#include "../src/app/gateway_dispatch_common.hpp"

namespace maicord {
namespace {
struct PollFixture {
    u32 structurals[2048], message_structurals[2048];
    char text[8192], mailbox[8192];
    u8 generated[16384];
    GatewayMessagePollAnswer answers[20];
    GatewayMessagePollAnswerCount counts[20];
    GatewayMessageEvent messages[3];
    GatewayMessageReaction reactions[20];
    GatewayMessageAttachment attachments[10], child_attachments[10];
    GatewayMessageEmbed embeds[10], child_embeds[10];
    GatewayMessageEmbedField fields[64], child_fields[64];
    GatewayMessageSticker stickers[3], child_stickers[3];
    GatewayMessageComponent components[40], child_components[40];
    GatewayMessageNamedUser users[100], child_users[100];
    u64 roles[100], member_roles[100];
    MessageSearchGroup groups[2];
    MessageSearchMessage search_messages[2];
    GatewayMessagePoll output;
    u32 used, sink_calls;
    char sink_questions[2][32];
    u32 sink_answer_ids[2];
} f{};
constexpr auto complete_poll = utf8_literal(
    R"({"question":{"text":"Where?"},"answers":[{"answer_id":7,"poll_media":{"text":"Home","emoji":{"id":null,"name":"\ud83d\ude80"}}},{"answer_id":42,"poll_media":{"text":"Away","emoji":{"id":"18446744073709551615","name":null,"animated":true}}}],"expiry":"2026-10-05T18:00:00Z","allow_multiselect":true,"layout_type":1,"results":{"is_finalized":false,"answer_counts":[{"id":42,"count":12,"me_voted":true}]}})");
GatewayEventDecodeResult decode(Utf8View body, bool complete = false,
                                GatewayMessagePollScratch scratch = {{f.answers, 10}, {f.counts, 10}},
                                u32 text_capacity = sizeof(f.text)) noexcept {
    const auto scan = json_scan_structurals_avx1(body, {f.structurals, 2048});
    if (!scan.ok())
        return {scan.error_offset, GatewayEventDecodeError::scan_failed, scan.error};
    return gateway_decode_message_poll(
        {body, 0,
         body.bytes == 4 && body.data[0] == 'n' ? JsonValueKind::null_value : JsonValueKind::object},
        {body, {f.structurals, scan.count}}, 32, {f.text, text_capacity}, &f.used, scratch, &f.output,
        complete);
}
GatewayMessageRichScratch rich() noexcept {
    return {{f.reactions, 20},
            {f.attachments, 10},
            {f.embeds, 10},
            {f.fields, 64},
            {f.stickers, 3},
            {f.components, 40},
            {{f.child_attachments, 10},
             {f.child_embeds, 10},
             {f.child_fields, 64},
             {f.child_stickers, 3},
             {f.child_components, 40},
             {f.child_users, 100}},
            {{f.answers, 10}, {f.counts, 10}}};
}
bool append_message(ProtocolWriter& writer, u64 id, Utf8View poll, bool hit = false) noexcept {
    const bool written =
        protocol_writer_append_literal(&writer, "{\"id\":\"") && protocol_writer_append_u64(&writer, id) &&
        protocol_writer_append_literal(&writer, "\",\"channel_id\":\"7\",\"author\":{\"id\":\"11\"},"
                                                "\"content\":\"\",\"timestamp\":\"2026-10-05T00:00:00Z\"");
    return written &&
           (!poll.data || (protocol_writer_append_literal(&writer, ",\"poll\":") &&
                           protocol_writer_append_bytes(
                               &writer, {reinterpret_cast<const u8*>(poll.data), poll.bytes}))) &&
           (!hit || protocol_writer_append_literal(&writer, ",\"hit\":true")) &&
           protocol_writer_append_byte(&writer, '}');
}

bool search_sink(void*, u32 index, const MessageSearchMessage& message) noexcept {
    if (index >= 2 || !message.message.poll.present || !message.message.poll.answers.count ||
        message.message.poll.question.bytes > sizeof(f.sink_questions[0]))
        return false;
    ++f.sink_calls;
    copy_bytes(f.sink_questions[index], message.message.poll.question.data,
               message.message.poll.question.bytes);
    f.sink_answer_ids[index] = message.message.poll.answers[0].answer_id;
    return true;
}

struct ReactionBatchCapture {
    GatewayMessageReactionEvent events[8];
    char names[8][kMessageReactionMaximumEmojiNameBytes];
    u32 calls, stop_after;
    bool bridge_invalid;
};
bool reaction_batch_sink(void* opaque, const GatewayMessageReactionEvent& event) noexcept {
    auto& capture = *static_cast<ReactionBatchCapture*>(opaque);
    const u32 index = capture.calls++;
    if (capture.stop_after && index >= capture.stop_after)
        return false;
    if (index < 8) {
        capture.events[index] = event;
        copy_bytes(capture.names[index], event.name.data, event.name.bytes);
        capture.events[index].name = {capture.names[index], event.name.bytes};
    }
    return true;
}
bool reaction_batch_publish_sink(void* opaque, const GatewayClientEvent& event,
                                 ConstSlice<u8> payload) noexcept {
    auto& capture = *static_cast<ReactionBatchCapture*>(opaque);
    GatewayMessageReactionEvent reaction{};
    const bool decoded = gateway_client_event_as_message_reaction(event, payload, &reaction);
    capture.bridge_invalid =
        capture.bridge_invalid || !decoded || event.kind != GatewayClientEventKind::dispatch ||
        event.dispatch_kind != GatewayClientDispatchKind::message_reaction_add || !event.has_sequence ||
        event.sequence != 91 || event.timestamp_ms != 1234 ||
        event.dispatch_payload_bytes != payload.count || event.reaction.name_bytes != payload.count ||
        reaction.kind != GatewayMessageReactionEventKind::add ||
        reaction.type != MessageReactionType::normal || reaction.channel_id != 7 ||
        reaction.message_id != 77;
    return decoded && reaction_batch_sink(opaque, reaction);
}
void reaction_batch_checks(void (*check)(bool, const char*) noexcept) noexcept {
    ReactionBatchCapture capture{};
    const auto visit = [&](Utf8View body,
                           u32 name_capacity = kMessageReactionMaximumEmojiNameBytes) noexcept {
        return gateway_visit_message_reaction_add_many({body, 0, JsonValueKind::object},
                                                       {f.structurals, 2048}, 32, {f.text, name_capacity},
                                                       &capture, reaction_batch_sink);
    };
    constexpr auto batch = utf8_literal(
        R"({"channel_id":"7","message_id":"77","guild_id":"9","reactions":[{"users":["11","12","11"],"emoji":{"id":null,"name":"\u2764\ufe0f"}},{"users":["11"],"emoji":{"id":"44","name":null,"animated":true}},{"users":["12"],"emoji":{"id":"45","name":"wave","animated":true}}]})");
    const auto accepted = visit(batch);
    bool normal = true;
    for (u32 i = 0; i < capture.calls && i < 8; ++i)
        normal = normal && capture.events[i].kind == GatewayMessageReactionEventKind::add &&
                 capture.events[i].type == MessageReactionType::normal &&
                 capture.events[i].channel_id == 7 && capture.events[i].message_id == 77;
    for (char& byte : f.text)
        byte = '!';
    check(accepted.ok() && accepted.visited == 5 && capture.calls == 5 && normal &&
              capture.events[0].user_id == 11 && capture.events[1].user_id == 12 &&
              capture.events[2].user_id == 11 && capture.events[3].emoji_id == 44 &&
              !capture.events[3].name.bytes && capture.events[3].animated &&
              capture.events[4].emoji_id == 45 && capture.events[4].animated &&
              utf8_equal(capture.events[0].name, utf8_literal("\xe2\x9d\xa4\xef\xb8\x8f")) &&
              utf8_equal(capture.events[4].name, utf8_literal("wave")),
          "reaction batch preserves normal additions wire order repeated users Unicode and unavailable "
          "custom emoji");
    capture = {};
    GatewayEnvelope envelope{};
    envelope.data = {batch, 0, JsonValueKind::object};
    envelope.event_name = {utf8_literal("MESSAGE_REACTION_ADD_MANY"), 0, false};
    envelope.sequence = 91;
    envelope.has_sequence = true;
    envelope.has_event_name = true;
    const auto published =
        gateway_client_publish_reaction_batch(envelope, {f.structurals, 2048}, 32, {f.text, sizeof(f.text)},
                                              1234, reaction_batch_publish_sink, &capture);
    for (char& byte : f.text)
        byte = '!';
    check(published.ok() && published.visited == 5 && capture.calls == 5 && !capture.bridge_invalid &&
              capture.events[0].user_id == 11 && capture.events[1].user_id == 12 &&
              capture.events[2].user_id == 11 && capture.events[3].emoji_id == 44 &&
              capture.events[3].animated && !capture.events[3].name.bytes &&
              capture.events[4].emoji_id == 45 && capture.events[4].animated &&
              utf8_equal(capture.events[0].name, utf8_literal("\xe2\x9d\xa4\xef\xb8\x8f")) &&
              utf8_equal(capture.events[4].name, utf8_literal("wave")),
          "reaction batch bridge stamps sequence time and individual add descriptors with copied name "
          "payloads");
    capture = {};
    capture.stop_after = 1;
    GatewayReactionBatchCursor cursor{};
    const auto publish_cancelled =
        gateway_client_publish_reaction_batch(envelope, {f.structurals, 2048}, 32, {f.text, sizeof(f.text)},
                                              1234, reaction_batch_publish_sink, &capture, &cursor);
    check(!publish_cancelled.ok() && publish_cancelled.decoded.ok() && publish_cancelled.cancelled &&
              publish_cancelled.visited == 1 && capture.calls == 2 && !capture.bridge_invalid &&
              cursor.active && cursor.sequence == 91 && cursor.published == 1,
          "reaction batch bridge stops immediately and retains the accepted prefix for resume");
    capture = {};
    capture.stop_after = 1;
    const auto replay_cancelled =
        gateway_client_publish_reaction_batch(envelope, {f.structurals, 2048}, 32, {f.text, sizeof(f.text)},
                                              1234, reaction_batch_publish_sink, &capture, &cursor);
    check(!replay_cancelled.ok() && replay_cancelled.decoded.ok() && replay_cancelled.cancelled &&
              replay_cancelled.visited == 2 && capture.calls == 2 && !capture.bridge_invalid &&
              capture.events[0].user_id == 12 && cursor.active && cursor.sequence == 91 &&
              cursor.published == 2,
          "reaction batch resumed cancellation advances total progress without repeating accepted users");
    capture = {};
    auto shorter = envelope;
    shorter.data = {
        utf8_literal(
            R"({"channel_id":"7","message_id":"77","reactions":[{"users":["11"],"emoji":{"name":"x"}}]})"),
        0, JsonValueKind::object};
    const auto shortened =
        gateway_client_publish_reaction_batch(shorter, {f.structurals, 2048}, 32, {f.text, sizeof(f.text)},
                                              1234, reaction_batch_publish_sink, &capture, &cursor);
    check(!shortened.ok() && shortened.decoded.error == GatewayEventDecodeError::invalid_data &&
              !shortened.cancelled && !capture.calls && cursor.active && cursor.published == 2,
          "reaction batch replay shorter than its accepted prefix is rejected without publishing");
    const auto resumed =
        gateway_client_publish_reaction_batch(envelope, {f.structurals, 2048}, 32, {f.text, sizeof(f.text)},
                                              1234, reaction_batch_publish_sink, &capture, &cursor);
    check(resumed.ok() && resumed.visited == 5 && capture.calls == 3 && !capture.bridge_invalid &&
              capture.events[0].user_id == 11 && capture.events[0].emoji_id == 0 &&
              capture.events[1].user_id == 11 && capture.events[1].emoji_id == 44 &&
              capture.events[2].user_id == 12 && capture.events[2].emoji_id == 45 &&
              utf8_equal(capture.events[2].name, utf8_literal("wave")) && !cursor.active &&
              !cursor.sequence && !cursor.published,
          "reaction batch resume publishes only its remaining suffix and clears completed progress");
    capture = {};
    check(
        visit(utf8_literal(R"({"channel_id":"7","message_id":"77","reactions":[]})")).ok() &&
            visit(
                utf8_literal(
                    R"({"channel_id":"7","message_id":"77","reactions":[{"users":[],"emoji":{"id":null,"name":"x"}}]})"))
                .ok() &&
            !capture.calls,
        "reaction batch empty arrays are validated no-ops");
    const Utf8View malformed[]{
        utf8_literal(
            R"({"channel_id":"7","message_id":"77","reactions":[{"users":["11"],"emoji":{"name":"x"}},{"users":["0"],"emoji":{"name":"y"}}]})"),
        utf8_literal(
            R"({"channel_id":"7","message_id":"77","reactions":[{"users":["11"],"emoji":{"name":"x"}},{"users":["12"],"emoji":{"name":"y","name":"z"}}]})"),
        utf8_literal(
            R"({"channel_id":"7","message_id":"77","reactions":[{"users":null,"emoji":{"name":"x"}}]})"),
        utf8_literal(
            R"({"channel_id":"7","message_id":"77","reactions":[{"users":["11"],"emoji":{"id":null,"name":null}}]})"),
        utf8_literal(R"({"channel_id":"7","message_id":"77","reactions":[],"reactions":[]})"),
        utf8_literal(R"({"channel_id":"7","message_id":"77","guild_id":null,"reactions":[]})")};
    bool atomic = true;
    for (const auto body : malformed) {
        capture = {};
        const auto rejected = visit(body);
        atomic = atomic && !rejected.ok() && !rejected.decoded.ok() && !rejected.cancelled &&
                 !rejected.visited && !capture.calls;
    }
    check(atomic, "reaction batch rejects malformed later users emoji duplicate fields and metadata before "
                  "publishing a prefix");
    capture = {};
    check(!visit(batch, 1).ok() && !capture.calls,
          "reaction batch name capacity failure cannot publish an earlier valid reaction");
    capture = {};
    capture.stop_after = 1;
    const auto cancelled = visit(batch);
    check(!cancelled.ok() && cancelled.decoded.ok() && cancelled.cancelled && cancelled.visited == 1 &&
              capture.calls == 2,
          "reaction batch stops at the first rejected callback without visiting later entries");
    capture = {};
    ProtocolWriter writer{{f.generated, sizeof(f.generated)}, 0};
    bool written =
        protocol_writer_append_literal(&writer, "{\"channel_id\":\"7\",\"message_id\":\"77\",\"reactions\":"
                                                "[{\"emoji\":{\"name\":\"x\"},\"users\":[");
    for (u32 i = 1; i <= 300; ++i)
        written = written && (i == 1 || protocol_writer_append_byte(&writer, ',')) &&
                  protocol_writer_append_byte(&writer, '"') && protocol_writer_append_u64(&writer, i) &&
                  protocol_writer_append_byte(&writer, '"');
    written = written && protocol_writer_append_literal(&writer, "]}]}");
    const Utf8View large{reinterpret_cast<const char*>(f.generated), writer.used};
    const auto streamed = visit(large);
    check(written && streamed.ok() && streamed.visited == 300 && capture.calls == 300,
          "reaction batch streams a populated message linearly without a small actor table cap");
    capture = {};
    const auto aliased = gateway_visit_message_reaction_add_many(
        {large, 0, JsonValueKind::object}, {f.structurals, 2048}, 32,
        {reinterpret_cast<char*>(f.generated), 128}, &capture, reaction_batch_sink);
    check(!aliased.ok() && aliased.decoded.error == GatewayEventDecodeError::invalid_input &&
              !capture.calls && f.generated[0] == '{',
          "reaction batch rejects scratch aliasing before overwriting its source");
}
} // namespace
void test_gateway_message_poll(void (*check)(bool, const char*) noexcept) noexcept {
    reaction_batch_checks(check);
    f.used = 0;
    check(decode(complete_poll, true).ok() && f.output.present && f.output.fields == 63 &&
              f.output.answers.count == 2 && f.output.answers[0].answer_id == 7 &&
              f.output.answers[1].answer_id == 42 && f.output.answers[1].emoji_id == ~0ull &&
              f.output.answers[1].emoji_animated &&
              utf8_equal(f.output.answers[0].emoji_name, utf8_literal("\xF0\x9F\x9A\x80")) &&
              f.output.results_known && !f.output.is_finalized && f.output.answer_counts.count == 1 &&
              f.output.answer_counts[0].answer_id == 42 && f.output.answer_counts[0].count == 12 &&
              f.output.answer_counts[0].me_voted,
          "poll decode retains arbitrary answer IDs own votes Unicode and custom emoji from owning text "
          "scratch");
    check(gateway_message_poll_valid(f.output),
          "poll decoded DTO satisfies the shared bounded semantic validator");
    const auto before = f.output;
    const auto first_answer = f.answers[0];
    const auto count = f.counts[0];
    const u32 before_used = f.used;
    check(
        !decode(
             utf8_literal(
                 R"({"question":{"text":"new"},"answers":[{"answer_id":7,"poll_media":{"text":"first"}},{"answer_id":7,"poll_media":{"text":"duplicate"}}]})"))
                .ok() &&
            f.used == before_used && f.output.question.data == before.question.data &&
            f.output.answers.count == before.answers.count &&
            f.answers[0].answer_id == first_answer.answer_id &&
            utf8_equal(f.answers[0].text, first_answer.text) && f.counts[0].count == count.count,
        "poll duplicate later answer fails without replacing descriptor banks text prefix or output DTO");
    check(!decode(complete_poll, true, {{f.answers, 1}, {f.counts, 10}}).ok() && f.used == before_used &&
              f.output.question.data == before.question.data &&
              f.answers[0].answer_id == first_answer.answer_id,
          "poll descriptor capacity failure is transactional and never truncates an answer inventory");
    check(!decode(complete_poll, true, {{f.answers, 10}, {f.counts, 10}}, before_used).ok() &&
              f.used == before_used && f.output.question.data == before.question.data,
          "poll decoded text capacity failure preserves existing owned prefix and output");
    const GatewayMessagePollScratch alias{{reinterpret_cast<GatewayMessagePollAnswer*>(f.text), 10},
                                          {f.counts, 10}};
    check(!decode(complete_poll, true, alias).ok() && f.used == before_used &&
              f.output.question.data == before.question.data,
          "poll output banks overlapping decoded text are rejected before any write");
    f.used = 0;
    check(decode(utf8_literal(R"({"results":{"is_finalized":true,"answer_counts":[]}})")).ok() &&
              f.output.fields == gateway_message_poll_field_results && f.output.results_known &&
              f.output.is_finalized && !f.output.answer_counts.count,
          "poll known empty results are distinct from unknown and preserve precise finalization metadata");
    check(decode(utf8_literal(R"({"question":{"text":"new question"}})")).ok() &&
              !(f.output.fields & gateway_message_poll_field_results) && !f.output.results_known,
          "poll omitted results carry no replacement instruction for previously known counts");
    check(!decode(utf8_literal(R"({"results":null})")).ok() &&
              !decode(utf8_literal(R"({"results":{}})")).ok(),
          "poll schema rejects explicit null results and incomplete result objects instead of inventing "
          "zero votes");
    const auto null_used = f.used;
    check(decode(utf8_literal("null")).ok() && !f.output.present && !f.output.fields && f.used == null_used,
          "poll explicit top-level null clears presence without copying text or fabricating results");
    f.used = 0;
    check(
        decode(utf8_literal(R"({"answers":[],"expiry":null,"layout_type":2,"allow_multiselect":false})"))
                .ok() &&
            f.output.present && !f.output.answers.count && f.output.expiry_null && !f.output.expiry_ms &&
            f.output.layout_type == 2,
        "poll empty answer update nullable expiry and unsupported bounded layout retain readable metadata");
    check(
        !decode(utf8_literal(R"({"question":{},"question":{}})")).ok() &&
            !decode(utf8_literal(R"({"answers":[{"answer_id":0,"poll_media":{}}]})")).ok() &&
            !decode(utf8_literal(R"({"answers":[{"answer_id":4294967296,"poll_media":{}}]})")).ok() &&
            !decode(
                 utf8_literal(
                     R"({"answers":[{"answer_id":7,"poll_media":{}}],"results":{"is_finalized":false,"answer_counts":[{"id":42,"count":1,"me_voted":false}]}})"))
                 .ok() &&
            !decode(
                 utf8_literal(
                     R"({"results":{"is_finalized":false,"answer_counts":[{"id":7,"count":1,"me_voted":false},{"id":7,"count":2,"me_voted":true}]}})"))
                 .ok(),
        "poll duplicate fields zero overflowing IDs foreign and duplicate count identities reject "
        "atomically");
    check(!decode(utf8_literal(R"({"expiry":"2026-02-30T00:00:00Z"})")).ok() &&
              !decode(utf8_literal(R"({"allow_multiselect":1})")).ok() &&
              !decode(utf8_literal(R"({"layout_type":65536})")).ok() &&
              !decode(utf8_literal(R"({"answers":[{"answer_id":1,"poll_media":{"emoji":{"id":"01"}}}]})"))
                   .ok(),
          "poll malformed timestamps booleans unbounded layouts and noncanonical emoji snowflakes reject");
    check(decode(utf8_literal(R"({"question":{"text":null},"answers":[{"answer_id":7,"poll_media":{}}]})"))
                  .ok() &&
              !f.output.question.bytes && !f.output.answers[0].text.bytes,
          "poll future nullable or omitted media text remains empty instead of fabricated question or "
          "answer text");

    const u32 question_limits[]{300u, 301u};
    for (u32 limit : question_limits) {
        ProtocolWriter writer{{f.generated, sizeof(f.generated)}, 0};
        bool written = protocol_writer_append_literal(&writer, "{\"question\":{\"text\":\"");
        for (u32 i = 0; written && i < limit; ++i)
            written = protocol_writer_append_literal(&writer, "\xF0\x9F\x9A\x80");
        written = written && protocol_writer_append_literal(&writer, "\"}}");
        f.used = 0;
        const auto decoded = decode({reinterpret_cast<const char*>(f.generated), writer.used});
        check(written && (limit == 300 ? decoded.ok() && f.output.question.bytes == 1200 : !decoded.ok()),
              "poll question enforces Unicode scalar and UTF8 byte boundaries without truncation");
    }
    const u32 answer_limits[]{55u, 56u};
    for (u32 limit : answer_limits) {
        ProtocolWriter writer{{f.generated, sizeof(f.generated)}, 0};
        bool written = protocol_writer_append_literal(
            &writer, "{\"answers\":[{\"answer_id\":7,\"poll_media\":{\"text\":\"");
        for (u32 i = 0; written && i < limit; ++i)
            written = protocol_writer_append_literal(&writer, "\xF0\x9F\x9A\x80");
        written = written && protocol_writer_append_literal(&writer, "\"}}]}");
        f.used = 0;
        const auto decoded = decode({reinterpret_cast<const char*>(f.generated), writer.used});
        check(written &&
                  (limit == 55 ? decoded.ok() && f.output.answers[0].text.bytes == 220 : !decoded.ok()),
              "poll answer enforces Unicode scalar and UTF8 byte boundaries without truncation");
    }
    for (u32 index = 0; index < 2; ++index) {
        ProtocolWriter boundary{{f.generated, sizeof(f.generated)}, 0};
        bool written = protocol_writer_append_c_string(
            &boundary, index ? "{\"answers\":[{\"answer_id\":1,\"poll_media\":{\"text\":\""
                             : "{\"question\":{\"text\":\"");
        const u32 scalars = index ? 56 : 301;
        for (u32 i = 0; written && i < scalars; ++i)
            written = protocol_writer_append_byte(&boundary, 'a');
        written = written && protocol_writer_append_c_string(&boundary, index ? "\"}}]}" : "\"}}");
        f.used = 0;
        check(written && !decode({reinterpret_cast<const char*>(f.generated), boundary.used}).ok(),
              "poll ASCII text proves scalar limits independently of the larger UTF8 byte cap");
    }
    ProtocolWriter writer{{f.generated, sizeof(f.generated)}, 0};
    bool written = protocol_writer_append_literal(&writer, "{\"answers\":[");
    for (u32 i = 0; written && i < 11; ++i)
        written = (!i || protocol_writer_append_byte(&writer, ',')) &&
                  protocol_writer_append_literal(&writer, "{\"answer_id\":") &&
                  protocol_writer_append_u64(&writer, i + 1) &&
                  protocol_writer_append_literal(&writer, ",\"poll_media\":{\"text\":\"a\"}}");
    written = written && protocol_writer_append_literal(&writer, "]}");
    f.used = 0;
    check(written && !decode({reinterpret_cast<const char*>(f.generated), writer.used}).ok(),
          "poll rejects eleven answers without partial inventory publication");

    constexpr auto vote_body = utf8_literal(
        R"({"channel_id":"7","message_id":"900","user_id":"11","guild_id":"19","answer_id":42})");
    GatewayMessagePollVoteEvent vote{};
    check(gateway_decode_message_poll_vote({vote_body, 0, JsonValueKind::object}, {f.structurals, 2048}, 32,
                                           true, &vote)
                  .ok() &&
              vote.channel_id == 7 && vote.message_id == 900 && vote.user_id == 11 && vote.guild_id == 19 &&
              vote.has_guild && vote.answer_id == 42 && vote.add,
          "poll vote add parser retains exact channel message actor guild and arbitrary answer identity");
    constexpr auto dm_vote =
        utf8_literal(R"({"channel_id":"7","message_id":"900","user_id":"11","answer_id":7})");
    check(gateway_decode_message_poll_vote({dm_vote, 0, JsonValueKind::object}, {f.structurals, 2048}, 32,
                                           false, &vote)
                  .ok() &&
              !vote.add && !vote.has_guild && !vote.guild_id,
          "poll vote removal permits absent guild for direct messages without inventing guild authority");
    const auto old_vote = vote;
    constexpr auto malformed_vote =
        utf8_literal(R"({"channel_id":"7","message_id":"900","user_id":"11","answer_id":0})");
    check(!gateway_decode_message_poll_vote({malformed_vote, 0, JsonValueKind::object},
                                            {f.structurals, 2048}, 32, true, &vote)
                  .ok() &&
              vote.answer_id == old_vote.answer_id && vote.add == old_vote.add,
          "poll malformed vote cannot partially overwrite a previous typed receipt");

    constexpr auto duplicate_vote = utf8_literal(
        R"({"channel_id":"7","message_id":"900","user_id":"11","answer_id":7,"answer_id":42})");
    check(!gateway_decode_message_poll_vote({duplicate_vote, 0, JsonValueKind::object},
                                            {f.structurals, 2048}, 32, true, &vote)
                  .ok() &&
              vote.answer_id == old_vote.answer_id && !vote.add,
          "poll duplicate vote answer identity cannot overwrite a retained receipt");
    check(!gateway_decode_message_poll_vote({vote_body, 0, JsonValueKind::object}, {f.structurals, 2048},
                                            32, true,
                                            reinterpret_cast<GatewayMessagePollVoteEvent*>(f.structurals))
               .ok(),
          "poll vote output overlapping structural scratch rejects before scanning or publication");
    writer.used = 0;
    written = append_message(writer, 900, complete_poll);
    GatewayMessageEvent basic{};
    check(written &&
              gateway_decode_message_create(
                  {{reinterpret_cast<const char*>(f.generated), writer.used}, 0, JsonValueKind::object},
                  {f.structurals, 2048}, 32, {f.text, sizeof(f.text)}, &basic)
                  .ok() &&
              basic.id == 900 && (basic.fields & gateway_message_field_poll) && basic.poll.present &&
              !basic.poll.fields,
          "poll basic core decode succeeds without poll banks and retains presence without inventing full "
          "metadata");
    writer.used = 0;
    written =
        protocol_writer_append_literal(&writer, "{\"op\":0,\"s\":1,\"t\":\"MESSAGE_CREATE\",\"d\":") &&
        append_message(writer, 900, complete_poll) && protocol_writer_append_byte(&writer, '}');
    GatewayEnvelope envelope{};
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    bool routed =
        written &&
        gateway_decode_envelope({reinterpret_cast<const char*>(f.generated), writer.used},
                                {f.structurals, 2048}, 32, &envelope)
            .ok() &&
        gateway_client_route_dispatch(envelope, {f.message_structurals, 2048}, 32,
                                      {f.mailbox, sizeof(f.mailbox)}, {f.roles, 100}, 11, &event, &payload)
            .ok();
    for (auto& byte : f.generated)
        byte = '!';
    GatewayMessageEvent decoded_message{};
    check(routed && gateway_client_event_as_message(event, payload, rich(), &decoded_message) &&
              decoded_message.poll.present && decoded_message.poll.answers.count == 2 &&
              decoded_message.poll.answer_counts[0].count == 12 &&
              utf8_equal(decoded_message.poll.question, utf8_literal("Where?")),
          "poll actual pointer-free Gateway mailbox survives complete JSON source reuse");
    check(routed && event.message.rich_payload_bytes ==
                        sizeof(GatewayClientMessageRichHeader) + sizeof(GatewayClientMessagePollHeader) +
                            2 * sizeof(GatewayClientMessagePollAnswerMutation) +
                            sizeof(GatewayClientMessagePollCountMutation),
          "poll optional mailbox descriptor block leaves ordinary rich header width unchanged");
    if (routed) {
        const u32 header_at =
            payload.count - event.message.rich_payload_bytes + sizeof(GatewayClientMessageRichHeader);
        GatewayClientMessagePollHeader packed{};
        copy_bytes(&packed, payload.data + header_at, sizeof(packed));
        packed.reserved[0] = 1;
        copy_bytes(f.mailbox + header_at, &packed, sizeof(packed));
        check(!gateway_client_event_as_message(event, payload, rich(), &decoded_message),
              "poll forged reserved mailbox metadata is rejected before model publication");
    }
    writer.used = 0;
    written = protocol_writer_append_literal(
                  &writer, "{\"op\":0,\"s\":2,\"t\":\"MESSAGE_POLL_VOTE_REMOVE\",\"d\":") &&
              protocol_writer_append_bytes(
                  &writer, {reinterpret_cast<const u8*>(vote_body.data), vote_body.bytes}) &&
              protocol_writer_append_byte(&writer, '}');
    routed =
        written &&
        gateway_decode_envelope({reinterpret_cast<const char*>(f.generated), writer.used},
                                {f.structurals, 2048}, 32, &envelope)
            .ok() &&
        gateway_client_route_dispatch(envelope, {f.message_structurals, 2048}, 32,
                                      {f.mailbox, sizeof(f.mailbox)}, {f.roles, 100}, 11, &event, &payload)
            .ok();
    check(routed && event.dispatch_kind == GatewayClientDispatchKind::message_poll_vote_remove &&
              gateway_client_event_as_message_poll_vote(event, payload, &vote) && !vote.add &&
              vote.answer_id == 42,
          "poll standalone vote removal routes through production typed Gateway accessor");
    event.poll_vote.add = true;
    check(!gateway_client_event_as_message_poll_vote(event, payload, &vote),
          "poll mismatched dispatch kind and receipt direction cannot publish");

    writer.used = 0;
    written = protocol_writer_append_byte(&writer, '[') && append_message(writer, 900, complete_poll) &&
              protocol_writer_append_byte(&writer, ',') && append_message(writer, 800, complete_poll) &&
              protocol_writer_append_byte(&writer, ']');
    MessageHistoryRichScratch page_scratch{};
    page_scratch.poll = {{f.answers, 20}, {f.counts, 20}};
    const auto page = decode_message_history_page(
        {reinterpret_cast<const char*>(f.generated), writer.used}, {f.structurals, 2048},
        {f.message_structurals, 2048}, 32, {f.text, sizeof(f.text)}, {f.messages, 2}, {}, page_scratch);
    check(written && page.ok() && f.messages[0].poll.answers.data != f.messages[1].poll.answers.data &&
              f.messages[0].poll.answer_counts.data != f.messages[1].poll.answer_counts.data &&
              f.messages[0].poll.answers[0].answer_id == 7 && f.messages[1].poll.answers[1].answer_id == 42,
          "poll REST history gives each page message nonoverlapping answer and count descriptors");
    page_scratch.poll.answers.count = 3;
    const auto exhausted = decode_message_history_page(
        {reinterpret_cast<const char*>(f.generated), writer.used}, {f.structurals, 2048},
        {f.message_structurals, 2048}, 32, {f.text, sizeof(f.text)}, {f.messages, 2}, {}, page_scratch);
    // The later message cannot get its own answer descriptors. It degrades to
    // its core text (poll dropped) while the first message keeps the exact
    // inventory it was given; nothing is shared or overwritten.
    check(written && exhausted.ok() && exhausted.page.count == 2 && exhausted.degraded_messages == 1 &&
              f.messages[0].id == 900 && f.messages[0].poll.present &&
              (f.messages[0].fields & gateway_message_field_poll) &&
              f.messages[0].poll.answers.data == f.answers && f.messages[0].poll.answers.count == 2 &&
              f.answers[0].answer_id == 7 && f.answers[1].answer_id == 42 &&
              f.messages[0].poll.answer_counts.data == f.counts &&
              utf8_equal(f.messages[0].poll.question, utf8_literal("Where?")) && f.messages[1].id == 800 &&
              !(f.messages[1].fields & gateway_message_field_poll) && !f.messages[1].poll.present &&
              !f.messages[1].poll.answers.data && !f.messages[1].poll.answers.count &&
              !f.messages[1].poll.answer_counts.data && !f.messages[1].poll.answer_counts.count &&
              !f.messages[1].poll.question.bytes,
          "poll later history descriptor exhaustion degrades only the later message to core text and "
          "never overwrites first message inventory");

    writer.used = 0;
    written = protocol_writer_append_byte(&writer, '[') && append_message(writer, 900, complete_poll) &&
              protocol_writer_append_byte(&writer, ',') &&
              append_message(writer, 800, utf8_literal("null")) &&
              protocol_writer_append_byte(&writer, ',') && append_message(writer, 700, {}) &&
              protocol_writer_append_byte(&writer, ']');
    page_scratch.poll = {{f.answers, 20}, {f.counts, 20}};
    const auto mixed = decode_message_history_page(
        {reinterpret_cast<const char*>(f.generated), writer.used}, {f.structurals, 2048},
        {f.message_structurals, 2048}, 32, {f.text, sizeof(f.text)}, {f.messages, 3}, {}, page_scratch);
    for (auto& byte : f.generated)
        byte = '!';
    check(written && mixed.ok() && f.messages[0].poll.present && f.messages[0].poll.answers.count == 2 &&
              utf8_equal(f.messages[0].poll.question, utf8_literal("Where?")) &&
              (f.messages[1].fields & gateway_message_field_poll) && !f.messages[1].poll.present &&
              !(f.messages[2].fields & gateway_message_field_poll) && !f.messages[2].poll.present,
          "poll mixed history keeps owned poll text lifetime and distinguishes null removal from omitted "
          "field");
    writer.used = 0;
    written = protocol_writer_append_literal(&writer, "{\"total_results\":2,\"messages\":[[") &&
              append_message(writer, 900, complete_poll, true) &&
              protocol_writer_append_literal(&writer, "],[") &&
              append_message(writer, 800, complete_poll, true) &&
              protocol_writer_append_literal(&writer, "]]}");
    MessageSearchQuery query{};
    query.scope = MessageSearchScope::channel;
    query.scope_id = 7;
    query.limit = 2;
    query.has = search_has_poll;
    const MessageSearchRichScratch search_scratch{
        {f.roles, 100}, {f.member_roles, 100}, {f.users, 100}, rich()};
    f.sink_calls = 0;
    const auto search = decode_message_search_page(
        {reinterpret_cast<const char*>(f.generated), writer.used}, query, {f.structurals, 2048},
        {f.message_structurals, 2048}, 32, {f.text, sizeof(f.text)}, {f.groups, 2}, {f.search_messages, 2},
        search_scratch, {nullptr, search_sink});
    check(written && search.ok() && f.sink_calls == 2 && f.sink_answer_ids[0] == 7 &&
              f.sink_answer_ids[1] == 7 && bytes_equal(f.sink_questions[0], "Where?", 6) &&
              bytes_equal(f.sink_questions[1], "Where?", 6) &&
              !(f.search_messages[0].message.fields & gateway_message_field_poll) &&
              !(f.search_messages[1].message.fields & gateway_message_field_poll) &&
              !f.search_messages[0].message.poll.question.bytes &&
              !f.search_messages[0].message.poll.answers.count &&
              !f.search_messages[0].message.poll.answer_counts.count &&
              !f.search_messages[1].message.poll.question.bytes &&
              !f.search_messages[1].message.poll.answers.count &&
              !f.search_messages[1].message.poll.answer_counts.count,
          "poll rich Search sink receives complete reusable metadata and scalar summaries retain no "
          "borrowed poll views");
}
} // namespace maicord
