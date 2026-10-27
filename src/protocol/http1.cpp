#include "maicord/protocol/http1.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {

enum class LineState : u8 {
    complete,
    need_more,
    error,
};

struct LineResult {
    u32 end;
    u32 next;
    u32 error_offset;
    LineState state;
};

bool case_equal(ConstSlice<u8> value, const char* literal, u32 literal_bytes) noexcept {
    if (value.count != literal_bytes)
        return false;
    for (u32 index = 0; index < value.count; ++index)
        if (ascii_lower(value[index]) != ascii_lower(static_cast<u8>(literal[index])))
            return false;
    return true;
}

// HTTP optional whitespace.
bool is_ows(u8 value) noexcept {
    return value == ' ' || value == '\t';
}

bool token_character(u8 value) noexcept {
    if (ascii_is_alnum(value))
        return true;
    switch (value) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
    case '-': case '.': case '^': case '_': case '`': case '|': case '~':
        return true;
    default:
        return false;
    }
}

HttpError validate_field_line(ConstSlice<u8> line, u32* colon_offset) noexcept {
    if (line.count == 0 || line[0] == ' ' || line[0] == '\t')
        return HttpError::invalid_field_name;
    u32 colon = 0;
    while (colon < line.count && line[colon] != ':') {
        if (!token_character(line[colon]))
            return HttpError::invalid_field_name;
        ++colon;
    }
    if (colon == 0 || colon == line.count)
        return HttpError::invalid_field_name;
    if (!http_field_value_valid(ConstSlice<u8>{line.data + colon + 1, line.count - colon - 1}))
        return HttpError::invalid_field_value;
    *colon_offset = colon;
    return HttpError::none;
}

LineResult find_line(ConstSlice<u8> input, u32 begin) noexcept {
    for (u32 offset = begin; offset < input.count; ++offset) {
        if (input[offset] == '\n' && (offset == begin || input[offset - 1] != '\r'))
            return LineResult{0, 0, offset, LineState::error};
        if (input[offset] != '\r')
            continue;
        if (offset + 1 >= input.count)
            return LineResult{0, 0, offset, LineState::need_more};
        if (input[offset + 1] != '\n')
            return LineResult{0, 0, offset, LineState::error};
        return LineResult{offset, offset + 2, 0, LineState::complete};
    }
    return LineResult{0, 0, input.count, LineState::need_more};
}

bool find_complete_head(ConstSlice<u8> input, u32 maximum, u32* header_bytes, u32* error_offset) noexcept {
    const u32 limit = input.count < maximum ? input.count : maximum;
    for (u32 offset = 0; offset < limit; ++offset) {
        if (input[offset] == '\n' && (offset == 0 || input[offset - 1] != '\r')) {
            *error_offset = offset;
            return false;
        }
        if (input[offset] == '\r') {
            if (offset + 1 >= input.count)
                break;
            if (input[offset + 1] != '\n') {
                *error_offset = offset;
                return false;
            }
        }
        if (offset + 3 < input.count && offset + 4 <= maximum && input[offset] == '\r' &&
            input[offset + 1] == '\n' && input[offset + 2] == '\r' && input[offset + 3] == '\n') {
            *header_bytes = offset + 4;
            return true;
        }
    }
    *header_bytes = 0;
    *error_offset = 0xFFFFFFFFu;
    return false;
}

HttpHeadResult head_error(HttpError error, u32 offset) noexcept {
    return HttpHeadResult{{}, offset, error, HttpParseState::error};
}

bool parse_status_line(ConstSlice<u8> line, HttpResponseHead* head, HttpError* error) noexcept {
    if (line.count < 12 || line[0] != 'H' || line[1] != 'T' || line[2] != 'T' || line[3] != 'P' ||
        line[4] != '/' || line[5] < '0' || line[5] > '9' || line[6] != '.' || line[7] < '0' ||
        line[7] > '9' || line[8] != ' ' || line[9] < '0' || line[9] > '9' || line[10] < '0' ||
        line[10] > '9' || line[11] < '0' || line[11] > '9' || (line.count > 12 && line[12] != ' ')) {
        *error = HttpError::invalid_status_line;
        return false;
    }
    head->version_major = static_cast<u8>(line[5] - '0');
    head->version_minor = static_cast<u8>(line[7] - '0');
    if (head->version_major != 1 || head->version_minor > 1) {
        *error = HttpError::unsupported_version;
        return false;
    }
    if (line.count > 13 && !http_field_value_valid(ConstSlice<u8>{line.data + 13, line.count - 13})) {
        *error = HttpError::invalid_status_line;
        return false;
    }
    head->status_code = static_cast<u16>((line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0'));
    return true;
}

bool parse_content_length(ConstSlice<u8> value, u64* parsed) noexcept {
    u32 offset = 0;
    bool have_value = false;
    u64 canonical = 0;
    for (;;) {
        while (offset < value.count && is_ows(value[offset]))
            ++offset;
        if (offset == value.count || value[offset] < '0' || value[offset] > '9')
            return false;
        u64 current = 0;
        while (offset < value.count && value[offset] >= '0' && value[offset] <= '9') {
            const u64 digit = value[offset++] - '0';
            if (current > (~0ull - digit) / 10)
                return false;
            current = current * 10 + digit;
        }
        if (have_value && current != canonical)
            return false;
        canonical = current;
        have_value = true;
        while (offset < value.count && is_ows(value[offset]))
            ++offset;
        if (offset == value.count) {
            *parsed = canonical;
            return true;
        }
        if (value[offset++] != ',')
            return false;
    }
}

bool token_list_contains(ConstSlice<u8> value, const char* expected) noexcept {
    const u32 expected_bytes = c_string_length(expected);
    u32 offset = 0;
    while (offset < value.count) {
        while (offset < value.count && (is_ows(value[offset]) || value[offset] == ','))
            ++offset;
        const u32 begin = offset;
        while (offset < value.count && value[offset] != ',')
            ++offset;
        if (case_equal(http_trim_ows(ConstSlice<u8>{value.data + begin, offset - begin}), expected, expected_bytes))
            return true;
    }
    return false;
}

HttpChunkStep chunk_step(HttpChunkStepState state, u32 consumed) noexcept {
    return HttpChunkStep{{nullptr, 0}, consumed, 0, HttpError::none, state};
}

HttpChunkStep chunk_error(HttpChunkDecoder* decoder, HttpError error, u32 consumed,
                          u32 error_offset) noexcept {
    decoder->state = HttpChunkState::failed;
    return HttpChunkStep{{nullptr, 0}, consumed, error_offset, error, HttpChunkStepState::error};
}

enum class ChunkSizeParse : u8 {
    ok,
    invalid,
    overflow,
};

bool is_quoted_text(u8 value) noexcept {
    return value == '\t' || value == ' ' || value == 0x21 || (value >= 0x23 && value <= 0x5B) ||
           (value >= 0x5D && value <= 0x7E) || value >= 0x80;
}

bool is_quoted_pair_value(u8 value) noexcept {
    return value == '\t' || value == ' ' || (value >= 0x21 && value <= 0x7E) || value >= 0x80;
}

ChunkSizeParse parse_chunk_size(ConstSlice<u8> line, u64* size) noexcept {
    u32 offset = 0;
    u64 result = 0;
    u32 digits = 0;
    const auto skip_ows = [&]() noexcept {
        while (offset < line.count && is_ows(line[offset]))
            ++offset;
    };
    const auto skip_token = [&]() noexcept {
        const u32 begin = offset;
        while (offset < line.count && token_character(line[offset]))
            ++offset;
        return offset != begin;
    };
    while (offset < line.count) {
        const i32 value = ascii_hex_value(static_cast<char>(line[offset]));
        if (value < 0)
            break;
        const u32 digit = static_cast<u32>(value);
        if (result > (~0ull - digit) / 16)
            return ChunkSizeParse::overflow;
        result = result * 16 + digit;
        ++offset;
        ++digits;
    }
    if (digits == 0)
        return ChunkSizeParse::invalid;

    while (offset < line.count) {
        skip_ows();
        if (offset == line.count || line[offset++] != ';')
            return ChunkSizeParse::invalid;
        skip_ows();
        if (!skip_token())
            return ChunkSizeParse::invalid;
        skip_ows();
        if (offset == line.count || line[offset] == ';')
            continue;
        if (line[offset++] != '=')
            return ChunkSizeParse::invalid;
        skip_ows();
        if (offset == line.count)
            return ChunkSizeParse::invalid;

        if (line[offset] == '"') {
            ++offset;
            bool closed = false;
            while (offset < line.count) {
                const u8 current = line[offset++];
                if (current == '"') {
                    closed = true;
                    break;
                }
                if (current == '\\') {
                    if (offset == line.count || !is_quoted_pair_value(line[offset++]))
                        return ChunkSizeParse::invalid;
                } else if (!is_quoted_text(current)) {
                    return ChunkSizeParse::invalid;
                }
            }
            if (!closed)
                return ChunkSizeParse::invalid;
        } else if (!skip_token()) {
            return ChunkSizeParse::invalid;
        }

        skip_ows();
        if (offset < line.count && line[offset] != ';')
            return ChunkSizeParse::invalid;
    }
    *size = result;
    return ChunkSizeParse::ok;
}

} // namespace

bool http_ascii_equal_case_insensitive(ConstSlice<u8> value, const char* literal) noexcept {
    return (value.data || value.count == 0) && case_equal(value, literal, c_string_length(literal));
}

ConstSlice<u8> http_trim_ows(ConstSlice<u8> value) noexcept {
    u32 begin = 0;
    u32 end = value.count;
    while (begin < end && is_ows(value[begin]))
        ++begin;
    while (end > begin && is_ows(value[end - 1]))
        --end;
    return ConstSlice<u8>{value.data + begin, end - begin};
}

Utf8View http_trim_ows(Utf8View value) noexcept {
    const ConstSlice<u8> trimmed = http_trim_ows(ConstSlice<u8>{reinterpret_cast<const u8*>(value.data), value.bytes});
    return {reinterpret_cast<const char*>(trimmed.data), trimmed.count};
}

bool http_field_value_valid(ConstSlice<u8> value) noexcept {
    if (!value.data && value.count != 0)
        return false;
    for (u32 index = 0; index < value.count; ++index)
        if (value[index] != '\t' && (value[index] < 0x20 || value[index] == 0x7F))
            return false;
    return true;
}

HttpRequestError http_parse_request_line(ConstSlice<u8> input, u32 maximum_head_bytes,
                                         HttpRequestHead* head) noexcept {
    u32 head_bytes = 0;
    for (u32 i = 0; i < input.count; ++i) {
        if (input[i] == '\n' && (!i || input[i - 1] != '\r'))
            return HttpRequestError::http;
        if (i + 3 < input.count && input[i] == '\r' && input[i + 1] == '\n' && input[i + 2] == '\r' &&
            input[i + 3] == '\n') {
            head_bytes = i + 4;
            break;
        }
    }
    if (!head_bytes)
        return input.count >= maximum_head_bytes ? HttpRequestError::capacity : HttpRequestError::need_input;
    if (head_bytes > maximum_head_bytes)
        return HttpRequestError::capacity;
    const char* text = reinterpret_cast<const char*>(input.data);
    u32 line_end = 0;
    while (line_end + 1 < head_bytes && !(text[line_end] == '\r' && text[line_end + 1] == '\n'))
        ++line_end;
    u32 method_end = 0;
    while (method_end < line_end && text[method_end] != ' ')
        ++method_end;
    u32 target_end = method_end + 1;
    while (target_end < line_end && text[target_end] != ' ')
        ++target_end;
    if (!method_end || method_end >= line_end || target_end <= method_end + 1 || target_end >= line_end ||
        !utf8_equal({text + target_end + 1, line_end - target_end - 1}, utf8_literal("HTTP/1.1")))
        return HttpRequestError::http;
    *head = {text, head_bytes, line_end + 2, {text, method_end}, {text + method_end + 1, target_end - method_end - 1}};
    return HttpRequestError::none;
}

HttpRequestError http_parse_request_fields(const HttpRequestHead& head, const char* names, u32 wanted,
                                           u32 maximum_body_bytes, HttpRequestFields* fields) noexcept {
    const char* text = head.text;
    u32 cursor = head.fields_begin, count = 0;
    while (cursor < head.head_bytes - 2) {
        const u32 begin = cursor;
        while (cursor + 1 < head.head_bytes && !(text[cursor] == '\r' && text[cursor + 1] == '\n'))
            ++cursor;
        const u32 end = cursor;
        cursor += 2;
        if (++count > 64 || begin == end || text[begin] == ' ' || text[begin] == '\t')
            return HttpRequestError::http;
        u32 colon = begin;
        while (colon < end && text[colon] != ':')
            ++colon;
        if (colon == begin || colon == end)
            return HttpRequestError::http;
        for (u32 i = begin; i < colon; ++i)
            if (!ascii_is_alnum(text[i]) && text[i] != '-')
                return HttpRequestError::http;
        const ConstSlice<u8> raw{reinterpret_cast<const u8*>(text + colon + 1), end - colon - 1};
        if (!http_field_value_valid(raw))
            return HttpRequestError::http;
        const ConstSlice<u8> name{reinterpret_cast<const u8*>(text + begin), colon - begin};
        const ConstSlice<u8> value = http_trim_ows(raw);
        if (case_equal(name, "content-length", 14)) {
            if (fields->has_content_length || !value.count)
                return HttpRequestError::framing;
            fields->has_content_length = true;
            for (u32 i = 0; i < value.count; ++i) {
                if (value[i] < '0' || value[i] > '9')
                    return HttpRequestError::framing;
                fields->content_length = fields->content_length * 10 + (value[i] - '0');
                if (fields->content_length > maximum_body_bytes)
                    return HttpRequestError::capacity;
            }
            continue;
        }
        u32 index = 0;
        const char* candidate = names;
        for (; *candidate; ++index) {
            const u32 bytes = c_string_length(candidate);
            if (case_equal(name, candidate, bytes))
                break;
            candidate += bytes + 1;
        }
        if (!*candidate)
            continue;
        if (index >= wanted || (fields->seen & (1u << index)))
            return HttpRequestError::framing;
        fields->seen |= 1u << index;
        fields->values[index] = {reinterpret_cast<const char*>(value.data), value.count};
    }
    return HttpRequestError::none;
}

HttpHeadResult http_parse_response_head(ConstSlice<u8> input, u32 maximum_header_bytes, u32 maximum_fields,
                                        HttpFieldVisitor visitor, void* context) noexcept {
    if ((!input.data && input.count != 0) || maximum_header_bytes < 4 || maximum_fields == 0)
        return head_error(HttpError::invalid_input, 0);
    u32 header_bytes = 0;
    u32 scan_error = 0;
    if (!find_complete_head(input, maximum_header_bytes, &header_bytes, &scan_error)) {
        if (scan_error != 0xFFFFFFFFu)
            return head_error(HttpError::invalid_line_ending, scan_error);
        if (input.count >= maximum_header_bytes)
            return head_error(HttpError::header_too_large, maximum_header_bytes);
        return HttpHeadResult{{}, 0, HttpError::none, HttpParseState::need_more};
    }

    HttpResponseHead head{};
    head.header_bytes = header_bytes;
    LineResult line = find_line(ConstSlice<u8>{input.data, header_bytes}, 0);
    if (line.state != LineState::complete)
        return head_error(HttpError::invalid_status_line, 0);
    HttpError status_error = HttpError::none;
    if (!parse_status_line(ConstSlice<u8>{input.data, line.end}, &head, &status_error))
        return head_error(status_error, 0);

    bool transfer_encoding_seen = false;
    u32 offset = line.next;
    while (offset + 2 <= header_bytes) {
        line = find_line(ConstSlice<u8>{input.data, header_bytes}, offset);
        if (line.state != LineState::complete)
            return head_error(HttpError::invalid_line_ending, offset);
        if (line.end == offset) {
            offset = line.next;
            break;
        }
        if (input[offset] == ' ' || input[offset] == '\t')
            return head_error(HttpError::obsolete_line_folding, offset);
        if (head.field_count >= maximum_fields)
            return head_error(HttpError::too_many_fields, offset);

        const ConstSlice<u8> raw_line{input.data + offset, line.end - offset};
        u32 colon = 0;
        const HttpError field_error = validate_field_line(raw_line, &colon);
        if (field_error != HttpError::none)
            return head_error(field_error, offset);
        const HttpField field{
            ConstSlice<u8>{raw_line.data, colon},
            http_trim_ows(ConstSlice<u8>{raw_line.data + colon + 1, raw_line.count - colon - 1}), offset};
        ++head.field_count;

        if (case_equal(field.name, "content-length", 14)) {
            u64 content_length = 0;
            if (!parse_content_length(field.value, &content_length))
                return head_error(HttpError::invalid_content_length, offset + colon + 1);
            if (head.has_content_length && head.content_length != content_length)
                return head_error(HttpError::conflicting_content_length, offset);
            head.has_content_length = true;
            head.content_length = content_length;
        } else if (case_equal(field.name, "transfer-encoding", 17)) {
            if (transfer_encoding_seen || !case_equal(http_trim_ows(field.value), "chunked", 7))
                return head_error(HttpError::invalid_transfer_encoding, offset);
            transfer_encoding_seen = true;
            head.chunked = true;
        } else if (case_equal(field.name, "connection", 10)) {
            head.connection_close = head.connection_close || token_list_contains(field.value, "close");
            head.connection_upgrade = head.connection_upgrade || token_list_contains(field.value, "upgrade");
        } else if (case_equal(field.name, "upgrade", 7)) {
            head.upgrade_websocket = head.upgrade_websocket || token_list_contains(field.value, "websocket");
        }
        if (visitor && !visitor(context, field))
            return head_error(HttpError::visitor_rejected, offset);
        offset = line.next;
    }

    if (offset != header_bytes)
        return head_error(HttpError::invalid_line_ending, offset);
    if (head.chunked && head.has_content_length)
        return head_error(HttpError::ambiguous_framing, header_bytes);
    return HttpHeadResult{head, 0, HttpError::none, HttpParseState::complete};
}

HttpBodyKind http_response_body_kind(const HttpResponseHead& head, bool request_was_head,
                                     bool request_was_connect) noexcept {
    if (request_was_head || (head.status_code >= 100 && head.status_code < 200) || head.status_code == 204 ||
        head.status_code == 304)
        return head.status_code == 101 ? HttpBodyKind::tunnel : HttpBodyKind::none;
    if (request_was_connect && head.status_code >= 200 && head.status_code < 300)
        return HttpBodyKind::tunnel;
    if (head.chunked)
        return HttpBodyKind::chunked;
    if (head.has_content_length)
        return head.content_length == 0 ? HttpBodyKind::none : HttpBodyKind::content_length;
    return HttpBodyKind::until_close;
}

bool http_chunk_decoder_initialize(HttpChunkDecoder* decoder, u64 maximum_body_bytes, u32 maximum_line_bytes,
                                   u32 maximum_trailer_bytes, u32 maximum_trailers) noexcept {
    if (maximum_body_bytes == 0 || maximum_line_bytes < 2 || maximum_trailer_bytes < 2 || maximum_trailers == 0)
        return false;
    *decoder = HttpChunkDecoder{0, 0, maximum_body_bytes, 0, maximum_line_bytes, maximum_trailer_bytes, 0,
                                maximum_trailers, HttpChunkState::size};
    return true;
}

HttpChunkStep http_chunk_decode(HttpChunkDecoder* decoder, ConstSlice<u8> input) noexcept {
    if ((!input.data && input.count != 0) || decoder->state == HttpChunkState::failed) {
        decoder->state = HttpChunkState::failed;
        return HttpChunkStep{{nullptr, 0}, 0, 0, HttpError::invalid_input, HttpChunkStepState::error};
    }
    if (decoder->state == HttpChunkState::complete)
        return chunk_step(HttpChunkStepState::complete, 0);

    u32 offset = 0;
    for (;;) {
        if (decoder->state == HttpChunkState::size) {
            const LineResult line = find_line(input, offset);
            if (line.state == LineState::error)
                return chunk_error(decoder, HttpError::invalid_line_ending, offset, line.error_offset);
            if (line.state == LineState::need_more) {
                if (input.count - offset > decoder->maximum_line_bytes)
                    return chunk_error(decoder, HttpError::invalid_chunk_size, offset, offset);
                return chunk_step(HttpChunkStepState::need_more, offset);
            }
            if (line.end - offset > decoder->maximum_line_bytes)
                return chunk_error(decoder, HttpError::invalid_chunk_size, offset, offset);
            u64 chunk_size = 0;
            const ChunkSizeParse size_parse =
                parse_chunk_size(ConstSlice<u8>{input.data + offset, line.end - offset}, &chunk_size);
            if (size_parse == ChunkSizeParse::overflow)
                return chunk_error(decoder, HttpError::chunk_size_overflow, offset, offset);
            if (size_parse != ChunkSizeParse::ok)
                return chunk_error(decoder, HttpError::invalid_chunk_size, offset, offset);
            if (decoder->body_bytes > decoder->maximum_body_bytes ||
                chunk_size > decoder->maximum_body_bytes - decoder->body_bytes)
                return chunk_error(decoder, HttpError::chunk_too_large, offset, offset);
            decoder->chunk_remaining = chunk_size;
            decoder->body_bytes += chunk_size;
            offset = line.next;
            decoder->state = chunk_size == 0 ? HttpChunkState::trailers : HttpChunkState::data;
            continue;
        }

        if (decoder->state == HttpChunkState::data) {
            const u32 available = input.count - offset;
            if (available == 0)
                return chunk_step(HttpChunkStepState::need_more, offset);
            const u32 bytes =
                decoder->chunk_remaining < available ? static_cast<u32>(decoder->chunk_remaining) : available;
            decoder->chunk_remaining -= bytes;
            if (decoder->chunk_remaining == 0)
                decoder->state = HttpChunkState::data_terminator;
            return HttpChunkStep{ConstSlice<u8>{input.data + offset, bytes}, offset + bytes, 0, HttpError::none,
                                 HttpChunkStepState::data};
        }

        if (decoder->state == HttpChunkState::data_terminator) {
            if (input.count - offset < 2) {
                if (input.count != offset && input[offset] != '\r')
                    return chunk_error(decoder, HttpError::invalid_chunk_terminator, offset, offset);
                return chunk_step(HttpChunkStepState::need_more, offset);
            }
            if (input[offset] != '\r' || input[offset + 1] != '\n')
                return chunk_error(decoder, HttpError::invalid_chunk_terminator, offset, offset);
            offset += 2;
            decoder->state = HttpChunkState::size;
            continue;
        }

        if (decoder->state == HttpChunkState::trailers) {
            const LineResult line = find_line(input, offset);
            if (line.state == LineState::error)
                return chunk_error(decoder, HttpError::invalid_line_ending, offset, line.error_offset);
            if (line.state == LineState::need_more) {
                if (decoder->trailer_bytes + input.count - offset > decoder->maximum_trailer_bytes ||
                    input.count - offset > decoder->maximum_line_bytes)
                    return chunk_error(decoder, HttpError::trailer_too_large, offset, offset);
                return chunk_step(HttpChunkStepState::need_more, offset);
            }
            const u32 line_bytes = line.next - offset;
            if (line_bytes > decoder->maximum_trailer_bytes ||
                decoder->trailer_bytes > decoder->maximum_trailer_bytes - line_bytes)
                return chunk_error(decoder, HttpError::trailer_too_large, offset, offset);
            decoder->trailer_bytes += line_bytes;
            if (line.end == offset) {
                offset = line.next;
                decoder->state = HttpChunkState::complete;
                return chunk_step(HttpChunkStepState::complete, offset);
            }
            if (decoder->trailer_count >= decoder->maximum_trailers)
                return chunk_error(decoder, HttpError::too_many_trailers, offset, offset);
            u32 colon = 0;
            if (validate_field_line(ConstSlice<u8>{input.data + offset, line.end - offset}, &colon) != HttpError::none)
                return chunk_error(decoder, HttpError::invalid_trailer, offset, offset);
            ++decoder->trailer_count;
            offset = line.next;
            continue;
        }
    }
}

namespace {

HttpResponseDecoderStep decoder_step(const HttpResponseDecoder& decoder, u32 accepted) noexcept {
    return HttpResponseDecoderStep{accepted, decoder.body_bytes, byte_queue_size(decoder.input),
                                   decoder.state, decoder.error};
}

HttpResponseDecoderStep decoder_failure(HttpResponseDecoder* decoder, HttpResponseDecoderError error,
                                        u32 accepted) noexcept {
    decoder->state = HttpResponseDecoderState::failed;
    decoder->error = error;
    decoder->connection_reusable = false;
    return decoder_step(*decoder, accepted);
}

bool decoder_copy_body(HttpResponseDecoder* decoder, ConstSlice<u8> bytes) noexcept {
    if ((!bytes.data && bytes.count != 0) || decoder->body_bytes > decoder->body.count ||
        bytes.count > decoder->body.count - decoder->body_bytes)
        return false;
    copy_bytes(decoder->body.data + decoder->body_bytes, bytes.data, bytes.count);
    decoder->body_bytes += bytes.count;
    return true;
}

bool decoder_connection_reusable(const HttpResponseHead& head) noexcept {
    return head.version_major == 1 && head.version_minor >= 1 && !head.connection_close;
}

HttpHeadResult decoder_parse_head(HttpResponseDecoder* decoder, ConstSlice<u8> input) noexcept {
    return decoder->head_parser
               ? decoder->head_parser(decoder->head_parser_context, input, decoder->maximum_header_bytes,
                                      decoder->maximum_fields)
               : http_parse_response_head(input, decoder->maximum_header_bytes, decoder->maximum_fields,
                                          nullptr, nullptr);
}

bool retain_final_head(HttpResponseDecoder* decoder, ConstSlice<u8> head) noexcept {
    if (!decoder->retained_head.data && decoder->retained_head.count == 0) {
        decoder->final_head_bytes = 0;
        return true;
    }
    if (!decoder->retained_head.data || head.count > decoder->retained_head.count)
        return false;
    copy_bytes(decoder->retained_head.data, head.data, head.count);
    decoder->final_head_bytes = head.count;
    return true;
}

} // namespace

bool http_response_decoder_initialize(HttpResponseDecoder* decoder, Slice<u8> input_storage,
                                      Slice<u8> retained_head, Slice<u8> body_output, u32 maximum_header_bytes,
                                      u32 maximum_fields, bool request_was_head, HttpResponseHeadParser head_parser,
                                      void* head_parser_context) noexcept {
    if (!input_storage.data || maximum_header_bytes < 4 || input_storage.count < maximum_header_bytes ||
        maximum_fields == 0 || (!retained_head.data && retained_head.count != 0) ||
        (retained_head.data && retained_head.count < maximum_header_bytes) ||
        (!body_output.data && body_output.count != 0) || (!head_parser && head_parser_context))
        return false;
    reset_value<HttpResponseDecoder>(decoder);
    decoder->body = body_output;
    decoder->retained_head = retained_head;
    decoder->head_parser = head_parser;
    decoder->head_parser_context = head_parser_context;
    decoder->maximum_header_bytes = maximum_header_bytes;
    decoder->maximum_fields = maximum_fields;
    decoder->request_was_head = request_was_head;
    decoder->state = HttpResponseDecoderState::head;
    return byte_queue_initialize(&decoder->input, input_storage);
}

HttpResponseDecoderStep http_response_decoder_feed(HttpResponseDecoder* decoder, ConstSlice<u8> input,
                                                   bool peer_closed) noexcept {
    if ((!input.data && input.count != 0) || decoder->state == HttpResponseDecoderState::failed)
        return decoder_failure(decoder, HttpResponseDecoderError::invalid_input, 0);
    if (decoder->state == HttpResponseDecoderState::complete) {
        if (input.count != 0)
            return decoder_failure(decoder, HttpResponseDecoderError::unexpected_data, 0);
        if (peer_closed)
            decoder->connection_reusable = false;
        return decoder_step(*decoder, 0);
    }
    if (!byte_queue_append(&decoder->input, input))
        return decoder_failure(decoder, HttpResponseDecoderError::input_capacity, 0);
    const u32 accepted = input.count;
    // More input is needed: a peer that closed first leaves the response truncated.
    const auto wait = [&]() noexcept {
        return peer_closed ? decoder_failure(decoder, HttpResponseDecoderError::unexpected_close, accepted)
                           : decoder_step(*decoder, accepted);
    };

    for (;;) {
        HttpBodyKind body_kind;
        if (decoder->state == HttpResponseDecoderState::head) {
            const ConstSlice<u8> readable = byte_queue_readable(decoder->input);
            const HttpHeadResult parsed = decoder_parse_head(decoder, readable);
            if (parsed.state == HttpParseState::need_more)
                return wait();
            if (!parsed.ok())
                return decoder_failure(decoder, HttpResponseDecoderError::head, accepted);
            if (parsed.head.status_code >= 100 && parsed.head.status_code < 200 && parsed.head.status_code != 101) {
                if (!byte_queue_consume(&decoder->input, parsed.head.header_bytes) ||
                    ++decoder->informational_responses > 8)
                    return decoder_failure(decoder, HttpResponseDecoderError::head, accepted);
                continue;
            }
            if (!retain_final_head(decoder, ConstSlice<u8>{readable.data, parsed.head.header_bytes}))
                return decoder_failure(decoder, HttpResponseDecoderError::head_capacity, accepted);
            if (!byte_queue_consume(&decoder->input, parsed.head.header_bytes))
                return decoder_failure(decoder, HttpResponseDecoderError::body_framing, accepted);
            decoder->parsed_head = parsed.head;
            decoder->connection_reusable = decoder_connection_reusable(parsed.head);
            body_kind = http_response_body_kind(parsed.head, decoder->request_was_head, false);
            if (body_kind == HttpBodyKind::tunnel)
                return decoder_failure(decoder, HttpResponseDecoderError::unsupported_tunnel, accepted);
            if (body_kind == HttpBodyKind::none) {
                decoder->state = HttpResponseDecoderState::complete;
                return decoder_step(*decoder, accepted);
            }
            if (body_kind == HttpBodyKind::content_length) {
                if (parsed.head.content_length > decoder->body.count)
                    return decoder_failure(decoder, HttpResponseDecoderError::body_capacity, accepted);
                decoder->expected_body_bytes = parsed.head.content_length;
            } else if (body_kind == HttpBodyKind::chunked) {
                if (!http_chunk_decoder_initialize(&decoder->chunk_decoder, decoder->body.count, 8 * 1024,
                                                   decoder->maximum_header_bytes, decoder->maximum_fields))
                    return decoder_failure(decoder, HttpResponseDecoderError::body_framing, accepted);
            } else {
                decoder->connection_reusable = false;
            }
            decoder->state = HttpResponseDecoderState::body;
        } else {
            body_kind = http_response_body_kind(decoder->parsed_head, decoder->request_was_head, false);
        }

        if (body_kind == HttpBodyKind::content_length) {
            const u64 remaining = decoder->expected_body_bytes - decoder->body_bytes;
            const ConstSlice<u8> readable = byte_queue_readable(decoder->input);
            const u32 copied = static_cast<u32>(minimum<u64>(remaining, static_cast<u64>(readable.count)));
            if (!decoder_copy_body(decoder, ConstSlice<u8>{readable.data, copied}) ||
                !byte_queue_consume(&decoder->input, copied))
                return decoder_failure(decoder, HttpResponseDecoderError::body_capacity, accepted);
            if (decoder->body_bytes == decoder->expected_body_bytes) {
                decoder->state = HttpResponseDecoderState::complete;
                return decoder_step(*decoder, accepted);
            }
            return wait();
        }
        if (body_kind == HttpBodyKind::chunked) {
            for (;;) {
                const HttpChunkStep chunk =
                    http_chunk_decode(&decoder->chunk_decoder, byte_queue_readable(decoder->input));
                if (!chunk.ok())
                    return decoder_failure(decoder, HttpResponseDecoderError::body_framing, accepted);
                if (chunk.state == HttpChunkStepState::data) {
                    if (!decoder_copy_body(decoder, chunk.data) ||
                        !byte_queue_consume(&decoder->input, chunk.consumed))
                        return decoder_failure(decoder, HttpResponseDecoderError::body_capacity, accepted);
                    continue;
                }
                if (!byte_queue_consume(&decoder->input, chunk.consumed))
                    return decoder_failure(decoder, HttpResponseDecoderError::body_framing, accepted);
                if (chunk.state == HttpChunkStepState::complete) {
                    decoder->state = HttpResponseDecoderState::complete;
                    return decoder_step(*decoder, accepted);
                }
                return wait();
            }
        }

        const ConstSlice<u8> readable = byte_queue_readable(decoder->input);
        if (!decoder_copy_body(decoder, readable) || !byte_queue_consume(&decoder->input, readable.count))
            return decoder_failure(decoder, HttpResponseDecoderError::body_capacity, accepted);
        if (peer_closed)
            decoder->state = HttpResponseDecoderState::complete;
        return decoder_step(*decoder, accepted);
    }
}

} // namespace maicord
