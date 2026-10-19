#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/rest.hpp"
#include "maicord/protocol/gateway_events.hpp"

namespace maicord {
// Stage channels (type 13): stage instances and speaker/audience voice states.
inline constexpr u8 kStageChannelType = 13;
inline constexpr u32 kStageTopicMaximumCodepoints = 120;
inline constexpr u32 kStageTopicMaximumBytes = kStageTopicMaximumCodepoints * 4;
inline constexpr u32 kStageVoiceStatusMaximumCodepoints = 500;
inline constexpr u32 kStageVoiceStatusMaximumBytes = kStageVoiceStatusMaximumCodepoints * 4;
inline constexpr u32 kStageMaximumTargetBytes = 96;
inline constexpr u32 kStageMaximumBodyBytes = 4096;
inline constexpr u64 kStageRouteBase = 0x5354414745000000ull; // "STAGE"
// Stage privacy levels; PUBLIC (1) is retired, new stages are guild only.
inline constexpr u8 kStagePrivacyGuildOnly = 2;

enum class StageOperation : u8 {
    request_to_speak,      // PATCH @me {request_to_speak_timestamp: now}
    lower_hand,            // PATCH @me {request_to_speak_timestamp: null}
    become_speaker,        // PATCH @me {suppress: false, request_to_speak_timestamp: null} (accept invite)
    decline_invite,        // PATCH @me {suppress: true, request_to_speak_timestamp: null}
    move_self_to_audience, // PATCH @me {suppress: true, self_video: false, self_stream: false}
    invite_to_speak,       // PATCH {user} {suppress: false, request_to_speak_timestamp: now}
    move_to_audience,      // PATCH {user} {suppress: true}
    start,                 // POST /stage-instances
    update_topic,          // PATCH /stage-instances/{channel}
    end,                   // DELETE /stage-instances/{channel}
    voice_status,          // PUT /channels/{channel}/voice-status {status} (a voice channel, not a stage)
    count
};

// Crosses the account module ABI (append-only).
struct StageRequest {
    StageOperation operation;
    u8 privacy_level;             // start/update_topic; zero sends guild only.
    bool send_start_notification; // start
    bool silent;                  // become_speaker right after starting a stage
    u32 reserved;
    u64 guild_id, channel_id;
    u64 user_id;     // invite_to_speak / move_to_audience
    u64 unix_ms;     // request_to_speak / invite_to_speak timestamp
    Utf8View topic;  // start / update_topic: 1..120 code points; voice_status: 0..500 (empty clears)
};
static_assert(sizeof(StageRequest) == 56);

enum class StageWriteError : u8 { none, input, capacity };
struct StageWriteResult {
    u32 bytes;
    StageWriteError error;
    constexpr bool ok() const noexcept {
        return error == StageWriteError::none;
    }
};

bool stage_topic_valid(Utf8View topic) noexcept;
// A voice channel status: one line of at most 500 code points; empty clears it.
bool stage_voice_status_valid(Utf8View status) noexcept;
bool stage_request_valid(const StageRequest&) noexcept;
RestMethod stage_method(StageOperation) noexcept;
bool stage_has_body(StageOperation) noexcept;
RestRouteKey stage_route(const StageRequest&) noexcept;
// Writers leave output untouched on failure.
StageWriteResult stage_write_target(const StageRequest&, Slice<u8> output) noexcept;
StageWriteResult stage_write_body(const StageRequest&, Slice<u8> output) noexcept;

// STAGE_INSTANCE_CREATE/UPDATE/DELETE data and guild stage_instances[] rows.
// The topic borrows the input document.
struct GatewayStageInstance {
    u64 id, guild_id, channel_id;
    JsonString topic;
    u8 privacy_level;
};
GatewayEventDecodeResult gateway_decode_stage_instance(JsonValue data, Slice<u32> scratch, u32 maximum_depth,
                                                       GatewayStageInstance* output) noexcept;
} // namespace maicord
