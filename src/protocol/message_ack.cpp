#include "maicord/protocol/message_ack.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/decimal.hpp"
#include "json_write.hpp"

namespace maicord {
namespace {
MessageAckWriteResult fail(MessageAckWriteError error) noexcept { return {0, error}; }
bool token_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& response = *static_cast<MessageAckResponse*>(context);
    if (!json_string_equals_ascii(name, utf8_literal("token"))) return true;
    if (response.token_known) return false;
    response.token_known = true;
    response.token_null = value.kind == JsonValueKind::null_value;
    return response.token_null || json_value_as_string(value, &response.token);
}
} // namespace

MessageAckWriteResult write_message_ack_target(
    u64 channel_id, u64 message_id, Slice<u8> output) noexcept {
    if (!channel_id || !output.data) return fail(MessageAckWriteError::invalid_input);
    constexpr u32 literals = sizeof("/api/v10/channels/") - 1 + sizeof("/messages/") - 1 + sizeof("/ack") - 1;
    const u32 required = literals + decimal_digits(channel_id) + decimal_digits(message_id);
    if (required > output.count || required > kMessageAckMaximumTargetBytes) return fail(MessageAckWriteError::capacity);
    ProtocolWriter writer{output, 0};
    if (!protocol_writer_append_literal(&writer, "/api/v10/channels/") ||
        !protocol_writer_append_u64(&writer, channel_id) ||
        !protocol_writer_append_literal(&writer, "/messages/") ||
        !protocol_writer_append_u64(&writer, message_id) ||
        !protocol_writer_append_literal(&writer, "/ack")) return fail(MessageAckWriteError::capacity);
    return {writer.used, MessageAckWriteError::none};
}

MessageAckWriteResult write_message_ack_body(
    const MessageAckRequest& request, Slice<u8> output) noexcept {
    if (!output.data) return fail(MessageAckWriteError::invalid_input);
    if (request.manual) {
        if (request.token_known || request.last_viewed_known || request.flags_known)
            return fail(MessageAckWriteError::invalid_input);
        const u64 required = sizeof("{\"manual\":true}") - 1 + (request.mention_count_known ?
            sizeof(",\"mention_count\":") - 1 + decimal_digits(request.mention_count) : 0);
        if (required > output.count || required > kMessageAckMaximumBodyBytes)
            return fail(MessageAckWriteError::capacity);
        ProtocolWriter writer{output, 0};
        bool valid = protocol_writer_append_literal(&writer, "{\"manual\":true");
        if (request.mention_count_known) valid = valid &&
            protocol_writer_append_literal(&writer, ",\"mention_count\":") &&
            protocol_writer_append_u64(&writer, request.mention_count);
        valid = valid && protocol_writer_append_literal(&writer, "}");
        return valid ? MessageAckWriteResult{writer.used, MessageAckWriteError::none} :
            fail(MessageAckWriteError::capacity);
    }
    if (request.mention_count_known) return fail(MessageAckWriteError::invalid_input);
    u32 token_bytes = 4; // null
    if (request.token_known && (!utf8_validate(request.token).valid ||
        !protocol_detail::measure_json_string(request.token, &token_bytes))) return fail(MessageAckWriteError::invalid_input);
    const u64 required = static_cast<u64>(sizeof("{\"token\":") - 1) + token_bytes + 1 +
        (request.last_viewed_known ? sizeof(",\"last_viewed\":") - 1 + decimal_digits(request.last_viewed) : 0) +
        (request.flags_known ? sizeof(",\"flags\":") - 1 + decimal_digits(request.flags) : 0);
    if (required > output.count || required > kMessageAckMaximumBodyBytes) return fail(MessageAckWriteError::capacity);
    ProtocolWriter writer{output, 0};
    bool valid = protocol_writer_append_literal(&writer, "{\"token\":");
    valid = valid && (request.token_known ? protocol_writer_append_json_string(&writer, request.token) :
        protocol_writer_append_literal(&writer, "null"));
    if (request.last_viewed_known) valid = valid && protocol_writer_append_literal(&writer, ",\"last_viewed\":") &&
        protocol_writer_append_u64(&writer, request.last_viewed);
    if (request.flags_known) valid = valid && protocol_writer_append_literal(&writer, ",\"flags\":") &&
        protocol_writer_append_u64(&writer, request.flags);
    valid = valid && protocol_writer_append_literal(&writer, "}");
    return valid ? MessageAckWriteResult{writer.used, MessageAckWriteError::none} : fail(MessageAckWriteError::capacity);
}

bool decode_message_ack_response(Utf8View body, Slice<u32> structural_scratch,
    MessageAckResponse* output) noexcept {
    if (!output || !body.data || !body.bytes || body.bytes > kMessageAckMaximumBodyBytes) return false;
    const auto scanned = json_scan_structurals_avx1(body, structural_scratch);
    if (!scanned.ok()) return false;
    MessageAckResponse response{};
    if (!json_visit_root_object(body, {structural_scratch.data, scanned.count}, 64, token_member, &response).ok()) return false;
    *output = response;
    return true;
}
} // namespace maicord
