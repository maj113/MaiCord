#pragma once
#include "maicord/protocol/voice_participants.hpp"

namespace maicord {
struct VoiceParticipant {
    u64 user_id;
    u32 speaking, flags, platform;
    bool connected, has_flags, has_platform;
};
struct VoiceSsrcBinding {
    u64 user_id, revision;
    u32 ssrc, primary_ssrc;
    VoiceSourceKind kind;
    bool active;
    u8 quality; // Video/screen simulcast quality (100 for audio and legacy video).
};
// Worker-only owned sorted arrays. Lookups return values, not pointers that a
// later membership update could invalidate. No packet-driven allocation/state.
struct VoiceParticipants {
    VoiceParticipant* users;
    VoiceSsrcBinding* sources;
    u64 generation, source_revision;
    u32 user_count, user_capacity, source_count, source_capacity;
};
enum class VoiceParticipantsError : u8 {
    none,
    invalid_input,
    stale_generation,
    decode,
    storage,
    conflicting_sources,
    revision_exhausted
};
struct VoiceParticipantsResult {
    VoiceParticipantsError error;
    GatewayEventDecodeResult decode;
    constexpr bool ok() const noexcept {
        return error == VoiceParticipantsError::none;
    }
};
// Full event validation and capacity reservation precede logical mutation.
// Connected membership (opcode 11) is distinct from an SSRC announcement or
// activity/flags. A valid mapping is not proof of DAVE membership or replay safety.
VoiceParticipantsResult voice_participants_event(VoiceParticipants* participants, u64 generation, u8 opcode,
                                                 JsonValue data, Slice<u32> scratch,
                                                 u32 maximum_depth) noexcept;
VoiceParticipant voice_participants_user(const VoiceParticipants& participants, u64 user_id) noexcept;
VoiceSsrcBinding voice_participants_source(const VoiceParticipants& participants, u32 ssrc) noexcept;
// A new transport Ready invalidates sources, but preserves call membership.
// Reissued source revisions cannot collide with jobs from before that Ready.
void voice_participants_clear_sources(VoiceParticipants* participants) noexcept;
void voice_participants_release(VoiceParticipants* participants) noexcept;
} // namespace maicord
