#include "maicord/protocol/voice_channel_info.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "json_helpers.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using namespace protocol_detail;
constexpr u32 kDepth = 16;
constexpr char kRequestPrefix[] = "{\"op\":43,\"d\":{\"guild_id\":\"";
constexpr char kRequestSuffix[] = "\",\"fields\":[\"status\",\"voice_start_time\"]}}";

struct Fields {
    u64 id, guild_id, start_s;
    JsonValue status, channels;
    bool has_id, has_guild, has_start, has_status, has_channels;
};
struct Decode {
    const JsonIndexedDocument* document;
    Slice<char> text;
    VoiceChannelInfoVisitor visitor;
    void* context;
    u64 guild_id;
    u32 count;
    VoiceChannelInfoError error;
};

bool visit_fields(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& f = *static_cast<Fields*>(opaque);
    bool valid = true;
    if (name_is(name, "id")) {
        valid = !f.has_id && json_value_to_snowflake(value, &f.id);
        f.has_id = true;
    } else if (name_is(name, "guild_id")) {
        valid = !f.has_guild && json_value_to_snowflake(value, &f.guild_id);
        f.has_guild = true;
    } else if (name_is(name, "voice_start_time")) {
        valid = !f.has_start &&
                (value.kind == JsonValueKind::null_value || json_value_to_u64_or_string(value, &f.start_s));
        f.has_start = true;
    } else if (name_is(name, "status")) {
        valid = !f.has_status &&
                (value.kind == JsonValueKind::null_value || value.kind == JsonValueKind::string);
        f.status = value;
        f.has_status = true;
    } else if (name_is(name, "channels")) {
        valid = !f.has_channels && value.kind == JsonValueKind::array;
        f.channels = value;
        f.has_channels = true;
    }
    return valid;
}

bool emit(Decode* d, const Fields& f, u64 guild_id) noexcept {
    if (!f.has_id || !f.id || !guild_id) {
        d->error = VoiceChannelInfoError::field;
        return false;
    }
    VoiceChannelInfoEntry entry{guild_id, f.id, f.start_s, {}, f.has_start, f.has_status};
    if (f.has_status && f.status.kind == JsonValueKind::string) {
        u32 bytes = 0;
        if (!decode_text(f.status, d->text.data, d->text.count, &bytes)) {
            JsonString encoded{};
            const bool string = json_value_as_string(f.status, &encoded);
            const auto size = json_string_decode_utf8(encoded, {});
            d->error = string && size.bytes > d->text.count ? VoiceChannelInfoError::capacity
                                                            : VoiceChannelInfoError::field;
            return false;
        }
        entry.status = {d->text.data, bytes};
    }
    ++d->count;
    return !d->visitor || d->visitor(d->context, entry);
}

bool visit_channel(void* opaque, u32, JsonValue value) noexcept {
    auto& d = *static_cast<Decode*>(opaque);
    if (value.kind != JsonValueKind::object) {
        d.error = VoiceChannelInfoError::field;
        return false;
    }
    Fields f{};
    if (!json_visit_indexed_object(*d.document, value, kDepth, visit_fields, &f).ok()) {
        if (d.error == VoiceChannelInfoError::none)
            d.error = VoiceChannelInfoError::field;
        return false;
    }
    return emit(&d, f, d.guild_id);
}
} // namespace

GatewayCommandWriteResult gateway_write_channel_info_request(u64 guild_id, Slice<u8> output) noexcept {
    if (!guild_id)
        return {0, GatewayCommandError::invalid_input};
    const u32 required =
        static_cast<u32>(sizeof(kRequestPrefix) + sizeof(kRequestSuffix) - 2) + decimal_digits(guild_id);
    if (!output.data || output.count < required)
        return {0, GatewayCommandError::output_too_small};
    u32 offset = sizeof(kRequestPrefix) - 1;
    copy_bytes(output.data, kRequestPrefix, offset);
    offset += decimal_write(output.data + offset, guild_id);
    copy_bytes(output.data + offset, kRequestSuffix, sizeof(kRequestSuffix) - 1);
    return {offset + static_cast<u32>(sizeof(kRequestSuffix) - 1), GatewayCommandError::none};
}

VoiceChannelInfoResult voice_channel_info_decode(VoiceChannelInfoEvent event, Utf8View data,
                                                 Slice<u32> scratch, Slice<char> text,
                                                 VoiceChannelInfoVisitor visitor, void* context) noexcept {
    if (!text.data && text.count)
        return {0, 0, VoiceChannelInfoError::input};
    const auto s = scan_json<VoiceChannelInfoError>(data, scratch);
    if (s.error != VoiceChannelInfoError::none)
        return {0, 0, s.error};
    Fields root{};
    if (!json_visit_root_object(data, s.document.structurals, kDepth, visit_fields, &root).ok())
        return {0, 0, VoiceChannelInfoError::field};
    Decode d{&s.document, text, visitor, context, root.guild_id, 0, VoiceChannelInfoError::none};
    if (!root.guild_id)
        return {0, 0, VoiceChannelInfoError::field};
    if (event == VoiceChannelInfoEvent::channel_info) {
        if (!root.has_channels)
            return {root.guild_id, 0, VoiceChannelInfoError::field};
        if (!json_visit_indexed_array(s.document, root.channels, kDepth, visit_channel, &d).ok())
            return {root.guild_id, d.count,
                    d.error == VoiceChannelInfoError::none ? VoiceChannelInfoError::json : d.error};
        return {root.guild_id, d.count, VoiceChannelInfoError::none};
    }
    // An update names the one field it changes.
    if ((event == VoiceChannelInfoEvent::start_time_update && !root.has_start) ||
        (event == VoiceChannelInfoEvent::status_update && !root.has_status))
        return {root.guild_id, 0, VoiceChannelInfoError::field};
    if (event == VoiceChannelInfoEvent::start_time_update)
        root.has_status = false;
    else
        root.has_start = false;
    if (!emit(&d, root, root.guild_id))
        return {root.guild_id, 0,
                d.error == VoiceChannelInfoError::none ? VoiceChannelInfoError::field : d.error};
    return {root.guild_id, 1, VoiceChannelInfoError::none};
}

u32 voice_channel_status_display(Utf8View status, Slice<char> output) noexcept {
    if (!status.data || !output.data)
        return 0;
    u32 used = 0;
    for (u32 i = 0; i < status.bytes && used < output.count;) {
        // <:name:id> or <a:name:id> -> :name:
        if (status.data[i] == '<') {
            u32 j = i + 1;
            if (j < status.bytes && status.data[j] == 'a')
                ++j;
            if (j < status.bytes && status.data[j] == ':') {
                const u32 name = j + 1;
                u32 k = name;
                while (k < status.bytes && status.data[k] != ':' && status.data[k] != '>' &&
                       status.data[k] != '<')
                    ++k;
                u32 digits = k + 1;
                while (k < status.bytes && digits < status.bytes && status.data[digits] >= '0' &&
                       status.data[digits] <= '9')
                    ++digits;
                if (k > name && k < status.bytes && status.data[k] == ':' && digits > k + 1 &&
                    digits < status.bytes && status.data[digits] == '>') {
                    const u32 bytes = k - name + 2;
                    if (used + bytes > output.count)
                        break;
                    output[used] = ':';
                    copy_bytes(output.data + used + 1, status.data + name, k - name);
                    output[used + bytes - 1] = ':';
                    used += bytes;
                    i = digits + 1;
                    continue;
                }
            }
        }
        output[used++] = status.data[i++];
    }
    return used;
}
} // namespace maicord
