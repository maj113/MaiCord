#include "maicord/protocol/message_ack.hpp"

namespace maicord {
namespace {
bool bytes_equal(const u8* output, u32 bytes, Utf8View expected) noexcept {
    return bytes == expected.bytes && utf8_equal({reinterpret_cast<const char*>(output), bytes}, expected);
}
bool sentinel(const u8* output, u32 bytes) noexcept {
    for (u32 i = 0; i < bytes; ++i) if (output[i] != 0xA5) return false;
    return true;
}
void fill(u8* output, u32 bytes) noexcept { for (u32 i = 0; i < bytes; ++i) output[i] = 0xA5; }
bool unchanged(const MessageAckResponse& a, const MessageAckResponse& b) noexcept {
    return a.token.encoded.data == b.token.encoded.data && a.token.encoded.bytes == b.token.encoded.bytes &&
        a.token.source_offset == b.token.source_offset && a.token.has_escapes == b.token.has_escapes &&
        a.token_known == b.token_known && a.token_null == b.token_null;
}
// Large-bound checks use static backing, not a large stack frame.
u8 large_output[kMessageAckMaximumBodyBytes]{};
char large_token[kMessageAckMaximumBodyBytes]{};
}

void test_message_ack(void (*check)(bool, const char*) noexcept) noexcept {
    u8 output[128]{};
    auto written = write_message_ack_target(1, 2, {output, 128});
    check(written.ok() && bytes_equal(output, written.bytes, utf8_literal("/api/v10/channels/1/messages/2/ack")), "ack_target_exact");
    written = write_message_ack_target(~0ull, ~0ull, {output, 128});
    check(written.ok() && bytes_equal(output, written.bytes,
        utf8_literal("/api/v10/channels/18446744073709551615/messages/18446744073709551615/ack")), "ack_target_max_snowflakes");
    const u32 target_bytes = written.bytes;
    check(write_message_ack_target(~0ull, ~0ull, {output, target_bytes}).ok(), "ack_target_exact_fit");
    for (u32 capacity = 0; capacity < target_bytes; ++capacity) {
        fill(output, 128);
        const auto failed = write_message_ack_target(~0ull, ~0ull, {output, capacity});
        check(failed.error == MessageAckWriteError::capacity && !failed.bytes && sentinel(output, 128), "ack_target_capacity_atomic");
    }
    fill(output, 128);
    check(!write_message_ack_target(0, 2, {output, 128}).ok() &&
        sentinel(output, 128), "ack_target_zero_channel_atomic");
    written = write_message_ack_target(1, 0, {output, 128});
    check(written.ok() && bytes_equal(output, written.bytes,
        utf8_literal("/api/v10/channels/1/messages/0/ack")), "ack_target_zero_boundary_exact");

    MessageAckRequest request{};
    written = write_message_ack_body(request, {output, 128});
    check(written.ok() && bytes_equal(output, written.bytes, utf8_literal("{\"token\":null}")), "ack_body_null_exact");
    request.token = {nullptr, 99}; // Unknown token bytes are deliberately ignored.
    written = write_message_ack_body(request, {output, 128});
    check(written.ok() && bytes_equal(output, written.bytes, utf8_literal("{\"token\":null}")), "ack_body_unknown_token_ignored");
    request.token = {};
    request.token_known = true;
    written = write_message_ack_body(request, {output, 128});
    check(written.ok() && bytes_equal(output, written.bytes, utf8_literal("{\"token\":\"\"}")), "ack_body_empty_token_legal");
    request.token = utf8_literal("a\"\\\n\t\x01");
    request.last_viewed = ~0u; request.flags = ~0u;
    request.last_viewed_known = request.flags_known = true;
    written = write_message_ack_body(request, {output, 128});
    check(written.ok() && bytes_equal(output, written.bytes,
        utf8_literal("{\"token\":\"a\\\"\\\\\\n\\t\\u0001\",\"last_viewed\":4294967295,\"flags\":4294967295}")), "ack_body_escaped_token_u32_max");
    const u32 body_bytes = written.bytes;
    check(write_message_ack_body(request, {output, body_bytes}).ok(), "ack_body_exact_fit");
    for (u32 capacity = 0; capacity < body_bytes; ++capacity) {
        fill(output, 128);
        const auto failed = write_message_ack_body(request, {output, capacity});
        check(failed.error == MessageAckWriteError::capacity && !failed.bytes && sentinel(output, 128), "ack_body_capacity_atomic");
    }
    request.last_viewed_known = request.flags_known = false;
    request.token = utf8_literal("\xC5\xBE");
    written = write_message_ack_body(request, {output, 128});
    check(written.ok() && bytes_equal(output, written.bytes, utf8_literal("{\"token\":\"\xC5\xBE\"}")), "ack_body_utf8_exact");
    request.last_viewed = 0; request.flags = 0;
    request.last_viewed_known = request.flags_known = true;
    written = write_message_ack_body(request, {output, 128});
    check(written.ok() && bytes_equal(output, written.bytes,
        utf8_literal("{\"token\":\"\xC5\xBE\",\"last_viewed\":0,\"flags\":0}")), "ack_body_known_zero_fields");
    const char invalid_utf8[] = {static_cast<char>(0xC0), static_cast<char>(0xAF)};
    request.token = {invalid_utf8, 2};
    fill(output, 128);
    check(write_message_ack_body(request, {output, 128}).error == MessageAckWriteError::invalid_input &&
        sentinel(output, 128), "ack_body_invalid_utf8_atomic");

    request = {};
    request.manual = true;
    request.token = {nullptr, ~0u}; // Omitted ordinary bytes are never inspected.
    request.last_viewed = request.flags = request.mention_count = ~0u;
    written = write_message_ack_body(request, {output, 128});
    check(written.ok() && bytes_equal(output, written.bytes, utf8_literal("{\"manual\":true}")),
        "ack_manual_unknown_count_ordinary_values_omitted");
    request.mention_count_known = true;
    request.mention_count = 0;
    written = write_message_ack_body(request, {output, 128});
    check(written.ok() && bytes_equal(output, written.bytes, utf8_literal("{\"manual\":true,\"mention_count\":0}")),
        "ack_manual_known_zero_count_exact");
    request.mention_count = ~0u;
    written = write_message_ack_body(request, {output, 128});
    check(written.ok() && bytes_equal(output, written.bytes,
        utf8_literal("{\"manual\":true,\"mention_count\":4294967295}")), "ack_manual_max_count_exact");
    const u32 manual_bytes = written.bytes;
    fill(output, 128);
    written = write_message_ack_body(request, {output, manual_bytes});
    check(written.ok() && written.bytes == manual_bytes && sentinel(output + manual_bytes, 128 - manual_bytes),
        "ack_manual_exact_fit_suffix_unchanged");
    for (u32 capacity = 0; capacity < manual_bytes; ++capacity) {
        fill(output, 128);
        const auto failed = write_message_ack_body(request, {output, capacity});
        check(failed.error == MessageAckWriteError::capacity && !failed.bytes && sentinel(output, 128),
            "ack_manual_capacity_atomic");
    }
    for (u32 ordinary_member = 0; ordinary_member < 3; ++ordinary_member) {
        request.token_known = ordinary_member == 0;
        request.last_viewed_known = ordinary_member == 1;
        request.flags_known = ordinary_member == 2;
        fill(output, 128);
        check(write_message_ack_body(request, {output, 128}).error == MessageAckWriteError::invalid_input &&
            sentinel(output, 128), "ack_manual_mixed_ordinary_metadata_atomic");
    }
    request = {};
    request.mention_count_known = true;
    fill(output, 128);
    check(write_message_ack_body(request, {output, 128}).error == MessageAckWriteError::invalid_input &&
        sentinel(output, 128), "ack_ordinary_mention_count_invalid_atomic");
    request.mention_count_known = false;
    request.mention_count = ~0u;
    written = write_message_ack_body(request, {output, 128});
    check(written.ok() && bytes_equal(output, written.bytes, utf8_literal("{\"token\":null}")),
        "ack_ordinary_unknown_count_default_unchanged");

    for (u32 i = 0; i < kMessageAckMaximumBodyBytes; ++i) large_token[i] = 'x';
    request = {};
    request.token_known = true;
    request.token = {large_token, kMessageAckMaximumBodyBytes - 12};
    written = write_message_ack_body(request, {large_output, kMessageAckMaximumBodyBytes});
    check(written.ok() && written.bytes == kMessageAckMaximumBodyBytes && large_output[0] == '{' &&
        large_output[written.bytes - 1] == '}', "ack_body_full_64k_caller_buffer");
    fill(large_output, kMessageAckMaximumBodyBytes);
    ++request.token.bytes;
    check(write_message_ack_body(request, {large_output, kMessageAckMaximumBodyBytes}).error == MessageAckWriteError::capacity &&
        sentinel(large_output, kMessageAckMaximumBodyBytes), "ack_body_over_64k_atomic");
    request.token = {large_token, kMessageAckMaximumBodyBytes / 6};
    for (u32 i = 0; i < request.token.bytes; ++i) large_token[i] = 1;
    check(write_message_ack_body(request, {large_output, kMessageAckMaximumBodyBytes}).error == MessageAckWriteError::capacity &&
        sentinel(large_output, kMessageAckMaximumBodyBytes), "ack_body_escaped_encoded_bound_atomic");

    u32 structurals[128]{};
    MessageAckResponse response{};
    check(decode_message_ack_response(utf8_literal("{}"), {structurals, 128}, &response) &&
        !response.token_known && !response.token_null, "ack_response_missing_token");
    check(decode_message_ack_response(utf8_literal("{\"token\":null}"), {structurals, 128}, &response) &&
        response.token_known && response.token_null, "ack_response_null_token");
    check(decode_message_ack_response(utf8_literal("{\"token\":\"\"}"), {structurals, 128}, &response) &&
        response.token_known && !response.token_null && response.token.encoded.bytes == 0, "ack_response_empty_token");
    check(decode_message_ack_response(utf8_literal("{\"token\":\"a\\n\\u017E\",\"extra\":{\"x\":[true,null]}}"),
        {structurals, 128}, &response) && response.token_known && !response.token_null && response.token.has_escapes, "ack_response_escaped_token_unknown_fields");
    char decoded[16]{};
    const auto decode = json_string_decode_utf8(response.token, {decoded, 16});
    check(decode.ok() && utf8_equal({decoded, decode.bytes}, utf8_literal("a\n\xC5\xBE")), "ack_response_token_decode");
    check(decode_message_ack_response(utf8_literal("{\"token\":\"\xC5\xBE\"}"), {structurals, 128}, &response) &&
        utf8_equal(response.token.encoded, utf8_literal("\xC5\xBE")), "ack_response_direct_utf8");
    const auto previous = response;
    const Utf8View malformed[] = {utf8_literal("[]"), utf8_literal("null"), utf8_literal("{\"token\":true}"),
        utf8_literal("{\"token\":42}"), utf8_literal("{\"token\":{}}"),
        utf8_literal("{\"token\":\"a\",\"token\":null}"), utf8_literal("{\"token\":null,\"to\\u006ben\":\"b\"}"),
        utf8_literal("{\"token\":\"a\",\"unknown\":tru}"), utf8_literal("{\"token\":\"\\uD800\"}"),
        utf8_literal("{\"token\":\"a\"} trailing"), utf8_literal("{\"token\":\"a\"")};
    for (const auto body : malformed)
        check(!decode_message_ack_response(body, {structurals, 128}, &response) && unchanged(previous, response), "ack_response_malformed_atomic");
    const char invalid_json[] = {'{', '"', 'x', '"', ':', '"', static_cast<char>(0xC0), static_cast<char>(0xAF), '"', '}'};
    check(!decode_message_ack_response({invalid_json, sizeof(invalid_json)}, {structurals, 128}, &response) &&
        unchanged(previous, response), "ack_response_unknown_invalid_utf8_atomic");
    check(!decode_message_ack_response(utf8_literal("{\"token\":\"a\"}"), {structurals, 1}, &response) &&
        unchanged(previous, response), "ack_response_scratch_capacity_atomic");
}
} // namespace maicord
