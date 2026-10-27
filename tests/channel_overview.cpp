#include "maicord/protocol/channel_overview.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {
bool same(Utf8View a, Utf8View b) noexcept {
    if (a.bytes != b.bytes)
        return false;
    for (u32 i = 0; i < a.bytes; ++i)
        if (a.data[i] != b.data[i])
            return false;
    return true;
}
bool unchanged(const ChannelOverviewSnapshot& a, const ChannelOverviewSnapshot& b) noexcept {
    const auto* x = reinterpret_cast<const u8*>(&a);
    const auto* y = reinterpret_cast<const u8*>(&b);
    for (u32 i = 0; i < sizeof(a); ++i)
        if (x[i] != y[i])
            return false;
    return true;
}
} // namespace
void test_channel_overview(void (*check)(bool, const char*) noexcept) noexcept {
    ChannelOverviewSnapshot value{};
    auto decoded = channel_overview_decode(
        utf8_literal("{\"id\":\"20\",\"guild_id\":\"10\",\"type\":0,\"name\":\"general\"}"), &value);
    check(
        decoded.ok() && decoded.fields == channel_overview_name &&
            value.known_fields == channel_overview_all && value.channel_id == 20 && value.guild_id == 10 &&
            !value.topic_bytes && !value.nsfw && !value.slowmode_seconds,
        "channel overview codec: full authority defaults absent optional fields and owns all four values");
    const auto saved = value;
    const Utf8View invalid[] = {
        utf8_literal("{\"id\":\"20\",\"guild_id\":\"10\",\"type\":5,\"name\":\"x\"}"),
        utf8_literal("{\"id\":\"20\",\"guild_id\":\"10\",\"type\":0,\"name\":\"\"}"),
        utf8_literal("{\"id\":\"20\",\"guild_id\":\"10\",\"type\":0,\"name\":\"bad\\nname\"}"),
        utf8_literal("{\"id\":\"20\",\"guild_id\":\"10\",\"type\":0,\"name\":\"x\",\"na\\u006de\":\"y\"}"),
        utf8_literal("{\"id\":\"20\",\"guild_id\":\"10\",\"type\":0,\"name\":\"x\",\"nsfw\":7}"),
        utf8_literal("{\"id\":\"20\",\"guild_id\":\"10\",\"type\":0,\"name\":\"x\",\"rate_limit_per_user\":21601}"),
        utf8_literal("{\"id\":\"20\",\"guild_id\":\"10\",\"type\":0,\"name\":\"x\",\"unknown\":[true,]}"),
        utf8_literal("{\"id\":\"20\",\"guild_id\":\"10\",\"type\":0,\"name\":\"x\",\"topic\":\"\\uD800\"}")};
    bool atomic = true;
    for (const auto input : invalid)
        atomic = !channel_overview_decode(input, &value).ok() && unchanged(value, saved) && atomic;
    check(atomic, "channel overview codec: identity type duplicate malformed Unicode and field failures "
                  "preserve the entire output atomically");
    check(!channel_overview_decode(utf8_literal("{\"id\":\"20\",\"guild_id\":\"10\",\"type\":0}"), &value)
                  .ok() &&
              unchanged(value, saved),
          "channel overview codec: full response cannot invent a missing channel name");
    u8 output[kChannelOverviewMaximumBodyBytes]{};
    ChannelOverviewPatch patch{utf8_literal("ignored"), utf8_literal("quote \"\\\n\t"), 0,
                               channel_overview_topic, false};
    auto written = channel_overview_write_patch(patch, {output, sizeof(output)});
    check(written.ok() && same({reinterpret_cast<char*>(output), written.bytes},
                               utf8_literal("{\"topic\":\"quote \\\"\\\\\\n\\t\"}")),
          "channel overview codec: dirty-only PATCH correctly escapes quotes slash newline and tab");
    written = channel_overview_write_target(~u64{0}, {output, sizeof(output)});
    check(written.ok() && same({reinterpret_cast<char*>(output), written.bytes},
                               utf8_literal("/api/v10/channels/18446744073709551615")),
          "channel overview codec: exact channel route accepts the full snowflake range");
    output[0] = 0xA5;
    const auto sized = channel_overview_write_patch(patch, {});
    check(
        sized.error == ChannelOverviewError::capacity && sized.bytes > 0 &&
            channel_overview_write_patch(patch, {output, 1}).error == ChannelOverviewError::capacity &&
            output[0] == 0xA5,
        "channel overview codec: exact size query and insufficient capacity leave caller output untouched");
    char emoji_name[404]{};
    for (u32 i = 0; i < 101; ++i) {
        emoji_name[i * 4] = static_cast<char>(0xF0);
        emoji_name[i * 4 + 1] = static_cast<char>(0x9F);
        emoji_name[i * 4 + 2] = static_cast<char>(0x98);
        emoji_name[i * 4 + 3] = static_cast<char>(0x80);
    }
    patch = {{emoji_name, 400}, {}, 0, channel_overview_name, false};
    check(channel_overview_patch_valid(patch),
          "channel overview codec: 100 four-byte Unicode scalars fit the name limit");
    patch.name.bytes = 404;
    check(!channel_overview_patch_valid(patch) &&
              !channel_overview_write_patch(patch, {output, sizeof(output)}).ok(),
          "channel overview codec: Unicode field limit counts scalars and rejects the 101st atomically");
    patch = {{}, {}, 0, channel_overview_name, false};
    check(
        !channel_overview_patch_valid(patch) && !channel_overview_patch_valid({{}, {}, 0, 0, false}) &&
            !channel_overview_patch_valid({{}, {}, 0, 16, false}),
        "channel overview codec: wire name cannot be empty and empty or unknown patch masks are rejected");
    char many[12'000]{};
    ProtocolWriter builder{{reinterpret_cast<u8*>(many), sizeof(many)}, 0};
    bool built = protocol_writer_append_literal(
        &builder, "{\"id\":\"20\",\"guild_id\":\"10\",\"type\":0,\"name\":\"x\",\"unknown\":[0");
    for (u32 i = 0; i < 5'000; ++i)
        built = protocol_writer_append_literal(&builder, ",0") && built;
    built = protocol_writer_append_literal(&builder, "]}") && built;
    check(built &&
              channel_overview_decode({many, builder.used}, &value).error ==
                  ChannelOverviewError::capacity &&
              unchanged(value, saved),
          "channel overview codec: bounded structural overflow rejects an entire otherwise valid unknown "
          "subtree");
}
} // namespace maicord
