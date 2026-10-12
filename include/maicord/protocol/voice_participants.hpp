#pragma once
#include "maicord/protocol/gateway_events.hpp"

namespace maicord {
enum class VoiceSourceKind : u8 {
    audio,
    video,
    screen,
    test,
    rtx
};
struct VoiceVideoSource {
    u32 ssrc, rtx_ssrc;
    VoiceSourceKind kind;
    bool active;
    u8 quality; // Simulcast layer quality 0-100; absent means 100.
};
struct VoiceParticipantEvent {
    JsonIndexedDocument document;
    JsonValue users, streams;
    u64 user_id;
    u32 audio_ssrc, video_ssrc, value, user_count, stream_count, maximum_depth;
    u8 opcode;
};
// Decode server opcodes 5, 11, 12, 13, 18 and 20. Full validation precedes
// publishing output or visiting consumers. Document/array views borrow input
// and caller structural scratch. Unknown fields remain forward-compatible.
// Speaking permits a missing user_id (self/legacy echo), never a malformed ID.
GatewayEventDecodeResult voice_decode_participant(u8 opcode, JsonValue data, Slice<u32> scratch,
                                                  u32 maximum_depth,
                                                  VoiceParticipantEvent* output) noexcept;
using VoiceParticipantVisitor = bool (*)(void*, u64) noexcept;
using VoiceSourceVisitor = bool (*)(void*, const VoiceVideoSource&) noexcept;
bool voice_visit_participant_users(const VoiceParticipantEvent& event, VoiceParticipantVisitor visitor,
                                   void* context) noexcept;
bool voice_visit_participant_sources(const VoiceParticipantEvent& event, VoiceSourceVisitor visitor,
                                     void* context) noexcept;
} // namespace maicord
