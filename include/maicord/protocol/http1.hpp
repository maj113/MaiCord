#pragma once

#include "maicord/core/byte_queue.hpp"

namespace maicord {

enum class HttpError : u8 {
    none,
    invalid_input,
    header_too_large,
    too_many_fields,
    invalid_line_ending,
    invalid_status_line,
    unsupported_version,
    obsolete_line_folding,
    invalid_field_name,
    invalid_field_value,
    visitor_rejected,
    invalid_content_length,
    conflicting_content_length,
    invalid_transfer_encoding,
    ambiguous_framing,
    invalid_chunk_size,
    chunk_size_overflow,
    chunk_too_large,
    invalid_chunk_terminator,
    trailer_too_large,
    too_many_trailers,
    invalid_trailer,
};

enum class HttpParseState : u8 {
    complete,
    need_more,
    error,
};

struct HttpField {
    ConstSlice<u8> name;
    ConstSlice<u8> value;
    u32 source_offset;
};

using HttpFieldVisitor = bool (*)(void* context, HttpField field) noexcept;

struct HttpResponseHead {
    u64 content_length;
    u32 header_bytes;
    u32 field_count;
    u16 status_code;
    u8 version_major;
    u8 version_minor;
    bool has_content_length;
    bool chunked;
    bool connection_close;
    bool connection_upgrade;
    bool upgrade_websocket;
};

struct HttpHeadResult {
    HttpResponseHead head;
    u32 error_offset;
    HttpError error;
    HttpParseState state;

    constexpr bool ok() const noexcept {
        return state == HttpParseState::complete;
    }
};

enum class HttpBodyKind : u8 {
    none,
    content_length,
    chunked,
    until_close,
    tunnel,
};

HttpHeadResult http_parse_response_head(ConstSlice<u8> input, u32 maximum_header_bytes, u32 maximum_fields,
                                        HttpFieldVisitor visitor, void* context) noexcept;
HttpBodyKind http_response_body_kind(const HttpResponseHead& head, bool request_was_head,
                                     bool request_was_connect) noexcept;
bool http_ascii_equal_case_insensitive(ConstSlice<u8> value, const char* literal) noexcept;
// Strips optional whitespace (spaces and tabs) from both ends of a field value.
ConstSlice<u8> http_trim_ows(ConstSlice<u8> value) noexcept;
Utf8View http_trim_ows(Utf8View value) noexcept;
// True when value holds no control characters other than tab.
bool http_field_value_valid(ConstSlice<u8> value) noexcept;

// Server side of a strict HTTP/1.1 request head: CRLF-only lines, one
// "METHOD target HTTP/1.1" request line and unfolded fields. The result
// codes are numerically the capacity/http/framing members of the loopback
// servers' error enums.
enum class HttpRequestError : u8 { none, need_input, capacity, http, framing };
struct HttpRequestHead {
    const char* text;
    u32 head_bytes, fields_begin;
    Utf8View method, target;
};
// Waits (need_input) for a complete head; capacity when the head would exceed
// maximum_head_bytes.
HttpRequestError http_parse_request_line(ConstSlice<u8> input, u32 maximum_head_bytes,
                                         HttpRequestHead* head) noexcept;
struct HttpRequestFields {
    Utf8View values[8];  // trimmed value of the i-th wanted name
    u32 seen;            // bit i: that name was present
    u32 content_length;
    bool has_content_length;
};
// names holds NUL-terminated lowercase field names: the first `wanted` are
// recorded (a repeat is framing), the rest are forbidden (framing). Names
// end with an extra NUL; at most 64 fields; a Content-Length above
// maximum_body_bytes is capacity. Zero `fields` first.
HttpRequestError http_parse_request_fields(const HttpRequestHead& head, const char* names, u32 wanted,
                                           u32 maximum_body_bytes, HttpRequestFields* fields) noexcept;

enum class HttpChunkState : u8 {
    size,
    data,
    data_terminator,
    trailers,
    complete,
    failed,
};

struct HttpChunkDecoder {
    u64 chunk_remaining;
    u64 body_bytes;
    u64 maximum_body_bytes;
    u32 trailer_bytes;
    u32 maximum_line_bytes;
    u32 maximum_trailer_bytes;
    u32 trailer_count;
    u32 maximum_trailers;
    HttpChunkState state;
};

enum class HttpChunkStepState : u8 {
    data,
    need_more,
    complete,
    error,
};

struct HttpChunkStep {
    ConstSlice<u8> data;
    u32 consumed;
    u32 error_offset;
    HttpError error;
    HttpChunkStepState state;

    constexpr bool ok() const noexcept {
        return state != HttpChunkStepState::error;
    }
};

bool http_chunk_decoder_initialize(HttpChunkDecoder* decoder, u64 maximum_body_bytes,
                                   u32 maximum_line_bytes, u32 maximum_trailer_bytes,
                                   u32 maximum_trailers) noexcept;
HttpChunkStep http_chunk_decode(HttpChunkDecoder* decoder, ConstSlice<u8> input) noexcept;

enum class HttpResponseDecoderState : u8 {
    head,
    body,
    complete,
    failed,
};

enum class HttpResponseDecoderError : u8 {
    none,
    invalid_input,
    input_capacity,
    head,
    head_capacity,
    unsupported_tunnel,
    body_capacity,
    body_framing,
    unexpected_close,
    unexpected_data,
};

using HttpResponseHeadParser = HttpHeadResult (*)(
    void* context, ConstSlice<u8> input, u32 maximum_header_bytes,
    u32 maximum_fields) noexcept;

struct HttpResponseDecoder {
    ByteQueue input;
    Slice<u8> body;
    Slice<u8> retained_head;
    HttpResponseHead parsed_head;
    HttpChunkDecoder chunk_decoder;
    HttpResponseHeadParser head_parser;
    void* head_parser_context;
    u64 expected_body_bytes;
    u32 body_bytes;
    u32 final_head_bytes;
    u32 informational_responses;
    u32 maximum_header_bytes;
    u32 maximum_fields;
    HttpResponseDecoderState state;
    HttpResponseDecoderError error;
    bool request_was_head;
    bool connection_reusable;
};

struct HttpResponseDecoderStep {
    u32 accepted;
    u32 body_bytes;
    u32 extra_bytes;
    HttpResponseDecoderState state;
    HttpResponseDecoderError error;

    constexpr bool ok() const noexcept {
        return error == HttpResponseDecoderError::none;
    }
};

// The optional retained-head span must be empty or at least
// maximum_header_bytes. It receives only the final non-informational head;
// interim 1xx heads never overwrite it.
bool http_response_decoder_initialize(
    HttpResponseDecoder* decoder, Slice<u8> input_storage,
    Slice<u8> retained_head, Slice<u8> body_output,
    u32 maximum_header_bytes, u32 maximum_fields,
    bool request_was_head = false,
    HttpResponseHeadParser head_parser = nullptr,
    void* head_parser_context = nullptr) noexcept;

HttpResponseDecoderStep http_response_decoder_feed(
    HttpResponseDecoder* decoder, ConstSlice<u8> input,
    bool peer_closed = false) noexcept;

} // namespace maicord
