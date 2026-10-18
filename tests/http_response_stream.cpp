#include "maicord/protocol/http_response_stream.hpp"
#include "maicord/core/memory.hpp"
#include <Windows.h>
#include <initializer_list>

namespace maicord { namespace {
ConstSlice<u8> bytes(Utf8View value) noexcept {
    return {reinterpret_cast<const u8*>(value.data), value.bytes};
}
struct Run {
    HttpResponseStreamState state; HttpResponseStreamError error; u64 written; bool exact; u64 expected;
};
Run run(Utf8View wire, u64 expected, u32 segment, bool close, Utf8View body = {},
        u64 maximum = 0) noexcept {
    u8 storage[2048]{}, written[64]{};
    HttpResponseStream d{};
    if (!http_response_stream_initialize(&d, {storage, sizeof(storage)}, expected,
                                         maximum ? maximum : expected, 1024, 32))
        return {HttpResponseStreamState::failed, HttpResponseStreamError::invalid_input, 0, false, 0};
    u32 offset = 0, copied = 0;
    auto state = http_response_stream_feed(&d, {});
    for (u32 guard = 0; guard < 10000; ++guard) {
        if (!state.ok()) break;
        if (state.pending.count) {
            if (state.pending.count <= sizeof(written) - copied) {
                copy_bytes(written + copied, state.pending.data, state.pending.count);
                copied += state.pending.count;
            }
            state = http_response_stream_consume(&d, state.pending.count);
        } else if (offset < wire.bytes) {
            const u32 count = wire.bytes - offset < segment ? wire.bytes - offset : segment;
            state = http_response_stream_feed(&d, {reinterpret_cast<const u8*>(wire.data) + offset, count},
                close && offset + count == wire.bytes);
            offset += state.accepted;
        } else {
            if (close && !d.peer_closed) state = http_response_stream_feed(&d, {}, true);
            break;
        }
    }
    return {state.state, state.error, state.body_bytes,
        offset == wire.bytes &&
            (!body.data || (copied == body.bytes && bytes_equal(written, body.data, body.bytes))),
        d.expected_body_bytes};
}
} // namespace

void test_http_response_stream(void (*check)(bool, const char*) noexcept) noexcept {
    const auto fixed = utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    const auto chunked = utf8_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2;note=ok\r\nhe\r\n3\r\nllo\r\n0\r\nX-Receipt: yes\r\n\r\n");
    const auto until_close = utf8_literal("HTTP/1.0 200 OK\r\n\r\nhello");
    bool partitions = true;
    for (u32 segment = 1; segment <= chunked.bytes; ++segment) {
        for (const auto wire : {fixed, chunked, until_close}) {
            const auto result = run(wire, 5, segment, true, utf8_literal("hello"));
            partitions = partitions && result.state == HttpResponseStreamState::complete &&
                result.error == HttpResponseStreamError::none && result.written == 5 && result.exact;
        }
    }
    check(partitions, "stream handles every small feed partition for length/chunked/until-close without body allocation");
    bool from_head = true;
    for (u32 segment = 1; segment <= chunked.bytes; ++segment) {
        for (const auto wire : {fixed, chunked}) {
            const auto result =
                run(wire, kHttpResponseLengthFromHead, segment, true, utf8_literal("hello"), 10);
            from_head = from_head && result.state == HttpResponseStreamState::complete &&
                        result.written == 5 && result.expected == 5 && result.exact;
        }
    }
    check(from_head &&
              run(until_close, kHttpResponseLengthFromHead, 64, true, {}, 10).error ==
                  HttpResponseStreamError::unsupported_body &&
              run(fixed, kHttpResponseLengthFromHead, 64, true, {}, 4).error ==
                  HttpResponseStreamError::size_limit &&
              run(chunked, kHttpResponseLengthFromHead, 64, true, {}, 4).error ==
                  HttpResponseStreamError::size_limit,
          "stream takes an unknown length from Content-Length or the last chunk within the maximum");
    const auto uncl = run(until_close, 5, 1, false, utf8_literal("hello"));
    check(uncl.state == HttpResponseStreamState::body && uncl.written == 5,
        "until-close stream waits for peer closure after exact bytes");
    bool zero = true;
    for (const auto wire : {utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"),
            utf8_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"),
            utf8_literal("HTTP/1.0 200 OK\r\n\r\n")}) {
        const auto result = run(wire, 0, 1, true);
        zero = zero && result.state == HttpResponseStreamState::complete && result.written == 0;
    }
    check(zero, "stream accepts an exact zero-byte attachment with all three framings");
    bool incomplete = true;
    for (const auto wire : {utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhell"),
            utf8_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n"),
            utf8_literal("HTTP/1.0 200 OK\r\n\r\nhell"), utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n")}) {
        const auto result = run(wire, 5, 1, true);
        incomplete = incomplete && result.state == HttpResponseStreamState::failed &&
            result.error == HttpResponseStreamError::unexpected_close;
    }
    check(incomplete, "stream rejects truncated headers, fixed bodies, chunks/trailers and close-delimited bodies");
    bool extra = true;
    for (const auto wire : {utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello!"),
            utf8_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n!"),
            utf8_literal("HTTP/1.0 200 OK\r\n\r\nhello!")}) {
        for (const u32 segment : {1u, wire.bytes}) {
            const auto result = run(wire, 5, segment, true);
            extra = extra && result.state == HttpResponseStreamState::failed &&
                result.error == HttpResponseStreamError::unexpected_data;
        }
    }
    check(extra, "stream rejects extra entity bytes and bytes after fixed/chunked completion in same or later feeds");
    bool malformed = true;
    for (const auto wire : {utf8_literal("HTTP/1.1 200 OK\nContent-Length: 5\n\nhello"),
            utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n"),
            utf8_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"),
            utf8_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\ng\r\n"),
            utf8_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello!\n0\r\n\r\n"),
            utf8_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n invalid: trailer\r\n\r\n"),
            utf8_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10000000000000000\r\n")}) {
        const auto result = run(wire, 5, 1, true);
        malformed = malformed && result.state == HttpResponseStreamState::failed;
    }
    check(malformed, "stream preserves strict CRLF/header/transfer coding/chunk/trailer/overflow parser rejection");
    const auto mismatch = run(utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nhell"), 5, 1, true);
    const auto short_chunk = run(utf8_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nhell\r\n0\r\n\r\n"), 5, 1, true);
    const auto nonzero = run(utf8_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx\r\n0\r\n\r\n"), 0, 1, true);
    check(mismatch.error == HttpResponseStreamError::size_mismatch && mismatch.written == 0 &&
        short_chunk.error == HttpResponseStreamError::size_mismatch && nonzero.state == HttpResponseStreamState::failed &&
        nonzero.written == 0, "stream compares declared and decoded sizes, including nonzero chunk in zero-sized attachment");

    u8 storage[2048]{};
    HttpResponseStream d{};
    check(http_response_stream_initialize(&d, {storage, sizeof(storage)}, 5, 5, 1024, 32), "stream fixture initializes");
    auto state = http_response_stream_feed(&d, bytes(fixed));
    const auto* borrowed = state.pending.data;
    const auto blocked = http_response_stream_feed(&d, bytes(utf8_literal("later")));
    check(state.pending.count == 5 && blocked.accepted == 0 && blocked.pending.data == borrowed &&
        bytes_equal(borrowed, "hello", 5) && blocked.body_bytes == 0,
        "pending output is immutable and feed backpressure accepts zero until write acknowledgement");
    state = http_response_stream_consume(&d, 2);
    check(state.pending.data == borrowed + 2 && state.pending.count == 3 && state.body_bytes == 2 &&
        bytes_equal(state.pending.data, "llo", 3), "partial consume retains the unwritten prefix without moving bytes");
    state = http_response_stream_consume(&d, 3);
    const auto terminal = http_response_stream_feed(&d, bytes(utf8_literal("!")));
    check(state.state == HttpResponseStreamState::complete && state.body_bytes == 5 &&
        terminal.error == HttpResponseStreamError::unexpected_data, "completed stream rejects later extra data");
    const auto sticky = http_response_stream_feed(&d, {});
    check(sticky.error == terminal.error, "terminal stream failures preserve original diagnostic");
    http_response_stream_initialize(&d, {storage, sizeof(storage)}, 5, 5, 1024, 32);
    state = http_response_stream_feed(&d, bytes(fixed));
    check(http_response_stream_consume(&d, state.pending.count + 1).error == HttpResponseStreamError::invalid_input,
        "stream rejects acknowledgement larger than borrowed output");
    const u64 large = 4294967301ull;
    http_response_stream_initialize(&d, {storage, sizeof(storage)}, large, large, 1024, 32);
    state = http_response_stream_feed(&d, bytes(utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 4294967301\r\n\r\n")));
    check(state.ok() && d.parsed_head.content_length == large && d.expected_body_bytes == large && !state.pending.count &&
        !http_response_stream_initialize(&d, {storage, sizeof(storage)}, large, large - 1, 1024, 32),
        "stream accepts u64 sizes above4GiB without allocating the advertised body and enforces maximum");

    u8 wire[4096]{};
    const auto head = utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 3500\r\n\r\n");
    copy_bytes(wire, head.data, head.bytes);
    for (u32 i = head.bytes; i < head.bytes + 3500; ++i) wire[i] = 'x';
    http_response_stream_initialize(&d, {storage, sizeof(storage)}, 3500, 3500, 1024, 32);
    state = http_response_stream_feed(&d, {wire, head.bytes + 3500}, true);
    const u32 accepted = state.accepted;
    bool capacity = accepted == sizeof(storage) && state.pending.count == sizeof(storage) - head.bytes && !d.peer_closed;
    state = http_response_stream_consume(&d, state.pending.count);
    state = http_response_stream_feed(&d, {wire + accepted, head.bytes + 3500 - accepted}, true);
    state = http_response_stream_consume(&d, state.pending.count);
    check(capacity && state.state == HttpResponseStreamState::complete && state.body_bytes == 3500 && d.peer_closed,
        "bounded stream reports partial acceptance and applies EOF only after the replayed remainder");

    const auto exact_head = utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-Pad: ");
    copy_bytes(wire, exact_head.data, exact_head.bytes);
    for (u32 i = exact_head.bytes; i < sizeof(storage) - 4; ++i) wire[i] = 'a';
    copy_bytes(wire + sizeof(storage) - 4, "\r\n\r\n", 4);
    copy_bytes(wire + sizeof(storage), "hello", 5);
    http_response_stream_initialize(&d, {storage, sizeof(storage)}, 5, 5, sizeof(storage), 32);
    state = http_response_stream_feed(&d, {wire, sizeof(storage) + 5}, true);
    const bool exact_admitted = state.ok() && state.accepted == sizeof(storage) && !state.pending.count &&
        d.parsed_head.header_bytes == sizeof(storage) && d.state == HttpResponseStreamState::body;
    state = http_response_stream_feed(&d, {wire + sizeof(storage), 5}, true);
    const auto* exact_pending = state.pending.data;
    state = http_response_stream_consume(&d, 2);
    const auto paused = http_response_stream_feed(&d, {});
    const bool pending_stable = paused.pending.data == exact_pending + 2 && paused.pending.count == 3 &&
        bytes_equal(paused.pending.data, "llo", 3);
    state = http_response_stream_consume(&d, 3);
    check(exact_admitted && pending_stable && state.state == HttpResponseStreamState::complete && state.body_bytes == 5,
        "maximum-size exact header frees a full buffer and preserves subsequently paused body span");
    http_response_stream_initialize(&d, {storage, sizeof(storage)}, 5, 5, sizeof(storage), 32);
    for (u32 i = sizeof(storage) - 4; i < sizeof(storage); ++i) wire[i] = 'a';
    const auto oversized = http_response_stream_feed(&d, {wire, sizeof(storage)});
    check(oversized.error == HttpResponseStreamError::head && d.framing_error == HttpError::header_too_large,
        "full buffer with incomplete oversized header fails rather than losing bytes or waiting forever");
}
} // namespace maicord
