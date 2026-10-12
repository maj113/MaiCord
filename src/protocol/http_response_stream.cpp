#include "maicord/protocol/http_response_stream.hpp"

namespace maicord { namespace {
HttpResponseStreamStep step(const HttpResponseStream& d, u32 accepted = 0) noexcept {
    return {d.pending, d.body_bytes, accepted, d.state, d.error};
}
HttpResponseStreamStep fail(HttpResponseStream* d, HttpResponseStreamError error,
                            u32 accepted = 0, HttpError framing = HttpError::none) noexcept {
    d->state = HttpResponseStreamState::failed;
    d->error = error;
    d->framing_error = framing;
    d->pending = {};
    return step(*d, accepted);
}
HttpResponseStreamStep pump(HttpResponseStream* d, u32 accepted) noexcept {
    if (d->pending.count || d->state == HttpResponseStreamState::failed) return step(*d, accepted);
    for (;;) {
        auto readable = byte_queue_readable(d->input);
        if (d->state == HttpResponseStreamState::head) {
            const auto parsed = d->head_parser
                ? d->head_parser(d->head_parser_context, readable, d->maximum_header_bytes, d->maximum_fields)
                : http_parse_response_head(readable, d->maximum_header_bytes, d->maximum_fields, nullptr, nullptr);
            if (parsed.state == HttpParseState::error) {
                // Admission callbacks may reject a syntactically valid head
                // while preserving its safe status/length diagnostics.
                if (parsed.head.header_bytes) d->parsed_head = parsed.head;
                return fail(d, HttpResponseStreamError::head, accepted, parsed.error);
            }
            if (parsed.state == HttpParseState::need_more) {
                if (d->peer_closed) return fail(d, HttpResponseStreamError::unexpected_close, accepted);
                if (!byte_queue_available(d->input)) return fail(d, HttpResponseStreamError::input_capacity, accepted);
                return step(*d, accepted);
            }
            // This stream is a single entity, never a tunnel or interim response.
            if (parsed.head.status_code < 200 || parsed.head.status_code == 204 || parsed.head.status_code == 304)
                return fail(d, HttpResponseStreamError::unsupported_body, accepted);
            d->parsed_head = parsed.head;
            if (parsed.head.has_content_length && parsed.head.content_length > d->maximum_body_bytes)
                return fail(d, HttpResponseStreamError::size_limit, accepted);
            if (parsed.head.has_content_length && parsed.head.content_length != d->expected_body_bytes)
                return fail(d, HttpResponseStreamError::size_mismatch, accepted);
            if (!byte_queue_consume(&d->input, parsed.head.header_bytes))
                return fail(d, HttpResponseStreamError::invalid_input, accepted);
            d->state = HttpResponseStreamState::body;
            continue;
        }
        if (d->state == HttpResponseStreamState::complete) {
            if (readable.count) return fail(d, HttpResponseStreamError::unexpected_data, accepted);
            return step(*d, accepted);
        }
        const auto kind = http_response_body_kind(d->parsed_head, false, false);
        if (kind == HttpBodyKind::chunked) {
            const auto chunk = http_chunk_decode(&d->chunk_decoder, readable);
            if (chunk.state == HttpChunkStepState::error)
                return fail(d, HttpResponseStreamError::body_framing, accepted, chunk.error);
            if (d->chunk_decoder.body_bytes > d->expected_body_bytes)
                return fail(d, HttpResponseStreamError::size_mismatch, accepted);
            if (chunk.state == HttpChunkStepState::data) {
                // Release only framing; body bytes remain borrowed until consume.
                if (chunk.consumed < chunk.data.count ||
                    !byte_queue_consume(&d->input, chunk.consumed - chunk.data.count))
                    return fail(d, HttpResponseStreamError::invalid_input, accepted);
                d->pending = byte_queue_readable(d->input);
                d->pending.count = chunk.data.count;
                return step(*d, accepted);
            }
            if (!byte_queue_consume(&d->input, chunk.consumed))
                return fail(d, HttpResponseStreamError::invalid_input, accepted);
            if (chunk.state == HttpChunkStepState::complete) {
                if (d->body_bytes != d->expected_body_bytes)
                    return fail(d, HttpResponseStreamError::size_mismatch, accepted);
                d->state = HttpResponseStreamState::complete;
                continue;
            }
            if (d->peer_closed) return fail(d, HttpResponseStreamError::unexpected_close, accepted);
            if (!byte_queue_available(d->input)) return fail(d, HttpResponseStreamError::input_capacity, accepted);
            return step(*d, accepted);
        }
        if (kind != HttpBodyKind::content_length && kind != HttpBodyKind::until_close && kind != HttpBodyKind::none)
            return fail(d, HttpResponseStreamError::unsupported_body, accepted);
        const u64 remaining = d->expected_body_bytes - d->body_bytes;
        if (readable.count > remaining)
            return fail(d, HttpResponseStreamError::unexpected_data, accepted);
        if (readable.count) {
            d->pending = readable;
            return step(*d, accepted);
        }
        if (kind != HttpBodyKind::until_close || d->peer_closed) {
            if (remaining) {
                if (d->peer_closed) return fail(d, HttpResponseStreamError::unexpected_close, accepted);
                return step(*d, accepted);
            }
            d->state = HttpResponseStreamState::complete;
            continue;
        }
        return step(*d, accepted);
    }
}
} // namespace

bool http_response_stream_initialize(HttpResponseStream* d, Slice<u8> storage,
    u64 expected, u64 maximum, u32 maximum_header, u32 maximum_fields,
    HttpResponseHeadParser parser, void* context) noexcept {
    if (!d || !storage.data || storage.count < maximum_header || maximum_header < 4 ||
        storage.count < 1026 || !maximum_fields || expected > maximum || (!parser && context)) return false;
    *d = {};
    d->expected_body_bytes = expected;
    d->maximum_body_bytes = maximum;
    d->maximum_header_bytes = maximum_header;
    d->maximum_fields = maximum_fields;
    d->head_parser = parser;
    d->head_parser_context = context;
    d->state = HttpResponseStreamState::head;
    // The strict shared chunk parser excludes zero maximum. A zero-sized entity
    // uses a one-byte parser limit and rejects every nonzero chunk before output.
    return byte_queue_initialize(&d->input, storage) &&
        http_chunk_decoder_initialize(&d->chunk_decoder, expected ? expected : 1, 1024, maximum_header, maximum_fields);
}

HttpResponseStreamStep http_response_stream_feed(HttpResponseStream* d, ConstSlice<u8> input,
                                                bool peer_closed) noexcept {
    if (!d) return {{}, 0, 0, HttpResponseStreamState::failed, HttpResponseStreamError::invalid_input};
    if (d->state == HttpResponseStreamState::failed) return step(*d);
    if ((!input.data && input.count) || !d->input.data)
        return fail(d, HttpResponseStreamError::invalid_input);
    if (d->pending.count) {
        if (!input.count && peer_closed) d->peer_closed = true;
        return step(*d);
    }
    if (input.count && (d->peer_closed || d->state == HttpResponseStreamState::complete))
        return fail(d, HttpResponseStreamError::unexpected_data);
    const u32 available = byte_queue_available(d->input);
    const u32 accepted = input.count < available ? input.count : available;
    if (accepted && !byte_queue_append(&d->input, {input.data, accepted}))
        return fail(d, HttpResponseStreamError::invalid_input);
    if (peer_closed && accepted == input.count) d->peer_closed = true;
    auto result = pump(d, accepted);
    if (result.state == HttpResponseStreamState::complete && accepted != input.count)
        return fail(d, HttpResponseStreamError::unexpected_data, accepted);
    return result;
}

HttpResponseStreamStep http_response_stream_consume(HttpResponseStream* d, u32 bytes) noexcept {
    if (!d) return {{}, 0, 0, HttpResponseStreamState::failed, HttpResponseStreamError::invalid_input};
    if (d->state == HttpResponseStreamState::failed) return step(*d);
    if (bytes > d->pending.count || !d->input.data)
        return fail(d, HttpResponseStreamError::invalid_input);
    if (bytes) {
        if (!byte_queue_consume(&d->input, bytes)) return fail(d, HttpResponseStreamError::invalid_input);
        d->body_bytes += bytes;
        d->pending.data += bytes;
        d->pending.count -= bytes;
    }
    if (!d->pending.count) d->pending = {};
    return pump(d, 0);
}

} // namespace maicord
