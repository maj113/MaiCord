#include "maicord/protocol/voice_participants.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/core/byte_range.hpp"
#include "json_field_lists.hpp"

namespace maicord {
namespace {
constexpr u32 user = 1, speaking = 2, ssrc = 4, users = 8, audio = 16, video = 32, streams = 64,
              flags = 128, platform = 256, delay = 512;
struct Read {
    VoiceParticipantEvent event;
    u32 seen, allowed, required, error_offset;
    GatewayEventDecodeError error;
};
bool number(JsonValue value, u32* output) noexcept {
    u64 n = 0;
    if (!json_value_to_u64(value, &n) || n > ~u32{0})
        return false;
    *output = static_cast<u32>(n);
    return true;
}
struct SourceRead {
    VoiceVideoSource source{0, 0, VoiceSourceKind::video, true, 100};
    u32 seen;
};
bool source_field(void* context, JsonString name, JsonValue value) noexcept {
    auto& read = *static_cast<SourceRead*>(context);
    const u32 index = protocol_detail::member_index<"ssrc", "rtx_ssrc", "type", "active", "quality">(name);
    if (index == 5)
        return true;
    if (read.seen & (1u << index))
        return false;
    read.seen |= 1u << index;
    // Null keeps the default (no SSRC, active, video, full quality).
    if (value.kind == JsonValueKind::null_value)
        return true;
    if (index == 4) {
        u32 quality = 0;
        if (!number(value, &quality) || quality > 100)
            return false;
        read.source.quality = static_cast<u8>(quality);
        return true;
    }
    if (index == 0)
        return number(value, &read.source.ssrc);
    if (index == 1)
        return number(value, &read.source.rtx_ssrc);
    if (index == 3)
        return json_value_to_bool(value, &read.source.active);
    JsonString type{};
    if (!json_value_as_string(value, &type) || !type.encoded.bytes)
        return false;
    // Unknown or missing types stay video.
    switch (protocol_detail::member_index<"audio", "screen", "test">(type)) {
    case 0: read.source.kind = VoiceSourceKind::audio; break;
    case 1: read.source.kind = VoiceSourceKind::screen; break;
    case 2: read.source.kind = VoiceSourceKind::test; break;
    default: break;
    }
    return true;
}
bool source(const VoiceParticipantEvent& event, JsonValue value, VoiceVideoSource* output) noexcept {
    if (value.kind != JsonValueKind::object)
        return false;
    SourceRead read{};
    const auto result =
        json_visit_indexed_object(event.document, value, event.maximum_depth, source_field, &read);
    if (!result.ok())
        return false;
    *output = read.source;
    return true;
}
bool valid_user(void*, u32, JsonValue value) noexcept {
    u64 id = 0;
    return json_value_to_snowflake(value, &id);
}
bool valid_source(void* context, u32, JsonValue value) noexcept {
    VoiceVideoSource parsed{};
    return source(*static_cast<VoiceParticipantEvent*>(context), value, &parsed);
}
bool field(void* context, JsonString name, JsonValue value) noexcept {
    auto& read = *static_cast<Read*>(context);
    auto& event = read.event;
    const u32 index = protocol_detail::member_index<"user_id", "speaking", "ssrc", "user_ids", "audio_ssrc", "video_ssrc",
                                                    "streams", "flags", "platform", "delay">(name);
    const u32 bit = 1u << index;
    if (!(read.allowed & bit))
        return true;
    read.error_offset = value.source_offset;
    if (read.seen & bit) {
        read.error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    read.seen |= bit;
    // Null announces a field without a value (as for a member without video
    // or platform); identities and the connected-user list must be real.
    if (value.kind == JsonValueKind::null_value && bit != user && bit != users)
        return true;
    bool valid = false;
    switch (bit) {
    case user:
        valid = json_value_to_snowflake(value, &event.user_id);
        break;
    case speaking: {
        bool legacy = false;
        if (json_value_to_bool(value, &legacy)) {
            event.value = legacy ? 1 : 0;
            valid = true;
        } else
            valid = number(value, &event.value);
        break;
    }
    case ssrc:
    case audio:
        valid = number(value, &event.audio_ssrc);
        break;
    case video:
        valid = number(value, &event.video_ssrc);
        break;
    case flags:
    case platform:
        valid = number(value, &event.value);
        break;
    case delay: {
        u32 ignored = 0;
        valid = number(value, &ignored);
        break;
    }
    case users:
    case streams: {
        if (value.kind != JsonValueKind::array)
            break;
        const auto result = json_visit_indexed_array(event.document, value, event.maximum_depth,
                                                     bit == users ? valid_user : valid_source, &event);
        valid = result.ok();
        if (bit == users) {
            event.users = value;
            event.user_count = result.member_count;
        } else {
            event.streams = value;
            event.stream_count = result.member_count;
        }
        break;
    }
    }
    if (!valid)
        read.error = GatewayEventDecodeError::invalid_data;
    return valid;
}
struct UsersVisit {
    VoiceParticipantVisitor visitor;
    void* context;
};
bool visit_user(void* context, u32, JsonValue value) noexcept {
    auto& visit = *static_cast<UsersVisit*>(context);
    u64 id = 0;
    return json_value_to_snowflake(value, &id) && visit.visitor(visit.context, id);
}
struct SourcesVisit {
    const VoiceParticipantEvent* event;
    VoiceSourceVisitor visitor;
    void* context;
};
bool visit_source(void* context, u32, JsonValue value) noexcept {
    auto& visit = *static_cast<SourcesVisit*>(context);
    VoiceVideoSource parsed{};
    return source(*visit.event, value, &parsed) && visit.visitor(visit.context, parsed);
}
} // namespace
GatewayEventDecodeResult voice_decode_participant(u8 opcode, JsonValue data, Slice<u32> scratch,
                                                  u32 maximum_depth,
                                                  VoiceParticipantEvent* output) noexcept {
    Read read{};
    switch (opcode) {
    case 5:
        read.allowed = user | speaking | ssrc | delay;
        read.required = speaking | ssrc;
        break;
    case 11:
        read.allowed = read.required = users;
        break;
    case 12:
        read.allowed = user | audio | video | streams;
        read.required = user | audio | video;
        break;
    case 13:
        read.allowed = read.required = user;
        break;
    case 18:
        read.allowed = read.required = user | flags;
        break;
    case 20:
        read.allowed = read.required = user | platform;
        break;
    default:
        return {data.source_offset, GatewayEventDecodeError::invalid_input};
    }
    if (!output || !maximum_depth || data.kind != JsonValueKind::object || !data.source.bytes ||
        scratch.count > ~u32{0} / sizeof(u32))
        return {data.source_offset, GatewayEventDecodeError::invalid_input};
    const ConstSlice<u8> input{reinterpret_cast<const u8*>(data.source.data), data.source.bytes};
    const ConstSlice<u8> indices{reinterpret_cast<const u8*>(scratch.data),
                                 scratch.count * static_cast<u32>(sizeof(u32))};
    const ConstSlice<u8> destination{reinterpret_cast<const u8*>(output), sizeof(*output)};
    if (!byte_range_valid(input) || !byte_range_valid(indices) || byte_ranges_overlap(input, indices) ||
        byte_ranges_overlap(input, destination) || byte_ranges_overlap(indices, destination))
        return {data.source_offset, GatewayEventDecodeError::invalid_input};
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok())
        return {data.source_offset + scan.error_offset, GatewayEventDecodeError::scan_failed, scan.error};
    read.event.document = {data.source, {scratch.data, scan.count}};
    read.event.maximum_depth = maximum_depth;
    read.event.opcode = opcode;
    const auto result =
        json_visit_value_object(data, read.event.document.structurals, maximum_depth, field, &read);
    if (!result.ok())
        return {data.source_offset +
                    (read.error == GatewayEventDecodeError::none ? result.error_offset : read.error_offset),
                read.error == GatewayEventDecodeError::none ? GatewayEventDecodeError::invalid_json
                                                            : read.error,
                JsonScanError::none, result.error};
    if ((read.seen & read.required) != read.required)
        return {data.source_offset, GatewayEventDecodeError::missing_required_field};
    *output = read.event;
    return {};
}
bool voice_visit_participant_users(const VoiceParticipantEvent& event, VoiceParticipantVisitor visitor,
                                   void* context) noexcept {
    if (event.opcode != 11)
        return false;
    UsersVisit visit{visitor, context};
    return json_visit_indexed_array(event.document, event.users, event.maximum_depth, visit_user, &visit)
        .ok();
}
bool voice_visit_participant_sources(const VoiceParticipantEvent& event, VoiceSourceVisitor visitor,
                                     void* context) noexcept {
    if (event.opcode != 12)
        return false;
    if (event.streams.kind != JsonValueKind::array)
        return event.stream_count == 0;
    SourcesVisit visit{&event, visitor, context};
    return json_visit_indexed_array(event.document, event.streams, event.maximum_depth, visit_source,
                                    &visit)
        .ok();
}
} // namespace maicord
