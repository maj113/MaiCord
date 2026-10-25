#pragma once
#include "maicord/app/voice_connection_setup.hpp"
#include "maicord/app/voice_socket.hpp"
#include "maicord/app/voice_media_setup.hpp"
#include "maicord/app/voice_udp_discovery.hpp"
#include "maicord/app/voice_udp_media.hpp"
#include "maicord/app/voice_participants.hpp"
#include "maicord/app/voice_dave_route.hpp"
#include "maicord/app/dave_session.hpp"
#include "maicord/app/voice_log.hpp"
#include "maicord/protocol/rtcp_feedback.hpp"
#include "maicord/protocol/voice_video_signaling.hpp"

namespace maicord {
inline constexpr u32 kVoiceDaveOutbox = 16;
// Consecutive invalid commit/welcome reports (opcode 31) before the call
// reconnects with a fresh session, as the official client does (close 4803).
inline constexpr u8 kVoiceDaveInvalidLimit = 5;
// Official client close codes for unrecoverable DAVE state; both reconnect
// with a fresh session on a 1-10 s backoff (its MLS failure backoff).
inline constexpr u16 kVoiceCloseRepeatedMlsInvalid = 4803;
inline constexpr u16 kVoiceCloseDaveDowngradeRefused = 4804;
inline constexpr u64 kVoiceDaveReconnectMinimumMs = 1'000, kVoiceDaveReconnectMaximumMs = 10'000;
// Video receive: Identify advertises video with the camera stream, Select
// Protocol offers H.264 decode-only (PT 101, RTX 102). No encoder is offered.
inline constexpr u32 kVoiceSinkWantsMaximum = 32;
enum class VoiceConnectionError : u8 {
    none,
    endpoint,
    command,
    storage,
    socket,
    media,
    datagram,
    protocol,
    protocol_timeout,
    participants
};
struct VoiceReceivedPacket {
    VoicePacketResult transport;
    VoiceSsrcBinding source;
    VoiceParticipant member;
    u64 setup_generation, ready_revision;
};
// RTP carries the current signaling-announced source/member. RTCP is from the
// authenticated UDP peer and deliberately carries no participant identity.
// This is NOT DAVE authorization or replay protection. Views expire on return;
// queued work must own its bytes and revalidate identity before using them.
using VoiceMappedMediaReceive = bool (*)(void*, ConstSlice<u8>, const VoiceReceivedPacket&) noexcept;
// Account-worker owner. Never copy or expose its encoded authentication command
// to UI/logging. One credential generation may create only one initial attempt;
// retry/resume policy is separate from rendezvous and not a zero-wait restart.
struct VoiceConnection {
    VoiceSocket socket;
    SecureBuffer command;
    VoiceMediaSetup media;
    VoiceUdpDiscovery udp;
    VoiceUdpMedia media_io;
    VoiceParticipants participants;
    VoiceParticipantsResult participant_error;
    VoiceMappedMediaReceive media_receive;
    void* media_context;
    u64 unannounced_rtp_packets;
    u64 attempted_generation;
    u64 ready_revision, udp_revision;
    u64 protocol_deadline_ms;
    u64 reconnect_deadline_ms;
    VoiceDaveInbox dave;
    // DAVE E2EE for this call. Gateway callbacks may not send, so replies are
    // queued as complete WebSocket payloads and flushed by drive in order.
    DaveSession dave_session;
    SecureBuffer dave_outbox[kVoiceDaveOutbox];
    bool dave_outbox_binary[kVoiceDaveOutbox];
    u64 dave_group_id, dave_user_id;
    u32 dave_outbox_head, dave_outbox_count, dave_external_sender_bytes;
    u8 dave_external_sender[kMlsMaxExternalSenderBytes]; // Opcode 25 seen before Session Description.
    bool dave_recovering;
    // Stage channels are not end-to-end encrypted: Session Description may
    // negotiate DAVE version 0 (transport-only passthrough) there. Version 0
    // anywhere else, and any later transition or epoch to version 0 (stages
    // included), is a downgrade refused as the official client does: close
    // 4804 and reconnect.
    bool dave_optional;
    u8 dave_invalid_streak;     // Opcode 31 reports since the last executed transition.
    u8 dave_reconnects;         // Consecutive fresh sessions requested for DAVE state (backoff).
    bool dave_reconnect_requested; // The socket closes with dave_close_code and re-identifies.
    u16 dave_close_code;        // 4803/4804, sent by drive; later events are ignored.
    // Diagnostics only (voice log): last logged phases and per-packet limiters.
    VoiceSocketPhase logged_socket_phase;
    VoiceUdpPhase logged_udp_phase;
    bool logged_description, stream_context;
    u64 logged_rejected_packets;
    VoiceLogLimiter unannounced_log, decrypt_log, rejected_log;
    VoiceConnectionError error;
    VoiceMediaSetupError media_error;
    VoiceCloseAction reconnect_action;
    u8 reconnect_attempts;
    bool identify_submitted;
    bool reconnect_pending; // The closed socket is replaced after reconnect_deadline_ms.
    bool resumed;           // The current socket authenticated with Resume (op 7).
    // Per-join RTC connection id (Select Protocol), kept across reconnects of
    // the same credential generation.
    u64 rtc_connection_generation;
    char rtc_connection_id[kVoiceRtcConnectionIdBytes];
    // voice_video_codec_bit set the owner's decoders handle; Select Protocol
    // offers them (voice_video_decode_offers) and records the offer in media.
    u32 video_decodable;
    // Desired media sink wants (op 15), sent after Session Description and
    // whenever they change. The default asks for every stream at full quality.
    VoiceSinkWant sink_wants[kVoiceSinkWantsMaximum];
    u32 sink_want_count;
    u8 sink_any_quality;
    bool sink_wants_configured; // The owner set wants; otherwise {"any":100}.
    bool sink_wants_sent;       // Current wants were accepted for this session.
    // Transport-wide sequence numbers of every authenticated RTP packet
    // (announced or not), reset per media attachment; the video sink sends
    // the feedback. rtp_extension_ids: header extension IDs seen (logged).
    RtcpTransportFeedbackState transport_feedback;
    u16 rtp_extension_ids;
};
inline constexpr u8 kVoiceReconnectAttempts = 3;
// Drive owner commands/timers. Deliver a polled socket completion directly to
// voice_connection_completion: it cancels a superseded attempt before delivery
// and starts replacements only afterward. Do not drive between polling and
// delivering that completion. No token/session views survive either call.
void voice_connection_drive(VoiceConnection* connection, const VoiceConnectionSetup& setup,
                            NetworkLoop* loop, u64 now_ms) noexcept;
bool voice_connection_owns(const VoiceConnection& connection, const NetworkCompletion& completion) noexcept;
void voice_connection_completion(VoiceConnection* connection, const VoiceConnectionSetup& setup,
                                 NetworkLoop* loop, const NetworkCompletion& completion,
                                 u64 now_ms) noexcept;
void voice_connection_stop(VoiceConnection* connection) noexcept;
bool voice_connection_drained(const VoiceConnection& connection) noexcept;
u64 voice_connection_deadline(const VoiceConnection& connection) noexcept;
// Typed worker transition after decoding Ready; owns endpoint metadata and
// schedules replacement discovery after the enclosing socket callback returns.
VoiceMediaSetupError voice_connection_ready(VoiceConnection* connection, const VoiceReady& ready) noexcept;
struct VoiceProtocolCommand {
    u64 ready_revision;
    u32 mode;
    GatewayCommandWriteResult write;
    u32 video_offer; // voice_video_codec_bit set of the offered video codecs.
};
// Worker-owned two-phase command: zero revision means discovery/selection is
// not ready. A nonzero revision has the writer result (including size queries).
VoiceProtocolCommand voice_connection_protocol_command(const VoiceConnection& connection,
                                                       Slice<u8> output) noexcept;
// Invoke only after the WebSocket accepted the prepared command. Stale, repeated
// or failed preparation cannot commit selection or change the cipher owner.
bool voice_connection_protocol_submitted(VoiceConnection* connection, const VoiceProtocolCommand& command,
                                         u64 now_ms) noexcept;
VoiceMediaSetupError voice_connection_description(VoiceConnection* connection, JsonValue data,
                                                  Slice<u32> scratch, u32 maximum_depth,
                                                  u64 now_ms) noexcept;
// Attach the worker's media pipeline only after current Description/key setup.
// The consumer owns buffers until connection drain. Unannounced RTP sources
// are dropped; the consumer must still enforce replay, DAVE and codec policy.
// receive_slots concurrent reads split receive_buffer equally (see
// voice_udp_media_begin); production posts kVoiceUdpMediaReceiveSlots.
bool voice_connection_media_start(VoiceConnection* connection, Slice<u8> receive_buffer,
                                  Slice<u8> send_buffer, VoiceMappedMediaReceive receive,
                                  void* context, u32 receive_slots = 1) noexcept;
// Worker-only identity-currency check for previously delivered work. Activity,
// flags and connected snapshots are not current authorization decisions.
bool voice_connection_media_current(const VoiceConnection& connection,
                                    const VoiceReceivedPacket& packet) noexcept;
VoiceUdpMediaSend voice_connection_media_send(VoiceConnection* connection, u64 ready_revision,
                                              ConstSlice<u8> plaintext) noexcept;
// Speaking (op 5) for the local audio SSRC announced by Ready.
VoiceSocketSend voice_connection_speaking(VoiceConnection* connection, u32 flags, u64 now_ms) noexcept;
// Replace the desired media sink wants; drive sends op 15 once the session is
// described and the socket is idle. Unchanged wants are not resent. False for
// invalid input (quality above 100, zero/duplicate SSRC, too many entries).
bool voice_connection_set_sink_wants(VoiceConnection* connection, u8 any_quality,
                                     ConstSlice<VoiceSinkWant> wants) noexcept;
// DAVE media hooks for the audio pipelines. Decrypt takes the transport-opened
// payload of a currently mapped RTP packet (its source user selects the
// sender keys); only a successful result may be decoded. Encrypt seals one
// Opus packet before RTP/transport encryption. Both pass through only while
// the negotiated DAVE protocol version is zero.
DaveMediaResult voice_connection_dave_decrypt(VoiceConnection* connection,
                                              const VoiceReceivedPacket& packet, ConstSlice<u8> frame,
                                              Slice<u8> scratch, Slice<u8> output, u64 now_ms) noexcept;
// Video: one assembled frame (Annex B after RTP depacketization) from media
// SSRC ssrc, which must still map to user_id's camera/screen source. Audio and
// video of a sender share its DAVE epoch, ratchet and replay window.
DaveMediaResult voice_connection_dave_decrypt_frame(VoiceConnection* connection, u64 user_id, u32 ssrc,
                                                    ConstSlice<u8> frame, Slice<u8> scratch, Slice<u8> output,
                                                    u64 now_ms) noexcept;
DaveSealResult voice_connection_dave_encrypt(VoiceConnection* connection, ConstSlice<u8> opus,
                                             Slice<u8> output) noexcept;
// True once DAVE (or negotiated passthrough) permits sending media.
bool voice_connection_dave_ready(const VoiceConnection& connection) noexcept;
// Gateway event handler of the voice socket (exposed for scripted tests).
bool voice_connection_event(VoiceConnection* connection, const VoiceSessionEvent& event) noexcept;
// DAVE signaling steps run from the socket callback: they update the call's
// session and queue replies (key package, commit/welcome, ready, invalid
// commit) without sending. Membership changes never end the call: malformed,
// unexpected or stale DAVE messages are logged and ignored; an invalid
// commit/welcome or a failed transition sends opcode 31 and rejoins with a
// fresh key package. Only local failures (storage, key generation) return
// false. kVoiceDaveInvalidLimit consecutive reports (close 4803) or a refused
// downgrade to version 0 (close 4804) request a fresh session instead. Flush
// sends queued replies in order while the socket is idle.
bool voice_connection_dave_description(VoiceConnection* connection, u64 now_ms) noexcept;
bool voice_connection_dave_binary(VoiceConnection* connection, const VoiceGatewayBinary& frame,
                                  u64 now_ms) noexcept;
bool voice_connection_dave_json(VoiceConnection* connection, u8 opcode, JsonValue data, Slice<u32> scratch,
                                u64 now_ms) noexcept;
bool voice_connection_dave_flush(VoiceConnection* connection, u64 now_ms) noexcept;
void voice_connection_dave_reset(VoiceConnection* connection) noexcept;
VoiceParticipantsResult voice_connection_participant(VoiceConnection* connection, u8 opcode, JsonValue data,
                                                     Slice<u32> scratch, u32 maximum_depth) noexcept;
} // namespace maicord
