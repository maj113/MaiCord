#pragma once
#include "maicord/protocol/voice_gateway.hpp"

namespace maicord {
// DAVE voice gateway opcodes (protocol v1).
enum class VoiceDaveOpcode : u8 {
    prepare_transition = 21,
    execute_transition = 22,
    transition_ready = 23,
    prepare_epoch = 24,
    mls_external_sender = 25,
    mls_key_package = 26,
    mls_proposals = 27,
    mls_commit_welcome = 28,
    mls_announce_commit_transition = 29,
    mls_welcome = 30,
    mls_invalid_commit_welcome = 31,
};
enum class VoiceDaveRoute : u8 {
    not_dave,
    deferred, // Valid server DAVE message held for the MLS group owner.
    rejected, // Client-only opcode or truncated payload from the server.
};
// Classification counters for routed DAVE messages. Holds no key material;
// deferred messages are handed to the call's DaveSession by the connection.
struct VoiceDaveInbox {
    u64 deferred, rejected;
    u16 last_transition_id;
    u8 last_opcode;
};
VoiceDaveRoute voice_dave_route_binary(VoiceDaveInbox* inbox, const VoiceGatewayBinary& frame) noexcept;
VoiceDaveRoute voice_dave_route_json(VoiceDaveInbox* inbox, u8 opcode) noexcept;

// Server JSON DAVE commands: prepare_transition (21: transition_id,
// protocol_version), execute_transition (22: transition_id), prepare_epoch
// (24: protocol_version, epoch; transition_id optional).
struct VoiceDaveCommand {
    u64 epoch;
    u16 transition_id, protocol_version;
    bool has_transition, has_version, has_epoch;
};
GatewayEventDecodeResult voice_dave_decode_command(u8 opcode, JsonValue data, Slice<u32> scratch,
                                                   u32 maximum_depth, VoiceDaveCommand* output) noexcept;
// Client ready_for_transition (23) and invalid_commit_welcome (31).
GatewayCommandWriteResult voice_dave_write_transition(VoiceDaveOpcode opcode, u16 transition_id,
                                                      Slice<u8> output) noexcept;
} // namespace maicord
