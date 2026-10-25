#include "maicord/model/channel_settings_model.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
enum Field : u32 {
    field_id = 1u << 0,
    field_guild = 1u << 1,
    field_type = 1u << 2,
    field_name = 1u << 3,
    field_parent = 1u << 4,
    field_bitrate = 1u << 5,
    field_user_limit = 1u << 6,
    field_slowmode = 1u << 7,
    field_nsfw = 1u << 8,
};
struct Parse {
    ChannelSettingsDraft value{};
    u32 seen{};
    bool failed{};
};
bool voice_like(u8 type) noexcept {
    return type == 2 || type == 13;
}
bool member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(opaque);
    struct Key {
        const char* name;
        u32 bit;
    };
    static constexpr Key keys[]{{"id", field_id},
                                {"guild_id", field_guild},
                                {"type", field_type},
                                {"name", field_name},
                                {"parent_id", field_parent},
                                {"bitrate", field_bitrate},
                                {"user_limit", field_user_limit},
                                {"rate_limit_per_user", field_slowmode},
                                {"nsfw", field_nsfw}};
    u32 bit = 0;
    for (const auto& key : keys) {
        u32 length = 0;
        while (key.name[length])
            ++length;
        if (json_string_equals_ascii(name, {key.name, length}))
            bit = key.bit;
    }
    if (!bit)
        return true;
    if (parse.seen & bit) {
        parse.failed = true;
        return false;
    }
    parse.seen |= bit;
    u64 number = 0;
    bool valid = true;
    auto& v = parse.value;
    switch (bit) {
    case field_id:
        valid = json_value_to_snowflake(value, &v.channel_id);
        break;
    case field_guild:
        valid = json_value_to_snowflake(value, &v.guild_id);
        break;
    case field_parent:
        valid = value.kind == JsonValueKind::null_value || json_value_to_snowflake(value, &v.parent_id);
        break;
    case field_type:
        valid = json_value_to_u64(value, &number) && number < 256;
        v.type = static_cast<u8>(number);
        break;
    case field_name: {
        JsonString encoded{};
        valid = json_value_as_string(value, &encoded);
        if (valid) {
            const auto decoded = json_string_decode_utf8(encoded, {v.name, kGuildAdminNameBytes});
            valid = decoded.ok();
            v.name_bytes = static_cast<u16>(decoded.bytes);
        }
        break;
    }
    case field_nsfw:
        valid = json_value_to_bool(value, &v.nsfw);
        break;
    default:
        valid = json_value_to_u64(value, &number) && number <= 1'000'000;
        if (bit == field_bitrate)
            v.bitrate = static_cast<u32>(number);
        else if (bit == field_user_limit)
            v.user_limit = static_cast<u32>(number);
        else
            v.slowmode_seconds = static_cast<u32>(number);
        break;
    }
    if (!valid)
        parse.failed = true;
    return valid;
}
} // namespace

bool channel_settings_supported(u8 type) noexcept {
    return type == 2 || type == 4 || type == 13;
}

bool channel_settings_decode(Utf8View body, ChannelSettingsDraft* output) noexcept {
    if (!output || !body.bytes || body.bytes > 64 * 1024 || !utf8_validate(body).valid)
        return false;
    u32 offsets[4096];
    const auto scanned = json_scan_structurals_avx1(body, {offsets, 4096});
    if (!scanned.ok())
        return false;
    Parse parse{};
    const auto parsed = json_visit_root_object(body, {offsets, scanned.count}, 32, member, &parse);
    constexpr u32 required = field_id | field_guild | field_type | field_name;
    if (!parsed.ok() || parse.failed || (parse.seen & required) != required ||
        !channel_settings_supported(parse.value.type))
        return false;
    copy_value(output, parse.value);
    return true;
}

bool channel_settings_equal(const ChannelSettingsDraft& a, const ChannelSettingsDraft& b) noexcept {
    return a.channel_id == b.channel_id && a.bitrate == b.bitrate && a.user_limit == b.user_limit &&
           a.slowmode_seconds == b.slowmode_seconds && a.nsfw == b.nsfw && a.name_bytes == b.name_bytes &&
           utf8_equal({a.name, a.name_bytes}, {b.name, b.name_bytes});
}

bool channel_settings_name_valid(Utf8View text) noexcept {
    const auto validation = utf8_validate(text);
    if (!validation.valid || !validation.codepoints || validation.codepoints > kGuildAdminNameCodepoints)
        return false;
    bool visible = false;
    for (u32 i = 0; i < text.bytes; ++i) {
        if (static_cast<u8>(text.data[i]) < 0x20)
            return false;
        visible = visible || text.data[i] != ' ';
    }
    return visible;
}

bool channel_settings_patch(const ChannelSettingsDraft& baseline, const ChannelSettingsDraft& draft,
                            ChannelPatch* patch) noexcept {
    if (!patch || baseline.channel_id != draft.channel_id || !draft.channel_id ||
        !channel_settings_supported(draft.type))
        return false;
    ChannelPatch value{};
    if (!utf8_equal({baseline.name, baseline.name_bytes}, {draft.name, draft.name_bytes})) {
        if (!channel_settings_name_valid({draft.name, draft.name_bytes}))
            return false;
        value.fields |= channel_patch_name;
        value.name = {draft.name, draft.name_bytes};
    }
    if (voice_like(draft.type)) {
        if (baseline.bitrate != draft.bitrate) {
            if (draft.bitrate < 8'000)
                return false;
            value.fields |= channel_patch_bitrate;
            value.bitrate = draft.bitrate;
        }
        if (baseline.user_limit != draft.user_limit) {
            if (draft.user_limit > (draft.type == 13 ? 10'000u : kChannelSettingsUserLimitMaximum))
                return false;
            value.fields |= channel_patch_user_limit;
            value.user_limit = draft.user_limit;
        }
        if (baseline.nsfw != draft.nsfw) {
            value.fields |= channel_patch_nsfw;
            value.nsfw = draft.nsfw;
        }
        if (baseline.slowmode_seconds != draft.slowmode_seconds) {
            if (draft.slowmode_seconds > 21'600)
                return false;
            value.fields |= channel_patch_slowmode;
            value.slowmode_seconds = draft.slowmode_seconds;
        }
    }
    if (!value.fields)
        return false;
    *patch = value;
    return true;
}

} // namespace maicord
