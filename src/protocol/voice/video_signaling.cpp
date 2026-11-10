#include "maicord/protocol/voice/video_signaling.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "../encoding/json_command_writer.hpp"
#include "../encoding/json_field_lists.hpp"

namespace maicord {
namespace {
using protocol_detail::EncodedString;
using protocol_detail::write_json_command;
using Writer = protocol_detail::JsonCommandWriter;
struct CodecName {
    const char* text;
    u32 bytes;
};
CodecName codec_name(VoiceVideoCodec codec) noexcept {
    switch (codec) {
    case VoiceVideoCodec::h264:
        return {"H264", 4};
    case VoiceVideoCodec::h265:
        return {"H265", 4};
    case VoiceVideoCodec::vp8:
        return {"VP8", 3};
    case VoiceVideoCodec::vp9:
        return {"VP9", 3};
    case VoiceVideoCodec::av1:
        return {"AV1", 3};
    default:
        return {nullptr, 0};
    }
}
bool uuid_valid(Utf8View id) noexcept {
    if (id.bytes != kVoiceRtcConnectionIdBytes || !id.data)
        return false;
    for (u32 i = 0; i != id.bytes; ++i) {
        const char c = id.data[i];
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? c != '-' : !ascii_is_lower_hex_digit(c))
            return false;
    }
    return true;
}
void transport_fields(Writer& writer, const EncodedString& address, const VoiceGatewaySelectProtocol& selection) noexcept {
    writer.put("\"address\":", address, ",\"port\":", selection.port, ",\"mode\":");
    if (selection.mode == voice_transport_aes256_gcm_rtpsize)
        writer.put("\"aead_aes256_gcm_rtpsize\"");
    else
        writer.put("\"aead_xchacha20_poly1305_rtpsize\"");
}
} // namespace

VoiceVideoCodec voice_video_codec(JsonString name) noexcept {
    if (!name.encoded.bytes)
        return VoiceVideoCodec::none;
    static constexpr VoiceVideoCodec codecs[]{VoiceVideoCodec::h264, VoiceVideoCodec::h265, VoiceVideoCodec::vp8,
                                       VoiceVideoCodec::vp9, VoiceVideoCodec::av1};
    for (const auto codec : codecs) {
        const auto text = codec_name(codec);
        if (json_string_equals_ascii(name, {text.text, text.bytes}))
            return codec;
    }
    return VoiceVideoCodec::unknown;
}

u32 voice_video_decode_offers(u32 decodable, Slice<VoiceCodecOffer> output) noexcept {
    static constexpr VoiceVideoCodec order[]{VoiceVideoCodec::av1, VoiceVideoCodec::h265, VoiceVideoCodec::h264};
#if defined(_WIN32)
    decodable |= voice_video_codec_bit(VoiceVideoCodec::h264);
    u32 count = 0;
    for (const auto codec : order)
        if ((decodable & voice_video_codec_bit(codec)) && count < output.count)
            output[count++] = {codec, false, true};
    return count;
#else
    // The decoders decide: nothing is offered that nothing decodes. Hardware codecs first, then the software-only ones.
    u32 count = 0;
    for (u32 software = 0; software != 2; ++software)
        for (const auto codec : order) {
            const bool soft = (decodable & voice_video_software_bit(codec)) != 0;
            if ((decodable & voice_video_codec_bit(codec)) && soft == (software != 0) && count < output.count)
                output[count++] = {codec, false, true};
        }
    return count;
#endif
}

u32 voice_video_offers(u32 decodable, u32 encodable, Slice<VoiceCodecOffer> output) noexcept {
    static constexpr VoiceVideoCodec order[]{VoiceVideoCodec::av1, VoiceVideoCodec::h265, VoiceVideoCodec::h264};
    VoiceCodecOffer decodes[kVoiceVideoMaximumCodecs];
    const u32 decode_count = voice_video_decode_offers(decodable, {decodes, kVoiceVideoMaximumCodecs});
    u32 decoded = 0;
    for (u32 i = 0; i != decode_count; ++i)
        decoded |= voice_video_codec_bit(decodes[i].codec);
    // Hardware codecs (encoded, or decoded without software) in Discord's order, then software-only decodes.
    u32 count = 0;
    for (u32 pass = 0; pass != 2; ++pass)
        for (const auto codec : order) {
            const bool encode = (encodable & voice_video_codec_bit(codec)) != 0,
                       decode = (decoded & voice_video_codec_bit(codec)) != 0;
            const bool software = decode && (decodable & voice_video_software_bit(codec)) != 0;
            const bool hardware = encode || (decode && !software);
            if ((pass ? !hardware && decode : hardware) && count < output.count)
                output[count++] = {codec, encode, decode};
        }
    return count;
}

bool voice_rtc_connection_id(ConstSlice<u8> random, Slice<char> output) noexcept {
    if (random.count != 16 || !random.data || output.count < kVoiceRtcConnectionIdBytes || !output.data)
        return false;
    static constexpr char hex[] = "0123456789abcdef";
    u8 bytes[16];
    copy_bytes(bytes, random.data, 16);
    bytes[6] = static_cast<u8>((bytes[6] & 0x0f) | 0x40); // Version 4.
    bytes[8] = static_cast<u8>((bytes[8] & 0x3f) | 0x80); // RFC 4122 variant.
    u32 at = 0;
    for (u32 i = 0; i != 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            output.data[at++] = '-';
        output.data[at++] = hex[bytes[i] >> 4];
        output.data[at++] = hex[bytes[i] & 15];
    }
    return true;
}

GatewayCommandWriteResult voice_gateway_write_select_protocol_media(const VoiceSelectProtocolMedia& selection,
                                                                    Slice<u8> output) noexcept {
    const auto& transport = selection.transport;
    EncodedString address{};
    const ConstSlice<u8> out{output.data, output.count};
    const ConstSlice<u8> address_bytes{reinterpret_cast<const u8*>(transport.address.data), transport.address.bytes};
    const ConstSlice<u8> id_bytes{reinterpret_cast<const u8*>(selection.rtc_connection_id.data),
                                  selection.rtc_connection_id.bytes};
    const ConstSlice<u8> codec_bytes{reinterpret_cast<const u8*>(selection.video.data),
                                     selection.video.count * static_cast<u32>(sizeof(VoiceCodecOffer))};
    if (!transport.port || transport.address.bytes > 63 || !byte_range_valid(address_bytes) ||
        !protocol_detail::encoded_string(transport.address, true, &address) || !byte_range_valid(out) ||
        byte_ranges_overlap(address_bytes, out) || byte_ranges_overlap(id_bytes, out) ||
        selection.video.count > kVoiceVideoMaximumCodecs || (selection.video.count && !selection.video.data) ||
        byte_ranges_overlap(codec_bytes, out) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(&selection), sizeof(selection)}, out) ||
        !uuid_valid(selection.rtc_connection_id) ||
        (transport.mode != voice_transport_aes256_gcm_rtpsize &&
         transport.mode != voice_transport_xchacha20_poly1305_rtpsize))
        return {0, GatewayCommandError::invalid_input};
    for (u32 i = 0; i != selection.video.count; ++i) {
        const auto& codec = selection.video[i];
        if (!codec_name(codec.codec).text || (!codec.encode && !codec.decode))
            return {0, GatewayCommandError::invalid_input};
        for (u32 j = 0; j != i; ++j)
            if (selection.video[j].codec == codec.codec)
                return {0, GatewayCommandError::invalid_input};
    }
    return write_json_command(output, [&](Writer& writer) noexcept {
        writer.put("{\"op\":1,\"d\":{\"protocol\":\"udp\",\"data\":{");
        transport_fields(writer, address, transport);
        writer.put("},");
        transport_fields(writer, address, transport);
        // Opus carries no RTX type or encode/decode flags (omitted).
        writer.put(",\"codecs\":[{\"name\":\"opus\",\"type\":\"audio\",\"priority\":1000,\"payload_type\":120}");
        for (u32 i = 0; i != selection.video.count; ++i) {
            const auto& codec = selection.video[i];
            const auto name = codec_name(codec.codec);
            const u32 payload = kVoiceVideoPayloadTypeBase + 2 * i;
            writer.put(",{\"name\":\"", Utf8View{name.text, name.bytes}, "\",\"type\":\"video\",\"priority\":",
                       1000u * (i + 1), ",\"payload_type\":", payload, ",\"rtx_payload_type\":", payload + 1,
                       ",\"encode\":", codec.encode, ",\"decode\":", codec.decode, '}');
        }
        writer.put("],\"rtc_connection_id\":\"", Utf8View{selection.rtc_connection_id.data, kVoiceRtcConnectionIdBytes},
                   "\",\"experiments\":[]}}");
    });
}

GatewayCommandWriteResult voice_gateway_write_media_sink_wants(u8 any_quality, ConstSlice<VoiceSinkWant> wants,
                                                               const Slice<u8>& output) noexcept {
    const ConstSlice<u8> want_bytes{reinterpret_cast<const u8*>(wants.data),
                                    wants.count * static_cast<u32>(sizeof(VoiceSinkWant))};
    if (any_quality > 100 || (wants.count && !wants.data) || wants.count > 4096 ||
        !byte_range_valid(want_bytes) || !byte_range_valid({output.data, output.count}) ||
        byte_ranges_overlap(want_bytes, {output.data, output.count}))
        return {0, GatewayCommandError::invalid_input};
    for (u32 i = 0; i != wants.count; ++i) {
        // SSRC 2^32-1 is not a JavaScript array index and would sort after "any".
        if (!wants[i].ssrc || wants[i].ssrc == ~u32{0} || wants[i].quality > 100)
            return {0, GatewayCommandError::invalid_input};
        for (u32 j = 0; j != i; ++j)
            if (wants[j].ssrc == wants[i].ssrc)
                return {0, GatewayCommandError::invalid_input};
    }
    return write_json_command(output, [&](Writer& writer) noexcept {
        writer.put("{\"op\":15,\"d\":{");
        // Selection in ascending SSRC order; the list is small and unsorted.
        u32 previous = 0;
        for (u32 written = 0; written != wants.count; ++written) {
            u32 best = ~u32{0};
            u8 quality = 0;
            for (u32 i = 0; i != wants.count; ++i)
                if (wants[i].ssrc > previous && wants[i].ssrc <= best) {
                    best = wants[i].ssrc;
                    quality = wants[i].quality;
                }
            writer.put('"', best, "\":", quality, ',');
            previous = best;
        }
        writer.put("\"any\":", any_quality, "}}");
    });
}

GatewayCommandWriteResult voice_gateway_write_video(const VoiceVideoState& state, Slice<u8> output) noexcept {
    const auto& stream = state.stream;
    const bool sending = stream.ssrc != 0;
    if (!byte_range_valid({output.data, output.count}) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(&state), sizeof(state)}, {output.data, output.count}) ||
        (sending && (!stream.rtx_ssrc || stream.quality > 100)))
        return {0, GatewayCommandError::invalid_input};
    return write_json_command(output, [&](Writer& writer) noexcept {
        writer.put("{\"op\":12,\"d\":{\"audio_ssrc\":", state.audio_ssrc, ",\"video_ssrc\":", stream.ssrc,
                   ",\"rtx_ssrc\":", stream.rtx_ssrc, ",\"streams\":[");
        if (sending) {
            const bool source = !stream.max_width || !stream.max_height;
            writer.put("{\"type\":\"video\",\"rid\":\"100\",\"ssrc\":", stream.ssrc, ",\"active\":", stream.active,
                       ",\"quality\":", stream.quality, ",\"rtx_ssrc\":", stream.rtx_ssrc, ",\"max_bitrate\":",
                       stream.max_bitrate, ",\"max_framerate\":", stream.max_framerate,
                       ",\"max_resolution\":{\"type\":",
                       source ? Utf8View{"\"source\"", 8} : Utf8View{"\"fixed\"", 7}, ",\"width\":",
                       source ? 0u : u32{stream.max_width}, ",\"height\":", source ? 0u : u32{stream.max_height},
                       "}}");
        }
        writer.put("]}}");
    });
}

namespace {
struct UpdateRead {
    VoiceSessionUpdate update;
    u32 seen, offset;
    GatewayEventDecodeError error;
};
bool update_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& read = *static_cast<UpdateRead*>(opaque);
    const u32 index = protocol_detail::member_index<"audio_codec", "video_codec", "media_session_id",
                                                    "keyframe_interval">(name);
    if (index == 4)
        return true;
    read.offset = value.source_offset;
    if (read.seen & (1u << index)) {
        read.error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    read.seen |= 1u << index;
    if (value.kind == JsonValueKind::null_value)
        return true;
    auto& update = read.update;
    bool valid = false;
    if (index == 3) {
        u64 number = 0;
        valid = json_value_to_u64(value, &number) && number <= ~u32{0};
        update.keyframe_interval_ms = static_cast<u32>(number);
        update.has_keyframe_interval = valid;
    } else {
        // The audio codec and media session id are validated only.
        JsonString ignored{};
        JsonString* target = index == 1 ? &update.video_codec : &ignored;
        valid = json_value_as_string(value, target) && target->encoded.bytes;
        if (valid && index == 1) {
            update.has_video_codec = true;
            update.video = voice_video_codec(update.video_codec);
        }
    }
    if (!valid)
        read.error = GatewayEventDecodeError::invalid_data;
    return valid;
}
} // namespace

GatewayEventDecodeResult voice_decode_session_update(JsonValue data, Slice<u32> scratch, u32 maximum_depth,
                                                     VoiceSessionUpdate* output) noexcept {
    if (data.kind != JsonValueKind::object)
        return {data.source_offset, GatewayEventDecodeError::invalid_input};
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok())
        return {data.source_offset + scan.error_offset, GatewayEventDecodeError::scan_failed, scan.error};
    UpdateRead read{};
    const auto result =
        json_visit_value_object(data, {scratch.data, scan.count}, maximum_depth, update_field, &read);
    if (!result.ok())
        return {data.source_offset + (read.error == GatewayEventDecodeError::none ? result.error_offset : read.offset),
                read.error == GatewayEventDecodeError::none ? GatewayEventDecodeError::invalid_json : read.error,
                JsonScanError::none, result.error};
    *output = read.update;
    return {};
}
} // namespace maicord
