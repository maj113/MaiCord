#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
inline constexpr u32 kPollTextMaximumBytes =
    kGatewayMessagePollQuestionMaximumBytes +
    kGatewayMessagePollMaximumAnswers *
        (kGatewayMessagePollAnswerMaximumBytes + kGatewayMessagePollEmojiMaximumBytes);
inline constexpr u32 kPollFields = gateway_message_poll_field_question |
                                   gateway_message_poll_field_answers | gateway_message_poll_field_expiry |
                                   gateway_message_poll_field_multiselect |
                                   gateway_message_poll_field_layout | gateway_message_poll_field_results;
GatewayEventDecodeResult failure(GatewayEventDecodeError error, u32 offset = 0,
                                 JsonReadError reader = JsonReadError::none) noexcept {
    return {offset, error, JsonScanError::none, reader};
}
bool overlap(const void* left, u64 left_bytes, const void* right, u64 right_bytes) noexcept {
    if (!left_bytes || !right_bytes)
        return false;
    const auto a = reinterpret_cast<usize>(left), b = reinterpret_cast<usize>(right);
    return !left || !right || left_bytes > ~static_cast<usize>(0) - a ||
           right_bytes > ~static_cast<usize>(0) - b || (a < b + right_bytes && b < a + left_bytes);
}
bool mark(u32* fields, u32 bit, GatewayEventDecodeError* error) noexcept {
    if ((*fields & bit) != 0) {
        *error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    *fields |= bit;
    return true;
}
bool nonzero_u32(JsonValue value, u32* out) noexcept {
    u64 number = 0;
    if (!json_value_to_u64(value, &number) || !number || number > ~0u)
        return false;
    *out = static_cast<u32>(number);
    return true;
}
bool canonical_snowflake(JsonValue value, u64* out) noexcept {
    JsonString string{};
    if (!json_value_as_string(value, &string) || string.has_escapes || !string.encoded.data ||
        !string.encoded.bytes || string.encoded.bytes > 20 || string.encoded.data[0] == '0')
        return false;
    return json_value_to_snowflake(value, out);
}
struct Text {
    char data[kPollTextMaximumBytes];
    u32 used;
    GatewayEventDecodeError error;
};
bool text(JsonValue value, Text* bank, u32 byte_limit, u32 scalar_limit, Utf8View* out,
          bool nullable = false) noexcept {
    if (nullable && value.kind == JsonValueKind::null_value) {
        *out = {};
        return true;
    }
    JsonString string{};
    if (!json_value_as_string(value, &string))
        return false;
    const auto size = json_string_decode_utf8(string, {});
    if ((!size.ok() && size.error != JsonStringDecodeError::output_too_small) || size.bytes > byte_limit ||
        bank->used > sizeof(bank->data) || size.bytes > sizeof(bank->data) - bank->used)
        return false;
    const auto decoded = json_string_decode_utf8(string, {bank->data + bank->used, size.bytes});
    if (!decoded.ok() || decoded.bytes != size.bytes)
        return false;
    const Utf8View result{bank->data + bank->used, decoded.bytes};
    const auto valid = utf8_validate(result);
    if (!valid.valid || valid.codepoints > scalar_limit)
        return false;
    bank->used += decoded.bytes;
    *out = decoded.bytes ? result : Utf8View{};
    return true;
}
struct Media {
    const JsonIndexedDocument* document;
    Text* bank;
    u32 depth, fields;
    Utf8View value, emoji_name;
    u64 emoji_id;
    bool animated, answer;
    GatewayEventDecodeError error;
};
bool emoji_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& context = *static_cast<Media*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("id"))) {
        if (!mark(&context.fields, 2, &context.error))
            return false;
        if (value.kind != JsonValueKind::null_value && !canonical_snowflake(value, &context.emoji_id))
            return false;
    } else if (json_string_equals_ascii(name, utf8_literal("name"))) {
        if (!mark(&context.fields, 4, &context.error) ||
            !text(value, context.bank, kGatewayMessagePollEmojiMaximumBytes,
                  kGatewayMessagePollEmojiMaximumBytes, &context.emoji_name, true))
            return false;
    } else if (json_string_equals_ascii(name, utf8_literal("animated"))) {
        if (!mark(&context.fields, 8, &context.error) || !json_value_to_bool(value, &context.animated))
            return false;
    }
    return true;
}
bool media_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& context = *static_cast<Media*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("text"))) {
        if (!mark(&context.fields, 1, &context.error) ||
            !text(value, context.bank,
                  context.answer ? kGatewayMessagePollAnswerMaximumBytes
                                 : kGatewayMessagePollQuestionMaximumBytes,
                  context.answer ? kGatewayMessagePollAnswerMaximumScalars
                                 : kGatewayMessagePollQuestionMaximumScalars,
                  &context.value, true))
            return false;
    } else if (context.answer && json_string_equals_ascii(name, utf8_literal("emoji"))) {
        if (!mark(&context.fields, 16, &context.error))
            return false;
        if (value.kind == JsonValueKind::null_value)
            return true;
        if (value.kind != JsonValueKind::object)
            return false;
        const auto read =
            json_visit_indexed_object(*context.document, value, context.depth, emoji_member, &context);
        if (!read.ok())
            return false;
        if (!context.emoji_id && !context.emoji_name.bytes)
            return false;
    }
    return true;
}
struct Answer {
    const JsonIndexedDocument* document;
    Text* bank;
    u32 depth, fields;
    GatewayMessagePollAnswer value;
    GatewayEventDecodeError error;
};
bool answer_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& context = *static_cast<Answer*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("answer_id"))) {
        return mark(&context.fields, 1, &context.error) && nonzero_u32(value, &context.value.answer_id);
    }
    if (json_string_equals_ascii(name, utf8_literal("poll_media"))) {
        if (!mark(&context.fields, 2, &context.error) || value.kind != JsonValueKind::object)
            return false;
        Media media{context.document, context.bank, context.depth, 0, {}, {}, 0, false, true, {}};
        const auto read =
            json_visit_indexed_object(*context.document, value, context.depth, media_member, &media);
        if (!read.ok()) {
            context.error = media.error;
            return false;
        }
        context.value.text = media.value;
        context.value.emoji_id = media.emoji_id;
        context.value.emoji_name = media.emoji_name;
        context.value.emoji_animated = media.animated;
    }
    return true;
}
struct Count {
    GatewayMessagePollAnswerCount value;
    u32 fields;
    GatewayEventDecodeError error;
};
bool count_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& context = *static_cast<Count*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("id")))
        return mark(&context.fields, 1, &context.error) && nonzero_u32(value, &context.value.answer_id);
    if (json_string_equals_ascii(name, utf8_literal("count"))) {
        u64 count = 0;
        if (!mark(&context.fields, 2, &context.error) || !json_value_to_u64(value, &count) || count > ~0u)
            return false;
        context.value.count = static_cast<u32>(count);
    } else if (json_string_equals_ascii(name, utf8_literal("me_voted")))
        return mark(&context.fields, 4, &context.error) &&
               json_value_to_bool(value, &context.value.me_voted);
    return true;
}
struct Poll {
    const JsonIndexedDocument* document;
    u32 depth;
    GatewayMessagePoll value;
    GatewayMessagePollAnswer answers[kGatewayMessagePollMaximumAnswers];
    GatewayMessagePollAnswerCount counts[kGatewayMessagePollMaximumAnswers];
    u32 answer_count, count_count, results_fields;
    Text bank;
    GatewayEventDecodeError error;
};
bool answer_item(void* opaque, u32 index, JsonValue value) noexcept {
    auto& context = *static_cast<Poll*>(opaque);
    if (index >= kGatewayMessagePollMaximumAnswers || value.kind != JsonValueKind::object)
        return false;
    Answer answer{context.document, &context.bank, context.depth, 0, {}, {}};
    const auto read =
        json_visit_indexed_object(*context.document, value, context.depth, answer_member, &answer);
    if (!read.ok()) {
        context.error = answer.error;
        return false;
    }
    if (answer.fields != 3)
        return false;
    for (u32 i = 0; i < index; ++i)
        if (context.answers[i].answer_id == answer.value.answer_id)
            return false;
    context.answers[index] = answer.value;
    context.answer_count = index + 1;
    return true;
}
bool count_item(void* opaque, u32 index, JsonValue value) noexcept {
    auto& context = *static_cast<Poll*>(opaque);
    if (index >= kGatewayMessagePollMaximumAnswers || value.kind != JsonValueKind::object)
        return false;
    Count count{};
    const auto read =
        json_visit_indexed_object(*context.document, value, context.depth, count_member, &count);
    if (!read.ok()) {
        context.error = count.error;
        return false;
    }
    if (count.fields != 7)
        return false;
    for (u32 i = 0; i < index; ++i)
        if (context.counts[i].answer_id == count.value.answer_id)
            return false;
    context.counts[index] = count.value;
    context.count_count = index + 1;
    return true;
}
bool results_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& context = *static_cast<Poll*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("is_finalized")))
        return mark(&context.results_fields, 1, &context.error) &&
               json_value_to_bool(value, &context.value.is_finalized);
    if (json_string_equals_ascii(name, utf8_literal("answer_counts"))) {
        if (!mark(&context.results_fields, 2, &context.error) || value.kind != JsonValueKind::array)
            return false;
        return json_visit_indexed_array(*context.document, value, context.depth, count_item, &context).ok();
    }
    return true;
}
bool poll_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& context = *static_cast<Poll*>(opaque);
    u32 bit = 0;
    if (json_string_equals_ascii(name, utf8_literal("question")))
        bit = gateway_message_poll_field_question;
    else if (json_string_equals_ascii(name, utf8_literal("answers")))
        bit = gateway_message_poll_field_answers;
    else if (json_string_equals_ascii(name, utf8_literal("expiry")))
        bit = gateway_message_poll_field_expiry;
    else if (json_string_equals_ascii(name, utf8_literal("allow_multiselect")))
        bit = gateway_message_poll_field_multiselect;
    else if (json_string_equals_ascii(name, utf8_literal("layout_type")))
        bit = gateway_message_poll_field_layout;
    else if (json_string_equals_ascii(name, utf8_literal("results")))
        bit = gateway_message_poll_field_results;
    else
        return true;
    if (!mark(&context.value.fields, bit, &context.error))
        return false;
    if (bit == gateway_message_poll_field_question) {
        if (value.kind != JsonValueKind::object)
            return false;
        Media media{context.document, &context.bank, context.depth, 0, {}, {}, 0, false, false, {}};
        const auto read =
            json_visit_indexed_object(*context.document, value, context.depth, media_member, &media);
        if (!read.ok()) {
            context.error = media.error;
            return false;
        }
        context.value.question = media.value;
    } else if (bit == gateway_message_poll_field_answers) {
        if (value.kind != JsonValueKind::array ||
            !json_visit_indexed_array(*context.document, value, context.depth, answer_item, &context).ok())
            return false;
    } else if (bit == gateway_message_poll_field_expiry) {
        context.value.expiry_null = value.kind == JsonValueKind::null_value;
        if (!context.value.expiry_null) {
            JsonString timestamp{};
            if (!json_value_as_string(value, &timestamp) ||
                !parse_iso_timestamp(timestamp, &context.value.expiry_ms))
                return false;
        }
    } else if (bit == gateway_message_poll_field_multiselect) {
        if (!json_value_to_bool(value, &context.value.allow_multiselect))
            return false;
    } else if (bit == gateway_message_poll_field_layout) {
        u64 layout = 0;
        if (!json_value_to_u64(value, &layout) || layout > 65535)
            return false;
        context.value.layout_type = static_cast<u16>(layout);
    } else {
        if (value.kind != JsonValueKind::object ||
            !json_visit_indexed_object(*context.document, value, context.depth, results_member, &context)
                 .ok() ||
            context.results_fields != 3)
            return false;
        context.value.results_known = true;
    }
    return true;
}
Utf8View relocate(Utf8View value, const Text& source, char* destination) noexcept {
    return value.bytes ? Utf8View{destination + (value.data - source.data), value.bytes} : Utf8View{};
}
struct Vote {
    GatewayMessagePollVoteEvent value;
    u32 fields;
    GatewayEventDecodeError error;
};
bool vote_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& context = *static_cast<Vote*>(opaque);
    u32 bit = 0;
    u64* id = nullptr;
    if (json_string_equals_ascii(name, utf8_literal("channel_id"))) {
        bit = 1;
        id = &context.value.channel_id;
    } else if (json_string_equals_ascii(name, utf8_literal("message_id"))) {
        bit = 2;
        id = &context.value.message_id;
    } else if (json_string_equals_ascii(name, utf8_literal("user_id"))) {
        bit = 4;
        id = &context.value.user_id;
    } else if (json_string_equals_ascii(name, utf8_literal("guild_id"))) {
        bit = 8;
        id = &context.value.guild_id;
    } else if (json_string_equals_ascii(name, utf8_literal("answer_id")))
        bit = 16;
    else
        return true;
    if (!mark(&context.fields, bit, &context.error))
        return false;
    if (id) {
        if (!canonical_snowflake(value, id))
            return false;
        if (bit == 8)
            context.value.has_guild = true;
        return true;
    }
    return nonzero_u32(value, &context.value.answer_id);
}
} // namespace
GatewayEventDecodeResult gateway_decode_message_poll(JsonValue data, const JsonIndexedDocument& document,
                                                     u32 maximum_depth, Slice<char> text_output,
                                                     u32* text_used,
                                                     const GatewayMessagePollScratch& scratch,
                                                     GatewayMessagePoll* output,
                                                     bool require_complete) noexcept {
    if (!output || !text_used || *text_used > text_output.count ||
        (!text_output.data && text_output.count) || (!scratch.answers.data && scratch.answers.count) ||
        (!scratch.answer_counts.data && scratch.answer_counts.count) || !maximum_depth ||
        !data.source.data || !data.source.bytes || data.source.bytes > 256 * 1024 || !document.input.data ||
        (!document.structurals.data && document.structurals.count))
        return failure(GatewayEventDecodeError::invalid_input);
    const void* banks[] = {text_output.data, scratch.answers.data, scratch.answer_counts.data, output,
                           text_used};
    const u64 sizes[] = {
        text_output.count, static_cast<u64>(scratch.answers.count) * sizeof(GatewayMessagePollAnswer),
        static_cast<u64>(scratch.answer_counts.count) * sizeof(GatewayMessagePollAnswerCount),
        sizeof(*output), sizeof(*text_used)};
    for (u32 i = 0; i < 5; ++i) {
        if (overlap(banks[i], sizes[i], document.input.data, document.input.bytes) ||
            overlap(banks[i], sizes[i], document.structurals.data,
                    static_cast<u64>(document.structurals.count) * sizeof(u32)) ||
            overlap(banks[i], sizes[i], &scratch, sizeof(scratch)) ||
            overlap(banks[i], sizes[i], &document, sizeof(document)))
            return failure(GatewayEventDecodeError::invalid_input);
        for (u32 j = i + 1; j < 5; ++j)
            if (overlap(banks[i], sizes[i], banks[j], sizes[j]))
                return failure(GatewayEventDecodeError::invalid_input);
    }
    const auto input_start = reinterpret_cast<usize>(document.input.data);
    const auto value_start = reinterpret_cast<usize>(data.source.data);
    if (value_start < input_start || value_start - input_start > document.input.bytes ||
        data.source.bytes > document.input.bytes - (value_start - input_start))
        return failure(GatewayEventDecodeError::invalid_input);
    if (data.kind == JsonValueKind::null_value) {
        if (data.source.bytes != 4 || !bytes_equal(data.source.data, "null", 4))
            return failure(GatewayEventDecodeError::invalid_data, data.source_offset);
        *output = {};
        return failure(GatewayEventDecodeError::none);
    }
    if (data.kind != JsonValueKind::object)
        return failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    if (!document.structurals.data || !document.structurals.count)
        return failure(GatewayEventDecodeError::invalid_input);
    Poll context{};
    context.document = &document;
    context.depth = maximum_depth;
    context.value.present = true;
    const auto read = json_visit_indexed_object(document, data, maximum_depth, poll_member, &context);
    if (!read.ok())
        return failure(context.error == GatewayEventDecodeError::none
                           ? GatewayEventDecodeError::invalid_data
                           : context.error,
                       read.error_offset, read.error);
    constexpr u32 required = kPollFields & ~gateway_message_poll_field_results;
    if ((require_complete && (context.value.fields & required) != required) || !context.value.fields)
        return failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    if (context.value.fields & gateway_message_poll_field_answers) {
        for (u32 i = 0; i < context.count_count; ++i) {
            bool found = false;
            for (u32 j = 0; j < context.answer_count; ++j)
                found |= context.counts[i].answer_id == context.answers[j].answer_id;
            if (!found)
                return failure(GatewayEventDecodeError::invalid_data, data.source_offset);
        }
    }
    context.value.answers = {context.answers, context.answer_count};
    context.value.answer_counts = {context.counts, context.count_count};
    if (!gateway_message_poll_valid(context.value))
        return failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    if (context.answer_count > scratch.answers.count || context.count_count > scratch.answer_counts.count ||
        context.bank.used > text_output.count - *text_used)
        return failure(GatewayEventDecodeError::message_content_capacity, data.source_offset);
    char* destination = text_output.data ? text_output.data + *text_used : nullptr;
    copy_bytes(destination, context.bank.data, context.bank.used);
    context.value.question = relocate(context.value.question, context.bank, destination);
    for (u32 i = 0; i < context.answer_count; ++i) {
        auto& answer = context.answers[i];
        answer.text = relocate(answer.text, context.bank, destination);
        answer.emoji_name = relocate(answer.emoji_name, context.bank, destination);
        scratch.answers[i] = answer;
    }
    for (u32 i = 0; i < context.count_count; ++i)
        scratch.answer_counts[i] = context.counts[i];
    context.value.answers = {context.answer_count ? scratch.answers.data : nullptr, context.answer_count};
    context.value.answer_counts = {context.count_count ? scratch.answer_counts.data : nullptr,
                                   context.count_count};
    *text_used += context.bank.used;
    *output = context.value;
    return failure(GatewayEventDecodeError::none);
}
GatewayEventDecodeResult gateway_decode_message_poll_vote(JsonValue data, Slice<u32> structural_scratch,
                                                          u32 maximum_depth, bool add,
                                                          GatewayMessagePollVoteEvent* output) noexcept {
    if (!output || data.kind != JsonValueKind::object || !data.source.data || !data.source.bytes ||
        data.source.bytes > 4096 || !structural_scratch.data || !structural_scratch.count ||
        !maximum_depth ||
        overlap(data.source.data, data.source.bytes, structural_scratch.data,
                static_cast<u64>(structural_scratch.count) * sizeof(u32)) ||
        overlap(output, sizeof(*output), data.source.data, data.source.bytes) ||
        overlap(output, sizeof(*output), structural_scratch.data,
                static_cast<u64>(structural_scratch.count) * sizeof(u32)))
        return failure(GatewayEventDecodeError::invalid_input);
    const auto scan = json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok())
        return {scan.error_offset, GatewayEventDecodeError::scan_failed, scan.error};
    Vote context{};
    context.value.add = add;
    const auto read = json_visit_value_object(data, {structural_scratch.data, scan.count},
                                             maximum_depth, vote_member, &context);
    if (!read.ok())
        return failure(context.error == GatewayEventDecodeError::none
                           ? GatewayEventDecodeError::invalid_data
                           : context.error,
                       read.error_offset, read.error);
    if ((context.fields & 23) != 23)
        return failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    *output = context.value;
    return failure(GatewayEventDecodeError::none);
}
} // namespace maicord
