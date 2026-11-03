#pragma once
#include "maicord/app/gateway/voice_control.hpp"

namespace maicord {

enum class VoiceSocketPhase : u8;
enum class VoiceConnectionError : u8;
enum class VoiceUdpPhase : u8;
enum class VoiceUdpError : u8;
enum class VoiceSourceKind : u8;
enum class VoiceVideoCodec : u8;
inline constexpr u32 kGatewayVoiceSpeakingUsers = 16;
struct GatewayClientVoiceSnapshot {
    u64 user_id, guild_id, channel_id, generation;
    u32 flags, present_flags;
    VoiceSetupPhase phase;
    GatewayVoiceError error;
    bool awaiting_acknowledgement;
    VoiceSocketPhase socket_phase;
    VoiceConnectionError connection_error;
    bool identify_submitted;
    u16 voice_close_code;
    VoiceUdpPhase udp_phase;
    VoiceUdpError udp_error;
    // Remote users currently heard speaking (received audio energy, or packet
    // activity with op 5 flags while deafened), sorted by ID.
    u32 speaking_count;
    u64 speaking_users[kGatewayVoiceSpeakingUsers];
    constexpr bool operator==(const GatewayClientVoiceSnapshot&) const noexcept = default;
};
static_assert(sizeof(GatewayClientVoiceSnapshot) == 184); // Crosses the account module ABI.
// The channel of the live call, guild or DM (guild 0), or 0 when there is none.
constexpr u64 gateway_voice_call_channel(const GatewayClientVoiceSnapshot& voice) noexcept {
    return voice.phase != VoiceSetupPhase::idle && voice.error == GatewayVoiceError::none ? voice.channel_id : 0;
}
// True while the voice worker lists the user among the call's speakers: it
// does as soon as it sends speech.
constexpr bool gateway_voice_self_speaking(const GatewayClientVoiceSnapshot& voice) noexcept {
    for (u32 i = 0; voice.user_id && i < voice.speaking_count && i < kGatewayVoiceSpeakingUsers; ++i)
        if (voice.speaking_users[i] == voice.user_id)
            return true;
    return false;
}
inline constexpr u32 kGatewayVoiceVideoUsers = 16;
// A remote participant announcing an active camera or screen stream (op 12):
// the UI's "video available" flag. frames stays zero until media arrives.
struct GatewayVoiceVideoUser {
    u64 user_id;
    u32 ssrc, frames;
    VoiceSourceKind kind; // video (camera) or screen.
    u8 quality;
    u8 reserved[6];
    constexpr bool operator==(const GatewayVoiceVideoUser&) const noexcept = default;
};
struct GatewayClientVoiceVideoSnapshot {
    u64 generation; // Voice setup generation the list belongs to.
    u32 count;      // Users sorted by ID.
    VoiceVideoCodec codec;
    u8 reserved[3];
    GatewayVoiceVideoUser users[kGatewayVoiceVideoUsers];
    // Integers without padding (asserted below): one byte compare instead of one per listed user.
    bool operator==(const GatewayClientVoiceVideoSnapshot& other) const noexcept {
        return bytes_equal(this, &other, sizeof(*this));
    }
};
static_assert(sizeof(GatewayVoiceVideoUser) == 24 && sizeof(GatewayClientVoiceVideoSnapshot) == 400); // ABI.
static_assert(__has_unique_object_representations(GatewayClientVoiceVideoSnapshot));

} // namespace maicord
