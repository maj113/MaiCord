#pragma once

#include "maicord/protocol/http1.hpp"

namespace maicord {

enum class HttpResponseStreamState : u8 { head, body, complete, failed };
enum class HttpResponseStreamError : u8 {
    none, invalid_input, input_capacity, head, unsupported_body,
    body_framing, size_mismatch, size_limit, unexpected_close, unexpected_data,
};

// An expected length the response decides: it must carry a Content-Length,
// at most the maximum, which is exact from then on (expected_body_bytes
// holds it once the head is parsed).
inline constexpr u64 kHttpResponseLengthFromHead = ~u64{0};

// Does not allocate. The caller keeps input storage and parser context alive.
// body_bytes counts bytes acknowledged by consume, not merely received bytes.
struct HttpResponseStream {
    ByteQueue input;
    HttpResponseHead parsed_head;
    HttpChunkDecoder chunk_decoder;
    HttpResponseHeadParser head_parser;
    void* head_parser_context;
    ConstSlice<u8> pending;
    u64 expected_body_bytes;
    u64 maximum_body_bytes;
    u64 body_bytes;
    u32 maximum_header_bytes;
    u32 maximum_fields;
    HttpResponseStreamState state;
    HttpResponseStreamError error;
    HttpError framing_error;
    bool peer_closed;
};

struct HttpResponseStreamStep {
    ConstSlice<u8> pending;
    u64 body_bytes;
    u32 accepted;
    HttpResponseStreamState state;
    HttpResponseStreamError error;
    constexpr bool ok() const noexcept { return error == HttpResponseStreamError::none; }
};

bool http_response_stream_initialize(
    HttpResponseStream* decoder, Slice<u8> input_storage,
    u64 expected_body_bytes, u64 maximum_body_bytes,
    u32 maximum_header_bytes, u32 maximum_fields,
    HttpResponseHeadParser head_parser = nullptr,
    void* head_parser_context = nullptr) noexcept;

// accepted may be smaller than input.count. Replay the unaccepted suffix,
// including its peer_closed indication, before trusting completion. No input
// is accepted while pending is nonempty: its address and bytes stay stable.
// Empty feeds can communicate EOF or drive framing after a consume.
HttpResponseStreamStep http_response_stream_feed(
    HttpResponseStream* decoder, ConstSlice<u8> input,
    bool peer_closed = false) noexcept;

// Acknowledges a successfully written prefix of pending. An error is terminal;
// output that was already acknowledged must be discarded by the download code.
HttpResponseStreamStep http_response_stream_consume(
    HttpResponseStream* decoder, u32 bytes) noexcept;

} // namespace maicord
