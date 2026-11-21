#pragma once
#include "maicord/ui/voice/panel.hpp"

namespace maicord {
// The sounds of the voice connection. Each value is its clip in the notification sound bank
// (maicord/app/notifications/sound_assets.hpp).
enum class VoiceConnectionSound : u8 { none = 0, join = 3, leave = 4, interrupted = 5 };
// The last look at the call: its channel, the generation of its voice server credentials, whether it was connected
// and whether it is a stage channel.
struct VoiceConnectionSoundTracker {
    u64 channel_id, generation;
    bool connected, stage;
};
// The sound for one look at the call: its channel (0 with no call), the generation of its voice server credentials,
// whether the channel is a stage and the voice card's state. The tracker takes the look. As in the official client,
// only a change of the connected state plays a sound, and stage channels play none.
// - join: the call becomes connected, also again after a drop or a move.
// - interrupted: the connected call starts to connect again or fails on the same channel and voice server. Only a
//   lost voice Gateway socket or a UDP or media failure does that.
// - leave: the connected call ends or moves to another channel or voice server.
constexpr VoiceConnectionSound voice_connection_sound_step(VoiceConnectionSoundTracker* tracker, u64 channel_id,
                                                           u64 generation, bool stage,
                                                           UiVoicePanelState state) noexcept {
    const VoiceConnectionSoundTracker last = *tracker;
    const bool connected = state == UiVoicePanelState::connected;
    *tracker = {channel_id, generation, connected, stage};
    if (connected && (!last.connected || channel_id != last.channel_id))
        return stage ? VoiceConnectionSound::none : VoiceConnectionSound::join;
    if (connected || !last.connected || last.stage)
        return VoiceConnectionSound::none;
    const bool dropped =
        state != UiVoicePanelState::hidden && channel_id == last.channel_id && generation == last.generation;
    return dropped ? VoiceConnectionSound::interrupted : VoiceConnectionSound::leave;
}
} // namespace maicord
