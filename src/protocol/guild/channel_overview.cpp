#include "maicord/protocol/guild/channel_overview.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/core/byte_range.hpp"
#include "../encoding/json_field_lists.hpp"
#include "../encoding/json_helpers.hpp"
#include "field_table.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
using protocol_detail::ErrorCodes;
using protocol_detail::FieldDef;
using protocol_detail::kFieldNotify;
using protocol_detail::kFieldNull;
using protocol_detail::ObjectReader;
using protocol_detail::read_member;
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
// The name (0), the topic (1) and the slowmode (2) must hold what they may; the type (6) is 0, a text channel's.
bool snapshot_hook(ObjectReader& r, u32 index, const JsonValue& value) noexcept {
    const auto& v = *reinterpret_cast<const ChannelOverviewSnapshot*>(r.base);
    if (index == 0)
        return text_valid({v.name, v.name_bytes}, true);
    if (index == 1)
        return text_valid({v.topic, v.topic_bytes}, false);
    if (index == 2)
        return v.slowmode_seconds <= kChannelOverviewMaximumSlowmodeSeconds;
    u64 type = 0;
    if (json_value_to_u64(value, &type) && type == 0)
        return true;
    r.error = static_cast<u8>(ChannelOverviewError::type);
    return false;
}
// The members sit at their bits: name 1, topic 2, slowmode 4, nsfw 8, then id 16, guild 32 and type 64.
constexpr FieldDef kSnapshotDefs[]{
    {"name", MAICORD_TEXT_ROW(field_text, kFieldNotify, ChannelOverviewSnapshot, name, name_bytes)},
    {"topic", MAICORD_TEXT_ROW(field_text, kFieldNull | kFieldNotify, ChannelOverviewSnapshot, topic, topic_bytes)},
    {"rate_limit_per_user", MAICORD_ROW(field_number, kFieldNotify, ChannelOverviewSnapshot, slowmode_seconds)},
    {"nsfw", MAICORD_FIELD_BOOL(ChannelOverviewSnapshot, nsfw)},
    {"id", MAICORD_FIELD_SNOWFLAKE(ChannelOverviewSnapshot, channel_id)},
    {"guild_id", MAICORD_FIELD_SNOWFLAKE(ChannelOverviewSnapshot, guild_id)},
    {"type", MAICORD_FIELD_HOOK},
};
constexpr ErrorCodes kSnapshotCodes{static_cast<u8>(ChannelOverviewError::duplicate),
                                    static_cast<u8>(ChannelOverviewError::field),
                                    static_cast<u8>(ChannelOverviewError::json),
                                    static_cast<u8>(ChannelOverviewError::identity),
                                    static_cast<u8>(ChannelOverviewError::capacity)};
MAICORD_FIELD_TABLE(kSnapshotSpec, kSnapshotDefs, snapshot_hook, kSnapshotCodes, 32);
void write(const ChannelOverviewPatch& patch, protocol_detail::JsonBody& b) noexcept {
    b.begin_object();
    b.member_if(patch.fields & channel_overview_name, "name", json_text(patch.name));
    b.member_if(patch.fields & channel_overview_topic, "topic", json_text(patch.topic));
    b.member_if(patch.fields & channel_overview_slowmode, "rate_limit_per_user", patch.slowmode_seconds);
    b.member_if(patch.fields & channel_overview_nsfw, "nsfw", json_bool(patch.nsfw));
    b.end_object();
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
ChannelOverviewResult channel_overview_decode(Utf8View input, ChannelOverviewSnapshot* output) noexcept {
    if (!output || !byte_range_valid({reinterpret_cast<const u8*>(input.data), input.bytes}) || !input.bytes ||
        input.bytes > 64 * 1024 || !utf8_validate(input).valid)
        return {0, ChannelOverviewError::input};
    // Application bound, not a wire-schema limit. Refuse structural overflow
    // atomically; never publish a truncated channel or unknown JSON subtree.
    u32 offsets[4096];
    const auto scanned = json_scan_structurals_avx1(input, {offsets, 4096});
    if (!scanned.ok())
        return {0, scanned.error == JsonScanError::output_overflow ? ChannelOverviewError::capacity
                                                                   : ChannelOverviewError::json};
    ChannelOverviewSnapshot value{};
    ObjectReader reader{{nullptr, 0}, &kSnapshotSpec, reinterpret_cast<u8*>(&value), 0};
    const auto parsed = json_visit_root_object(input, {offsets, scanned.count}, 32, read_member, &reader);
    if (!parsed.ok())
        return {0, reader.error ? static_cast<ChannelOverviewError>(reader.error) : ChannelOverviewError::json};
    if ((reader.seen & (16 | 32 | 64 | 1)) != (16 | 32 | 64 | 1))
        return {0, ChannelOverviewError::identity};
    value.known_fields = channel_overview_all;
    *output = value;
    return {input.bytes, ChannelOverviewError::none, static_cast<u8>(reader.seen & channel_overview_all)};
}
ChannelOverviewResult channel_overview_write_target(u64 channel_id, Slice<u8> output) noexcept {
    if (!channel_id || !byte_range_valid({output.data, output.count}))
        return {0, ChannelOverviewError::input};
    u8 target[64];
    ProtocolWriter writer{{target, sizeof(target)}, 0};
    if (!protocol_write(&writer, "/api/v9/channels/", channel_id))
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
    protocol_detail::JsonBody body{{temporary, sizeof(temporary)}};
    write(patch, body);
    if (!body.ok)
        return {0, ChannelOverviewError::capacity};
    if (output.count < body.writer.used)
        return {body.writer.used, ChannelOverviewError::capacity};
    copy_bytes(output.data, temporary, body.writer.used);
    return {body.writer.used, ChannelOverviewError::none, patch.fields};
}
} // namespace maicord
