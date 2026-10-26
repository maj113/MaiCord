#include "maicord/protocol/voice_media_events.hpp"

#include "maicord/core/memory.hpp"
#include "json_field_lists.hpp"

namespace maicord {
namespace {
// Recognized wire names, not a claim that an encryptor is implemented.
u32 voice_transport_mode(JsonString name) noexcept {
    switch (protocol_detail::member_index<"aead_aes256_gcm_rtpsize", "aead_xchacha20_poly1305_rtpsize">(name)) {
    case 0: return voice_transport_aes256_gcm_rtpsize;
    case 1: return voice_transport_xchacha20_poly1305_rtpsize;
    default: return 0;
    }
}
struct Read {
    JsonIndexedDocument document;
    VoiceReady ready;
    VoiceSessionDescription description;
    u32 seen, depth, offset, keys;
    GatewayEventDecodeError error;
    u8 key[32];
};
bool reject(Read& read, GatewayEventDecodeError error, u32 offset) noexcept {
    read.error = error;
    read.offset = offset;
    return false;
}
bool string(JsonValue value, JsonString* output) noexcept {
    return json_value_as_string(value, output) && output->encoded.bytes;
}
bool strings(void*, u32, JsonValue value) noexcept {
    JsonString output{};
    return string(value, &output);
}
bool modes(void* opaque, u32, JsonValue value) noexcept {
    auto& read = *static_cast<Read*>(opaque);
    JsonString name{};
    if (!string(value, &name))
        return false;
    read.ready.recognized_modes |= voice_transport_mode(name);
    return true;
}
bool objects(void*, u32, JsonValue value) noexcept {
    return value.kind == JsonValueKind::object;
}
bool key_byte(void* opaque, u32 index, JsonValue value) noexcept {
    auto& read = *static_cast<Read*>(opaque);
    u64 byte = 0;
    if (index >= 32 || !json_value_to_u64(value, &byte) || byte > 255)
        return false;
    read.key[index] = static_cast<u8>(byte);
    ++read.keys;
    return true;
}
bool array(Read& read, JsonValue value, JsonArrayVisitor visitor, bool nonempty = false) noexcept {
    if (value.kind != JsonValueKind::array)
        return false;
    const auto result = json_visit_indexed_array(read.document, value, read.depth, visitor, &read);
    return result.ok() && (!nonempty || result.member_count);
}
bool ready_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& read = *static_cast<Read*>(opaque);
    const u32 index = protocol_detail::member_index<"ssrc", "ip", "port", "modes", "streams", "experiments">(name);
    if (index == 6)
        return true; // In particular, ignore Ready's obsolete heartbeat_interval.
    const u32 bit = 1u << index;
    if (read.seen & bit)
        return reject(read, GatewayEventDecodeError::duplicate_field, value.source_offset);
    read.seen |= bit;
    u64 number = 0;
    bool valid = false;
    switch (index) {
    case 0:
        valid = json_value_to_u64(value, &number) && number && number <= ~u32{0};
        read.ready.ssrc = static_cast<u32>(number);
        break;
    case 1:
        valid = string(value, &read.ready.ip);
        break;
    case 2:
        valid = json_value_to_u64(value, &number) && number && number <= 65535;
        read.ready.port = static_cast<u16>(number);
        break;
    case 3:
        valid = array(read, value, modes, true);
        break;
    case 4:
        valid = array(read, value, objects);
        break;
    case 5:
        valid = array(read, value, strings);
        break;
    }
    return valid || reject(read, GatewayEventDecodeError::invalid_data, value.source_offset);
}
bool description_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& read = *static_cast<Read*>(opaque);
    const u32 index = protocol_detail::member_index<"mode", "secret_key", "sdp", "audio_codec", "video_codec",
                                                    "media_session_id", "keyframe_interval",
                                                    "bandwidth_estimation_experiment", "dave_protocol_version">(name);
    if (index == 9)
        return true;
    const u32 bit = 1u << index;
    if (read.seen & bit)
        return reject(read, GatewayEventDecodeError::duplicate_field, value.source_offset);
    read.seen |= bit;
    auto& d = read.description;
    JsonString text{};
    u64 number = 0;
    bool valid = false;
    switch (index) {
    case 1:
        valid = array(read, value, key_byte) && read.keys == 32;
        break;
    case 4:
        d.has_video_codec = true;
        valid = string(value, &d.video_codec);
        break;
    case 6:
        d.has_keyframe_interval = true;
        valid = json_value_to_u64(value, &number) && number <= ~u32{0};
        d.keyframe_interval_ms = static_cast<u32>(number);
        break;
    case 8:
        valid = json_value_to_u64(value, &number) && number <= 65535;
        d.dave_protocol_version = static_cast<u16>(number);
        break;
    default: // mode, sdp, audio codec, media session id and bandwidth experiment are validated only.
        valid = string(value, &text);
        if (index == 0)
            d.recognized_mode = voice_transport_mode(text);
        break;
    }
    return valid || reject(read, GatewayEventDecodeError::invalid_data, value.source_offset);
}
GatewayEventDecodeResult decode(JsonValue data, Slice<u32> scratch, u32 depth, JsonMemberVisitor visitor,
                                Read& read) noexcept {
    if (data.kind != JsonValueKind::object)
        return {data.source_offset, GatewayEventDecodeError::invalid_input};
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok())
        return {data.source_offset + scan.error_offset, GatewayEventDecodeError::scan_failed, scan.error};
    read.document = {data.source, {scratch.data, scan.count}};
    read.depth = depth;
    const auto result =
        json_visit_value_object(data, read.document.structurals, depth, visitor, &read);
    if (!result.ok())
        return {data.source_offset +
                    (read.error == GatewayEventDecodeError::none ? result.error_offset : read.offset),
                read.error == GatewayEventDecodeError::none ? GatewayEventDecodeError::invalid_json
                                                            : read.error,
                JsonScanError::none, result.error};
    return {};
}
} // namespace
GatewayEventDecodeResult voice_decode_ready(JsonValue data, Slice<u32> scratch, u32 maximum_depth,
                                            VoiceReady* output) noexcept {
    Read read{};
    const auto result = decode(data, scratch, maximum_depth, ready_field, read);
    if (!result.ok())
        return result;
    if ((read.seen & 15) != 15)
        return {data.source_offset, GatewayEventDecodeError::missing_required_field};
    *output = read.ready;
    return {};
}
GatewayEventDecodeResult voice_decode_session_description(JsonValue data, Slice<u32> scratch,
                                                          u32 maximum_depth,
                                                          VoiceSessionDescription* output,
                                                          Slice<u8> key_output) noexcept {
    if (key_output.count && (!key_output.data || key_output.count != 32))
        return {data.source_offset, GatewayEventDecodeError::invalid_input};
    if (key_output.count) {
        const usize source = reinterpret_cast<usize>(data.source.data),
                    target = reinterpret_cast<usize>(key_output.data);
        if (source <= target ? target - source < data.source.bytes : source - target < key_output.count)
            return {data.source_offset, GatewayEventDecodeError::invalid_input};
    }
    Read read{};
    auto result = decode(data, scratch, maximum_depth, description_field, read);
    if (result.ok()) {
        const u32 transport = read.seen & 7;
        if (transport != 3 && transport != 4)
            result = {data.source_offset, GatewayEventDecodeError::missing_required_field};
        else {
            read.description.udp = transport == 3;
            *output = read.description;
            if (key_output.count)
                copy_bytes(key_output.data, read.key, 32);
        }
    }
    secure_clear_bytes(read.key, sizeof(read.key));
    return result;
}
} // namespace maicord
