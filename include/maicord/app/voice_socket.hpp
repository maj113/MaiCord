#pragma once

#include "maicord/platform/tls_stream.hpp"
#include "maicord/protocol/voice_session.hpp"
#include "maicord/core/stable_storage.hpp"

namespace maicord {
enum class VoiceSocketPhase : u8 {
    empty,
    connecting,
    opening,
    open,
    closing,
    closed
};
enum class VoiceSocketError : u8 {
    none,
    storage,
    random,
    transport,
    protocol,
    heartbeat,
    timeout
};
enum class VoiceSocketSend : u8 {
    accepted,
    busy,
    invalid,
    failed
};
enum class VoiceSocketOutput : u8 {
    none,
    opening,
    application,
    heartbeat,
    pong,
    close
};
// Callback views are worker-only and expire on return. Return false to stop;
// do not invoke socket APIs from a callback. Queue owner actions for after step.
using VoiceSocketHandler = bool (*)(void*, const VoiceSessionEvent&) noexcept;
struct VoiceSocket {
    TlsStream tls;
    VoiceSession session;
    Slice<u8> storage;
    StableStorage json_storage;
    GatewayEndpoint endpoint;
    Slice<u8> output;
    VoiceSocketHandler handler;
    void* context;
    u64 setup_generation, now_ms, connect_deadline_ms, close_deadline_ms;
    u32 output_bytes;
    u8 control[125];
    u8 control_bytes;
    u16 close_code;
    VoiceSocketPhase phase;
    VoiceSocketError error;
    VoiceSocketOutput sending;
    TlsStreamEvent tls_event;
    bool pending_pong, pending_close, stop_requested, resume, peer_closed;
};
// Zero-initialize once. All APIs execute on an existing network worker. This
// owns an erased packet allocation and lazily committed JSON index; no worker thread or
// retained credential copy is made. Credentials stay in the account's locked
// owner; transient encoded packets are erased after completion and on teardown.
// An attempt must drain before reuse. setup_generation tags the account's voice
// rendezvous; the session heartbeat separately tags socket attempts. Resume is
// chosen by the account/media owner, never inferred from socket opening alone.
bool voice_socket_begin(VoiceSocket* socket, NetworkLoop* loop, const GatewayEndpoint& endpoint,
                        u64 setup_generation, u32 maximum_message_bytes, VoiceSocketHandler handler,
                        void* context, u64 now_ms, bool resume = false) noexcept;
void voice_socket_completion(VoiceSocket* socket, NetworkLoop* loop, const NetworkCompletion& completion,
                             u64 now_ms) noexcept;
void voice_socket_tick(VoiceSocket* socket, u64 now_ms) noexcept;
// Zero means wait for I/O/owner commands. Due heartbeat sends under backpressure
// are retired until write completion, avoiding zero-timeout worker loops.
u64 voice_socket_deadline(const VoiceSocket& socket) noexcept;
VoiceSocketSend voice_socket_send(VoiceSocket* socket, WebSocketOpcode opcode, ConstSlice<u8> payload,
                                  u64 now_ms) noexcept;
// Owner-initiated close of an open socket with a close code (e.g. 4804): the
// close frame is sent when output is idle and the socket then ends. Not from
// a callback. False when the socket is not open or already closing.
bool voice_socket_close(VoiceSocket* socket, u16 code, u64 now_ms) noexcept;
// Immediate cancellation, including during resolve/TLS/opening. Continue routing
// completions until phase==closed before freeing the owner or worker loop.
void voice_socket_stop(VoiceSocket* socket) noexcept;
} // namespace maicord
