#include "maicord/protocol/channel_overview.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/byte_range.hpp"
#include "json_field_lists.hpp"

namespace maicord {
namespace {
bool text_valid(Utf8View text, bool name) noexcept {
    if (!byte_range_valid({reinterpret_cast<const u8*>(text.data), text.bytes}) ||
        text.bytes > (name ? kChannelOverviewNameBytes : kChannelOverviewTopicBytes))
        return false;
    const auto valid = utf8_validate(text);
    if (!valid.valid ||
        valid.codepoints > (name ? kChannelOverviewNameCodepoints : kChannelOverviewTopicCodepoints) ||
        (name && !valid.codepoints))
        return false;
    for (u32 i = 0; i < text.bytes; ++i)
        if (!text.data[i] || (name && (text.data[i] == '\r' || text.data[i] == '\n')))
            return false;
    return true;
}
struct Parse {
    ChannelOverviewSnapshot value{};
    u32 seen{};
    u8 fields{};
    ChannelOverviewError error{};
};
bool member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(opaque);
    // Bits: name 1, topic 2, slowmode 4, nsfw 8, then id 16, guild 32 and type 64.
    const u32 bit = protocol_detail::member_bit<"name", "topic", "rate_limit_per_user", "nsfw", "id", "guild_id", "type">(name);
    if (!bit)
        return true;
    if (parse.seen & bit) {
        parse.error = ChannelOverviewError::duplicate;
        return false;
    }
    parse.seen |= bit;
    bool valid = true;
    u64 number = 0;
    if (bit == 16)
        valid = json_value_to_snowflake(value, &parse.value.channel_id);
    else if (bit == 32)
        valid = json_value_to_snowflake(value, &parse.value.guild_id);
    else if (bit == 64) {
        valid = json_value_to_u64(value, &number) && number == 0;
        parse.value.type = static_cast<u8>(number);
        if (!valid)
            parse.error = ChannelOverviewError::type;
    } else if (bit == channel_overview_name || bit == channel_overview_topic) {
        char* destination = bit == channel_overview_name ? parse.value.name : parse.value.topic;
        const u32 capacity =
            bit == channel_overview_name ? kChannelOverviewNameBytes : kChannelOverviewTopicBytes;
        u32& bytes = bit == channel_overview_name ? parse.value.name_bytes : parse.value.topic_bytes;
        if (bit == channel_overview_topic && value.kind == JsonValueKind::null_value)
            bytes = 0;
        else {
            JsonString encoded{};
            valid = json_value_as_string(value, &encoded);
            if (valid) {
                const auto decoded = json_string_decode_utf8(encoded, {destination, capacity});
                valid = decoded.ok();
                bytes = decoded.bytes;
            }
        }
        valid = valid && text_valid({destination, bytes}, bit == channel_overview_name);
    } else if (bit == channel_overview_slowmode) {
        valid = json_value_to_u64(value, &number) && number <= kChannelOverviewMaximumSlowmodeSeconds;
        parse.value.slowmode_seconds = static_cast<u32>(number);
    } else
        valid = json_value_to_bool(value, &parse.value.nsfw);
    if (!valid) {
        if (parse.error == ChannelOverviewError::none)
            parse.error = ChannelOverviewError::field;
        return false;
    }
    if (bit <= channel_overview_nsfw)
        parse.fields |= static_cast<u8>(bit);
    return true;
}
bool write(const ChannelOverviewPatch& patch, ProtocolWriter* writer) noexcept {
    if (!protocol_writer_append_byte(writer, '{'))
        return false;
    bool first = true;
    const auto key = [&](u8 bit, const char* name) noexcept {
        if (!(patch.fields & bit))
            return true;
        if (!first && !protocol_writer_append_byte(writer, ','))
            return false;
        first = false;
        return protocol_writer_append_c_string(writer, name);
    };
    return (!(patch.fields & channel_overview_name) ||
            (key(channel_overview_name, "\"name\":") &&
             protocol_writer_append_json_string(writer, patch.name))) &&
           (!(patch.fields & channel_overview_topic) ||
            (key(channel_overview_topic, "\"topic\":") &&
             protocol_writer_append_json_string(writer, patch.topic))) &&
           (!(patch.fields & channel_overview_slowmode) ||
            (key(channel_overview_slowmode, "\"rate_limit_per_user\":") &&
             protocol_writer_append_u64(writer, patch.slowmode_seconds))) &&
           (!(patch.fields & channel_overview_nsfw) ||
            (key(channel_overview_nsfw, "\"nsfw\":") &&
             protocol_writer_append_c_string(writer, patch.nsfw ? "true" : "false"))) &&
           protocol_writer_append_byte(writer, '}');
}
} // namespace
Utf8View channel_overview_name_view(const ChannelOverviewSnapshot& value) noexcept {
    return value.name_bytes <= sizeof(value.name) ? Utf8View{value.name, value.name_bytes} : Utf8View{};
}
Utf8View channel_overview_topic_view(const ChannelOverviewSnapshot& value) noexcept {
    return value.topic_bytes <= sizeof(value.topic) ? Utf8View{value.topic, value.topic_bytes} : Utf8View{};
}
bool channel_overview_patch_valid(const ChannelOverviewPatch& patch) noexcept {
    return patch.fields && !(patch.fields & ~channel_overview_all) &&
           (!(patch.fields & channel_overview_name) || text_valid(patch.name, true)) &&
           (!(patch.fields & channel_overview_topic) || text_valid(patch.topic, false)) &&
           (!(patch.fields & channel_overview_slowmode) ||
            patch.slowmode_seconds <= kChannelOverviewMaximumSlowmodeSeconds);
}
ChannelOverviewResult channel_overview_decode(Utf8View input, bool partial,
                                              const ChannelOverviewSnapshot* previous,
                                              ChannelOverviewSnapshot* output) noexcept {
    if (!output || !byte_range_valid({reinterpret_cast<const u8*>(input.data), input.bytes}) ||
        !input.bytes || input.bytes > 64 * 1024 || !utf8_validate(input).valid ||
        (partial && (!previous || !previous->channel_id || !previous->guild_id || previous->type != 0 ||
                     previous->known_fields & ~channel_overview_all ||
                     previous->name_bytes > kChannelOverviewNameBytes ||
                     previous->topic_bytes > kChannelOverviewTopicBytes ||
                     (previous->known_fields &&
                      !channel_overview_patch_valid(
                          {channel_overview_name_view(*previous), channel_overview_topic_view(*previous),
                           previous->slowmode_seconds, previous->known_fields, previous->nsfw})))))
        return {0, ChannelOverviewError::input};
    // Application bound, not a wire-schema limit. Refuse structural overflow
    // atomically; never publish a truncated channel or unknown JSON subtree.
    u32 offsets[4096]{};
    const auto scanned = json_scan_structurals_avx1(input, {offsets, 4096});
    if (!scanned.ok())
        return {0, scanned.error == JsonScanError::output_overflow ? ChannelOverviewError::capacity
                                                                   : ChannelOverviewError::json};
    Parse parse{};
    if (partial)
        parse.value = *previous;
    const auto parsed = json_visit_root_object(input, {offsets, scanned.count}, 32, member, &parse);
    if (!parsed.ok())
        return {0, parse.error == ChannelOverviewError::none ? ChannelOverviewError::json : parse.error};
    if (!(parse.seen & 16) || (!partial && (parse.seen & (16 | 32 | 64 | 1)) != (16 | 32 | 64 | 1)) ||
        (partial &&
         (parse.value.channel_id != previous->channel_id || parse.value.guild_id != previous->guild_id)))
        return {0, ChannelOverviewError::identity};
    parse.value.known_fields =
        partial ? static_cast<u8>(previous->known_fields | parse.fields) : channel_overview_all;
    *output = parse.value;
    return {input.bytes, ChannelOverviewError::none, parse.fields};
}
ChannelOverviewResult channel_overview_write_target(u64 channel_id, Slice<u8> output) noexcept {
    if (!channel_id || !byte_range_valid({output.data, output.count}))
        return {0, ChannelOverviewError::input};
    u8 target[64];
    ProtocolWriter writer{{target, sizeof(target)}, 0};
    if (!protocol_writer_append_literal(&writer, "/api/v10/channels/") ||
        !protocol_writer_append_u64(&writer, channel_id))
        return {0, ChannelOverviewError::capacity};
    if (output.count < writer.used)
        return {writer.used, ChannelOverviewError::capacity};
    copy_bytes(output.data, target, writer.used);
    return {writer.used};
}
ChannelOverviewResult channel_overview_write_patch(const ChannelOverviewPatch& patch,
                                                   Slice<u8> output) noexcept {
    if (!channel_overview_patch_valid(patch) || !byte_range_valid({output.data, output.count}))
        return {0, ChannelOverviewError::input};
    u8 temporary[kChannelOverviewMaximumBodyBytes];
    ProtocolWriter writer{{temporary, sizeof(temporary)}, 0};
    if (!write(patch, &writer))
        return {0, ChannelOverviewError::capacity};
    if (output.count < writer.used)
        return {writer.used, ChannelOverviewError::capacity};
    copy_bytes(output.data, temporary, writer.used);
    return {writer.used, ChannelOverviewError::none, patch.fields};
}
} // namespace maicord
