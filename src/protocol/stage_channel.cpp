#include "maicord/protocol/stage_channel.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/guild_admin.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {
bool self_operation(StageOperation operation) noexcept {
    return operation <= StageOperation::move_self_to_audience;
}
bool user_operation(StageOperation operation) noexcept {
    return operation == StageOperation::invite_to_speak || operation == StageOperation::move_to_audience;
}
bool instance_operation(StageOperation operation) noexcept {
    return operation >= StageOperation::start && operation <= StageOperation::end;
}
bool timestamp_operation(StageOperation operation) noexcept {
    return operation == StageOperation::request_to_speak || operation == StageOperation::invite_to_speak;
}
u8 privacy(const StageRequest& r) noexcept {
    return r.privacy_level ? r.privacy_level : kStagePrivacyGuildOnly;
}
// "request_to_speak_timestamp":"YYYY-MM-DDTHH:MM:SS+00:00"
bool append_timestamp(ProtocolWriter* w, u64 unix_ms) noexcept {
    char text[32];
    const u32 bytes = guild_admin_format_timestamp(unix_ms / 1000, {text, sizeof(text)});
    return bytes && protocol_writer_append_literal(w, "\"request_to_speak_timestamp\":") &&
           protocol_writer_append_json_string(w, {text, bytes});
}
} // namespace

bool stage_topic_valid(Utf8View topic) noexcept {
    if (!topic.data || !topic.bytes || topic.bytes > kStageTopicMaximumBytes || !utf8_validate(topic).valid)
        return false;
    u32 codepoints = 0;
    bool visible = false;
    for (u32 i = 0; i < topic.bytes; ++i) {
        const u8 c = static_cast<u8>(topic.data[i]);
        if ((c & 0xc0) != 0x80)
            ++codepoints;
        if (c == '\n' || c == '\r')
            return false;
        visible = visible || (c != ' ' && c != '\t');
    }
    return visible && codepoints <= kStageTopicMaximumCodepoints;
}

bool stage_voice_status_valid(Utf8View status) noexcept {
    if (!status.bytes)
        return true;
    if (!status.data || status.bytes > kStageVoiceStatusMaximumBytes || !utf8_validate(status).valid)
        return false;
    u32 codepoints = 0;
    for (u32 i = 0; i < status.bytes; ++i) {
        const u8 c = static_cast<u8>(status.data[i]);
        if (c == '\n' || c == '\r')
            return false;
        codepoints += (c & 0xc0) != 0x80;
    }
    return codepoints <= kStageVoiceStatusMaximumCodepoints;
}

bool stage_request_valid(const StageRequest& r) noexcept {
    if (r.operation >= StageOperation::count || !r.channel_id)
        return false;
    if (!instance_operation(r.operation) && !r.guild_id)
        return false;
    if (user_operation(r.operation) && !r.user_id)
        return false;
    if (timestamp_operation(r.operation) && r.unix_ms / 1000 >= 253'402'300'800ull)
        return false;
    if (r.operation == StageOperation::start || r.operation == StageOperation::update_topic)
        return stage_topic_valid(r.topic) && (r.privacy_level <= kStagePrivacyGuildOnly);
    if (r.operation == StageOperation::voice_status)
        return stage_voice_status_valid(r.topic);
    return true;
}

RestMethod stage_method(StageOperation operation) noexcept {
    switch (operation) {
    case StageOperation::start:
        return RestMethod::post;
    case StageOperation::end:
        return RestMethod::delete_method;
    case StageOperation::voice_status:
        return RestMethod::put;
    default:
        return RestMethod::patch;
    }
}

bool stage_has_body(StageOperation operation) noexcept {
    return operation != StageOperation::end && operation < StageOperation::count;
}

RestRouteKey stage_route(const StageRequest& r) noexcept {
    // Voice-state routes are guild-major; stage instances bucket per route.
    if (self_operation(r.operation))
        return {kStageRouteBase + 1, r.guild_id, RestMethod::patch, RestMajorParameter::guild};
    if (user_operation(r.operation))
        return {kStageRouteBase + 2, r.guild_id, RestMethod::patch, RestMajorParameter::guild};
    if (r.operation == StageOperation::start)
        return {kStageRouteBase + 3, 0, RestMethod::post, RestMajorParameter::none};
    if (r.operation == StageOperation::voice_status)
        return {kStageRouteBase + 5, r.channel_id, RestMethod::put, RestMajorParameter::channel};
    return {kStageRouteBase + 4, 0, stage_method(r.operation), RestMajorParameter::none};
}

StageWriteResult stage_write_target(const StageRequest& r, Slice<u8> output) noexcept {
    if (!stage_request_valid(r) || !output.data)
        return {0, StageWriteError::input};
    u8 staged[kStageMaximumTargetBytes];
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    bool ok = protocol_writer_append_literal(&w, "/api/v9");
    if (self_operation(r.operation) || user_operation(r.operation)) {
        ok = ok && protocol_writer_append_literal(&w, "/guilds/") && protocol_writer_append_u64(&w, r.guild_id) &&
             protocol_writer_append_literal(&w, "/voice-states/");
        ok = ok && (self_operation(r.operation) ? protocol_writer_append_literal(&w, "@me")
                                                : protocol_writer_append_u64(&w, r.user_id));
    } else if (r.operation == StageOperation::voice_status) {
        ok = ok && protocol_writer_append_literal(&w, "/channels/") && protocol_writer_append_u64(&w, r.channel_id) &&
             protocol_writer_append_literal(&w, "/voice-status");
    } else {
        ok = ok && protocol_writer_append_literal(&w, "/stage-instances");
        if (r.operation != StageOperation::start)
            ok = ok && protocol_writer_append_byte(&w, '/') && protocol_writer_append_u64(&w, r.channel_id);
    }
    return protocol_writer_finish<StageWriteResult>(w, ok, output, staged);
}

StageWriteResult stage_write_body(const StageRequest& r, Slice<u8> output) noexcept {
    if (!stage_request_valid(r) || !output.data || !stage_has_body(r.operation))
        return {0, StageWriteError::input};
    u8 staged[kStageMaximumBodyBytes];
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    bool ok = protocol_writer_append_byte(&w, '{');
    const auto channel = [&]() noexcept {
        return protocol_writer_append_literal(&w, "\"channel_id\":") &&
               protocol_writer_append_snowflake(&w, r.channel_id);
    };
    switch (r.operation) {
    case StageOperation::request_to_speak:
        ok = ok && append_timestamp(&w, r.unix_ms) && protocol_writer_append_byte(&w, ',') && channel();
        break;
    case StageOperation::lower_hand:
        ok = ok && protocol_writer_append_literal(&w, "\"request_to_speak_timestamp\":null,") && channel();
        break;
    case StageOperation::become_speaker:
    case StageOperation::decline_invite:
        ok = ok &&
             (r.operation == StageOperation::become_speaker ? protocol_writer_append_literal(&w, "\"suppress\":false,")
                                                            : protocol_writer_append_literal(&w, "\"suppress\":true,")) &&
             protocol_writer_append_literal(&w, "\"request_to_speak_timestamp\":null,") && channel();
        if (r.silent && r.operation == StageOperation::become_speaker)
            ok = ok && protocol_writer_append_literal(&w, ",\"silent\":true");
        break;
    case StageOperation::move_self_to_audience:
        ok = ok && protocol_writer_append_literal(&w, "\"suppress\":true,") && channel() &&
             protocol_writer_append_literal(&w, ",\"self_video\":false,\"self_stream\":false");
        break;
    case StageOperation::invite_to_speak:
        ok = ok && protocol_writer_append_literal(&w, "\"suppress\":false,") && append_timestamp(&w, r.unix_ms) &&
             protocol_writer_append_byte(&w, ',') && channel();
        break;
    case StageOperation::move_to_audience:
        ok = ok && protocol_writer_append_literal(&w, "\"suppress\":true,") && channel();
        break;
    case StageOperation::start:
        ok = ok && channel() && protocol_writer_append_literal(&w, ",\"topic\":") &&
             protocol_writer_append_json_string(&w, r.topic) &&
             protocol_writer_append_literal(&w, ",\"privacy_level\":") &&
             protocol_writer_append_u64(&w, privacy(r)) &&
             (r.send_start_notification ? protocol_writer_append_literal(&w, ",\"send_start_notification\":true")
                                        : protocol_writer_append_literal(&w, ",\"send_start_notification\":false"));
        break;
    case StageOperation::update_topic:
        ok = ok && protocol_writer_append_literal(&w, "\"topic\":") && protocol_writer_append_json_string(&w, r.topic) &&
             protocol_writer_append_literal(&w, ",\"privacy_level\":") && protocol_writer_append_u64(&w, privacy(r));
        break;
    case StageOperation::voice_status:
        ok = ok && protocol_writer_append_literal(&w, "\"status\":") &&
             protocol_writer_append_json_string(&w, r.topic.bytes ? r.topic : utf8_literal(""));
        break;
    default:
        ok = false;
        break;
    }
    ok = ok && protocol_writer_append_byte(&w, '}');
    return protocol_writer_finish<StageWriteResult>(w, ok, output, staged);
}

namespace {
struct InstanceDecode {
    GatewayStageInstance value{};
    u32 seen = 0;
    GatewayEventDecodeError error = GatewayEventDecodeError::none;
};
bool visit_instance(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& d = *static_cast<InstanceDecode*>(opaque);
    u32 field = 0;
    if (json_string_equals_ascii(key, utf8_literal("id")))
        field = 1;
    else if (json_string_equals_ascii(key, utf8_literal("guild_id")))
        field = 2;
    else if (json_string_equals_ascii(key, utf8_literal("channel_id")))
        field = 4;
    else if (json_string_equals_ascii(key, utf8_literal("topic")))
        field = 8;
    else if (json_string_equals_ascii(key, utf8_literal("privacy_level")))
        field = 16;
    if (!field)
        return true;
    if (d.seen & field) {
        d.error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    d.seen |= field;
    bool valid = false;
    u64 number = 0;
    switch (field) {
    case 1:
        valid = json_value_to_snowflake(value, &d.value.id);
        break;
    case 2:
        valid = json_value_to_snowflake(value, &d.value.guild_id);
        break;
    case 4:
        valid = json_value_to_snowflake(value, &d.value.channel_id);
        break;
    case 8:
        valid = value.kind == JsonValueKind::null_value || json_value_as_string(value, &d.value.topic);
        break;
    default:
        valid = value.kind == JsonValueKind::null_value || (json_value_to_u64(value, &number) && number <= 255);
        d.value.privacy_level = static_cast<u8>(number);
        break;
    }
    if (!valid)
        d.error = GatewayEventDecodeError::invalid_data;
    return valid;
}
} // namespace

GatewayEventDecodeResult gateway_decode_stage_instance(JsonValue data, Slice<u32> scratch, u32 depth,
                                                       GatewayStageInstance* output) noexcept {
    if (!output || !data.source.data || !data.source.bytes || !scratch.data || !scratch.count || !depth)
        return {0, GatewayEventDecodeError::invalid_input};
    if (data.kind != JsonValueKind::object)
        return {data.source_offset, GatewayEventDecodeError::invalid_data};
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok())
        return {data.source_offset + scan.error_offset, GatewayEventDecodeError::scan_failed, scan.error};
    InstanceDecode d{};
    const auto read = json_visit_value_object(data, {scratch.data, scan.count}, depth, visit_instance, &d);
    if (!read.ok())
        return {data.source_offset + read.error_offset,
                d.error == GatewayEventDecodeError::none ? GatewayEventDecodeError::invalid_json : d.error,
                JsonScanError::none, read.error};
    if ((d.seen & 4) == 0 || !d.value.channel_id)
        return {data.source_offset, GatewayEventDecodeError::missing_required_field};
    *output = d.value;
    return {};
}
} // namespace maicord
