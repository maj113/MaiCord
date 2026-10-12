#pragma once

#include "maicord/protocol/gateway_commands.hpp"
#include "maicord/protocol/gateway_endpoint.hpp"
#include "maicord/protocol/gateway_events.hpp"
#include "maicord/protocol/gateway_payload.hpp"

namespace maicord {
// VOICE_SERVER_UPDATE supplies an authority, not a URL. Ports 80/443 are
// normalized to secure port 443.
GatewayEndpointError voice_gateway_endpoint(Utf8View authority, GatewayEndpoint* output) noexcept;

struct VoiceGatewayEnvelope {
    JsonValue data;
    u16 sequence;
    u8 opcode;
    bool has_sequence;
};
// Voice uses "seq", not the main Gateway's "s". Both zero and wraparound
// values are significant. Results borrow the input and publish only on success.
GatewayDecodeResult voice_gateway_decode_json(Utf8View input, Slice<u32> scratch, u32 maximum_depth,
                                              VoiceGatewayEnvelope* output) noexcept;
GatewayEventDecodeResult voice_gateway_decode_heartbeat_ack(JsonValue data, Slice<u32> scratch,
    u32 maximum_depth, u64* nonce) noexcept;
struct VoiceGatewayBinary {
    ConstSlice<u8> data;
    u16 sequence;
    u8 opcode;
};
// Server binary frames: big-endian uint16 sequence, uint8 opcode, payload.
bool voice_gateway_decode_binary(ConstSlice<u8> input, VoiceGatewayBinary* output) noexcept;
// Client binary frames have only an opcode prefix, with no sequence header.
GatewayCommandWriteResult voice_gateway_write_binary(u8 opcode, ConstSlice<u8> payload,
                                                     Slice<u8> output) noexcept;

struct VoiceGatewaySession {
    u64 server_id, channel_id;
    Utf8View session_id, token;
};
enum class VoiceStreamType : u8 {
    video,
    screen
};
// Requested send stream ("rid" is a decimal string on the wire: "100" full,
// "50" simulcast low layer). SSRCs are assigned by the server.
struct VoiceIdentifyStream {
    VoiceStreamType type;
    u8 rid, quality;
};
struct VoiceGatewayIdentify {
    VoiceGatewaySession session;
    u64 user_id;
    u16 max_dave_protocol_version;
    bool video;
    // Always serialized ("streams":[] when empty).
    // The camera context requests {video, "100", 100}.
    ConstSlice<VoiceIdentifyStream> streams;
};
struct VoiceGatewaySelectProtocol {
    Utf8View address;
    u16 port;
    u32 mode;
};
// These writers measure before writing and leave all output unchanged on error.
// A null/zero output is a size query. Input strings must not alias the output.
// Advertised capabilities must come from implemented transport support; these
// codecs alone do not provide DAVE or a media connection.
GatewayCommandWriteResult voice_gateway_write_identify(const VoiceGatewayIdentify& identify,
                                                       Slice<u8> output) noexcept;
GatewayCommandWriteResult voice_gateway_write_resume(const VoiceGatewaySession& session, i32 sequence_ack,
                                                     Slice<u8> output) noexcept;
GatewayCommandWriteResult voice_gateway_write_heartbeat(u64 nonce, i32 sequence_ack,
                                                        Slice<u8> output) noexcept;

enum VoiceSpeakingFlag : u32 {
    voice_speaking_microphone = 1u << 0,
    voice_speaking_soundshare = 1u << 1,
    voice_speaking_priority = 1u << 2,
};
GatewayCommandWriteResult voice_gateway_write_speaking(u32 flags, u32 ssrc, Slice<u8> output) noexcept;

enum class VoiceCloseAction : u8 {
    stop,
    resume,          // Same session: Resume (op 7) with the last sequence.
    reconnect_fresh, // Same credentials: new socket and Identify.
};
// close_code zero means the transport ended without a close frame.
VoiceCloseAction voice_gateway_close_action(u16 close_code) noexcept;
} // namespace maicord
