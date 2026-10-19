#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/gateway_commands.hpp"

namespace maicord {
// Voice channel extras: each GUILD_VOICE channel's status and the start of its
// current voice session. Opcode 43 asks for a guild's values; CHANNEL_INFO
// answers it and VOICE_CHANNEL_START_TIME_UPDATE / VOICE_CHANNEL_STATUS_UPDATE
// keep them current.
inline constexpr u32 kGatewayRequestChannelInfoOpcode = 43;
inline constexpr u32 kVoiceChannelStatusMaximumCodepoints = 500;
inline constexpr u32 kVoiceChannelStatusMaximumBytes = kVoiceChannelStatusMaximumCodepoints * 4;

// {"op":43,"d":{"guild_id":"<id>","fields":["status","voice_start_time"]}}
GatewayCommandWriteResult gateway_write_channel_info_request(u64 guild_id, Slice<u8> output) noexcept;

enum class VoiceChannelInfoEvent : u8 {
    channel_info,
    start_time_update,
    status_update
};
// One channel's values. A field that is absent leaves has_* false; null or an
// empty status clears it. voice_start_time is whole Unix seconds.
struct VoiceChannelInfoEntry {
    u64 guild_id, channel_id;
    u64 start_s;
    Utf8View status; // Decoded UTF-8 in the caller's text buffer.
    bool has_start, has_status;
};
using VoiceChannelInfoVisitor = bool (*)(void* context, const VoiceChannelInfoEntry& entry) noexcept;
enum class VoiceChannelInfoError : u8 {
    none,
    input,
    json,
    field,
    capacity
};
struct VoiceChannelInfoResult {
    u64 guild_id;
    u32 count;
    VoiceChannelInfoError error;
    constexpr bool ok() const noexcept {
        return error == VoiceChannelInfoError::none;
    }
};
// Visits CHANNEL_INFO's channels[] (each inherits the document's guild_id) or
// the single channel of an update. text holds one decoded status at a time
// (kVoiceChannelStatusMaximumBytes suffices); a longer one fails with capacity.
VoiceChannelInfoResult voice_channel_info_decode(VoiceChannelInfoEvent event, Utf8View data,
                                                 Slice<u32> scratch, Slice<char> text,
                                                 VoiceChannelInfoVisitor visitor, void* context) noexcept;
// Display text for a status: custom emoji markup <a:name:id> becomes :name:.
// Returns the bytes written (never more than status.bytes).
u32 voice_channel_status_display(Utf8View status, Slice<char> output) noexcept;
} // namespace maicord
