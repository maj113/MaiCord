#pragma once

#include "maicord/core/byte_queue.hpp"
#include "maicord/platform/network.hpp"
#include "maicord/platform/tls_client.hpp"

namespace maicord {

inline constexpr u32 kTlsStreamMaximumWriteSpans = 4;
// The edge closes an idle HTTP/1.1 connection after about 400 s and nothing
// reads an idle stream, so an owner that reuses one connects afresh after this
// idle time (Chromium's used-idle-socket timeout) instead of failing a request.
inline constexpr u32 kTlsStreamIdleReuseMs = 5 * 60 * 1000;

enum class TlsStreamState : u8 {
    empty,
    resolving,
    connecting,
    handshake,
    ready,
    writing,
    reading,
    closing,
    closed,
    failed,
    duplex,
    read_paused,
};

enum class TlsStreamError : u8 {
    none,
    invalid_input,
    resolve,
    connect,
    tls,
    send,
    receive,
    capacity,
    unexpected_data,
};

enum class TlsStreamEventKind : u8 {
    waiting,
    ready,
    write_complete,
    read_complete,
    drained,
    failed,
    read_paused,
};

struct TlsStreamEvent {
    u32 native_error;
    TlsStreamEventKind kind;
    TlsStreamError error;

    constexpr bool ok() const noexcept {
        return error == TlsStreamError::none;
    }
};

struct TlsStreamBuffers {
    Slice<u8> encrypted_input;
    Slice<u8> handshake_output;
    Slice<u8> record_output;
    // Owner storage in VirtualAlloc pages: whole pages of a closed stream are decommitted, not wiped.
    bool release_pages = false;
};

enum class TlsStreamReadAction : u8 {
    continue_read,
    complete,
    reject,
    pause,
};

using TlsStreamCommitHandler = void (*)(void* context, ConstSlice<u8> committed) noexcept;
using TlsStreamReadHandler = TlsStreamReadAction (*)(void* context, ConstSlice<u8> plaintext,
                                                     bool peer_closed) noexcept;

struct TlsStream {
    NetworkResolver resolver;
    NetworkConnection connection;
    TlsClient tls;
    ByteQueue encrypted_input;
    TlsStreamBuffers buffers;
    addrinfoexW* next_address;
    const wchar_t* server_name;
    wchar_t service[6];
    const u8* send_data;
    ConstSlice<u8> write_spans[kTlsStreamMaximumWriteSpans];
    TlsStreamCommitHandler commit_handler;
    TlsStreamReadHandler read_handler;
    void* callback_context;
    void* read_context;
    u32 send_bytes;
    u32 send_offset;
    u32 write_span_count;
    u32 write_span_index;
    u32 write_span_offset;
    u32 current_plaintext_bytes;
    u32 outstanding_operations;
    SECURITY_STATUS tls_status;
    TlsHandshakeState after_tls_send;
    TlsStreamState state;
    bool resolver_started;
    bool tls_initialized;
    bool handshake_send;
    bool duplex;
    bool paused_peer_closed;
    bool read_completed;
    bool buffers_released;
};

// Zero-initialize before first use. All API calls belong to one worker; socket
// I/O can overlap but mutations and callbacks are serialized on that worker.
// server_name and every buffer remain caller-owned and stable until the
// stream reaches closed. Buffers may not overlap. endpoint takes a hostname or
// unbracketed IP literal, not a URL or host:port authority.
bool tls_stream_initialize(TlsStream* stream, const wchar_t* server_name,
                           TlsStreamBuffers buffers) noexcept;
bool tls_stream_initialize_endpoint(TlsStream* stream, const wchar_t* server_name, u16 port,
                                    TlsStreamBuffers buffers) noexcept;
TlsStreamEvent tls_stream_begin_connect(TlsStream* stream, NetworkLoop* loop) noexcept;
TlsStreamEvent tls_stream_start_write(TlsStream* stream, ConstSlice<ConstSlice<u8>> spans,
                                      TlsStreamCommitHandler commit_handler = nullptr,
                                      void* callback_context = nullptr) noexcept;
TlsStreamEvent tls_stream_start_read(TlsStream* stream, TlsStreamReadHandler read_handler,
                                     void* callback_context) noexcept;
// A non-duplex callback may pause after consuming/copying its plaintext. No
// further receive is posted and encrypted tail records are kept. Resume only
// after downstream borrowed buffers are released. Resume first calls the handler
// with empty input (and EOF, if one was pending), then authenticates the kept tail.
// Empty records/shutdown are allowed; extra application bytes fail. A fragmented
// tail can return waiting until its outstanding receive completes; the caller keeps its deadline.
TlsStreamEvent tls_stream_resume_read(TlsStream* stream) noexcept;
// Starts a persistent reader while allowing serialized writes. Read and commit
// callbacks have independent contexts. Callbacks consume/update caller state;
// defer stream close/reinitialization until the invoking stream API returns.
TlsStreamEvent tls_stream_start_duplex(TlsStream* stream, TlsStreamReadHandler read_handler,
                                       void* callback_context) noexcept;
TlsStreamEvent tls_stream_handle_completion(TlsStream* stream, NetworkLoop* loop,
                                            const NetworkCompletion& completion) noexcept;

// Close cancels resolver/socket work. Caller storage remains live until a
// drained event or tls_stream_drained() is true.
void tls_stream_close(TlsStream* stream) noexcept;
bool tls_stream_drained(TlsStream* stream) noexcept;
// A closed stream's buffers are decommitted instead of wiped when buffers.release_pages is set and the
// owner's buffers live in VirtualAlloc pages; begin_connect and initialize recommit them (zeroed). Call
// this before the owner touches the buffers of a closed stream or frees their allocation.
bool tls_stream_restore_buffers(TlsStream* stream) noexcept;

} // namespace maicord
