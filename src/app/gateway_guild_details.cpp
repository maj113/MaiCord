#include "gateway_guild_details.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/model/guild_detail_store.hpp"
#include "maicord/protocol/json_ids.hpp"

namespace maicord {
namespace {

struct Key {
    Utf8View name;
    u16 field;
};
constexpr Key kKeys[] = {
    {utf8_literal("premium_tier"), guild_detail_premium_tier},
    {utf8_literal("premium_subscription_count"), guild_detail_boost_count},
    {utf8_literal("system_channel_flags"), guild_detail_system_channel_flags},
    {utf8_literal("rules_channel_id"), guild_detail_rules_channel},
    {utf8_literal("public_updates_channel_id"), guild_detail_public_updates_channel},
    {utf8_literal("safety_alerts_channel_id"), guild_detail_safety_alerts_channel},
    {utf8_literal("premium_progress_bar_enabled"), guild_detail_progress_bar},
    {utf8_literal("description"), guild_detail_description},
    {utf8_literal("vanity_url_code"), guild_detail_vanity},
    {utf8_literal("incidents_data"), guild_detail_incidents},
    {utf8_literal("member_count"), guild_detail_member_count},
    {utf8_literal("splash"), guild_detail_splash},
};

bool is_null(JsonValue value) noexcept {
    return value.kind == JsonValueKind::null_value;
}

bool nullable_id(JsonValue value, u64* id) noexcept {
    if (is_null(value)) {
        *id = 0;
        return true;
    }
    return json_value_to_snowflake(value, id);
}

bool nullable_u32(JsonValue value, u32* output) noexcept {
    u64 number = 0;
    if (!is_null(value) && (!json_value_to_u64(value, &number) || number > 0xFFFFFFFFull))
        return false;
    *output = static_cast<u32>(number);
    return true;
}

enum class Put : u8 { invalid, ok, full };

// A JSON string (null as empty) decoded into the writer.
Put put_text(GatewayPayloadWriter* writer, JsonValue value, u32 maximum, u32* bytes) noexcept {
    *bytes = 0;
    if (is_null(value))
        return Put::ok;
    JsonString text{};
    if (!json_value_as_string(value, &text))
        return Put::invalid;
    const auto sized = json_string_decode_utf8(text, Slice<char>{nullptr, 0});
    if ((sized.error != JsonStringDecodeError::output_too_small && !sized.ok()) || sized.bytes > maximum)
        return Put::invalid;
    if (writer->used > writer->output.count || sized.bytes > writer->output.count - writer->used) {
        writer->overflow = true;
        return Put::full;
    }
    char* target = writer->output.data + writer->used;
    const auto decoded = json_string_decode_utf8(text, Slice<char>{target, writer->output.count - writer->used});
    if (!decoded.ok() || decoded.bytes != sized.bytes || !utf8_validate({target, decoded.bytes}).valid)
        return Put::invalid;
    writer->used += decoded.bytes;
    *bytes = decoded.bytes;
    return Put::ok;
}

struct FeatureNames {
    GatewayPayloadWriter* writer;
    u32 count;
    Put result;
};

// Each name is one length byte and its bytes; empty or longer names, or
// non-strings, make the whole list unusable.
bool visit_feature(void* opaque, u32, JsonValue value) noexcept {
    auto& names = *static_cast<FeatureNames*>(opaque);
    auto& writer = *names.writer;
    if (writer.used >= writer.output.count) {
        writer.overflow = true;
        names.result = Put::full;
        return false;
    }
    const u32 mark = writer.used++;
    u32 bytes = 0;
    const Put put = value.kind == JsonValueKind::string ? put_text(&writer, value, 255, &bytes) : Put::invalid;
    if (put != Put::ok || !bytes) {
        writer.used = mark;
        names.result = put == Put::full ? Put::full : Put::invalid;
        return false;
    }
    writer.output.data[mark] = static_cast<char>(bytes);
    ++names.count;
    return true;
}

bool incident_time(JsonValue value, u64* milliseconds) noexcept {
    JsonString stamp{};
    *milliseconds = 0;
    return is_null(value) || (json_value_as_string(value, &stamp) && parse_iso_timestamp(stamp, milliseconds));
}

// Malformed incident values read as no incident.
bool visit_incident(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& d = *static_cast<GatewayGuildDetails*>(opaque);
    u64 stamp = 0;
    if (json_name_is(name, utf8_literal("invites_disabled_until"))) {
        if (incident_time(value, &stamp))
            d.invites_disabled_until_ms = stamp;
    } else if (json_name_is(name, utf8_literal("dms_disabled_until"))) {
        if (incident_time(value, &stamp))
            d.dms_disabled_until_ms = stamp;
    } else if (json_name_is(name, utf8_literal("raid_detected_at"))) {
        if (incident_time(value, &stamp))
            d.raid_detected_at_ms = stamp;
    } else if (json_name_is(name, utf8_literal("dm_spam_detected_at"))) {
        if (incident_time(value, &stamp))
            d.dm_spam_detected_at_ms = stamp;
    } else if (json_name_is(name, utf8_literal("lockdown_duration_hours"))) {
        u64 hours = 0;
        if (!is_null(value) && json_value_to_u64(value, &hours))
            d.lockdown_duration_hours = static_cast<u8>(hours > 255 ? 255 : hours);
    }
    return true;
}

} // namespace

bool gateway_guild_detail_visit(GatewayGuildDetailValues* d, JsonString name, JsonValue value) noexcept {
    for (const Key& key : kKeys) {
        if (!json_name_is(name, key.name))
            continue;
        if (d->seen & key.field)
            return true;
        bool valid = true;
        u64 number = 0;
        switch (key.field) {
        case guild_detail_premium_tier:
            valid = json_value_to_u64(value, &number) && number <= 255;
            if (valid)
                d->premium_tier = static_cast<u8>(number);
            break;
        case guild_detail_boost_count: valid = nullable_u32(value, &d->premium_subscription_count); break;
        case guild_detail_member_count: valid = nullable_u32(value, &d->member_count); break;
        case guild_detail_system_channel_flags: valid = nullable_u32(value, &d->system_channel_flags); break;
        case guild_detail_rules_channel: valid = nullable_id(value, &d->rules_channel_id); break;
        case guild_detail_public_updates_channel: valid = nullable_id(value, &d->public_updates_channel_id); break;
        case guild_detail_safety_alerts_channel: valid = nullable_id(value, &d->safety_alerts_channel_id); break;
        case guild_detail_progress_bar:
            d->premium_progress_bar_enabled = false;
            valid = is_null(value) || json_value_to_bool(value, &d->premium_progress_bar_enabled);
            break;
        case guild_detail_description:
            valid = is_null(value) || value.kind == JsonValueKind::string;
            d->description = value;
            break;
        case guild_detail_vanity:
            valid = is_null(value) || value.kind == JsonValueKind::string;
            d->vanity_url_code = value;
            break;
        case guild_detail_incidents:
            valid = is_null(value) || value.kind == JsonValueKind::object;
            d->incidents_data = value;
            break;
        case guild_detail_splash:
            valid = is_null(value) || value.kind == JsonValueKind::string;
            d->splash = value;
            break;
        default: break;
        }
        if (valid)
            d->seen = static_cast<u16>(d->seen | key.field);
        return true;
    }
    return false;
}

void gateway_guild_detail_merge(GatewayGuildDetailValues* to, const GatewayGuildDetailValues& from) noexcept {
    const u16 take = from.seen & ~to->seen;
    if (take & guild_detail_premium_tier)
        to->premium_tier = from.premium_tier;
    if (take & guild_detail_boost_count)
        to->premium_subscription_count = from.premium_subscription_count;
    if (take & guild_detail_member_count)
        to->member_count = from.member_count;
    if (take & guild_detail_system_channel_flags)
        to->system_channel_flags = from.system_channel_flags;
    if (take & guild_detail_rules_channel)
        to->rules_channel_id = from.rules_channel_id;
    if (take & guild_detail_public_updates_channel)
        to->public_updates_channel_id = from.public_updates_channel_id;
    if (take & guild_detail_safety_alerts_channel)
        to->safety_alerts_channel_id = from.safety_alerts_channel_id;
    if (take & guild_detail_progress_bar)
        to->premium_progress_bar_enabled = from.premium_progress_bar_enabled;
    if (take & guild_detail_description)
        to->description = from.description;
    if (take & guild_detail_vanity)
        to->vanity_url_code = from.vanity_url_code;
    if (take & guild_detail_incidents)
        to->incidents_data = from.incidents_data;
    if (take & guild_detail_splash)
        to->splash = from.splash;
    to->seen = static_cast<u16>(to->seen | take);
}

bool gateway_guild_details_append(const GatewayGuildDetailValues& v, JsonValue features, bool features_seen,
                                  Slice<u32> scratch, u32 maximum_depth, GatewayPayloadWriter* writer,
                                  u16* bytes) noexcept {
    *bytes = 0;
    if (!writer || !scratch.data || !scratch.count)
        return true;
    const u32 mark = writer->used;
    const auto rollback = [&](bool result) noexcept {
        writer->used = mark;
        return result;
    };
    GatewayGuildDetails header{};
    constexpr u16 counts = guild_detail_boost_count | guild_detail_member_count;
    header.known = static_cast<u16>((guild_detail_all & ~(counts | guild_detail_description | guild_detail_vanity |
                                                          guild_detail_features)) |
                                    (v.seen & counts));
    header.rules_channel_id = v.rules_channel_id;
    header.public_updates_channel_id = v.public_updates_channel_id;
    header.safety_alerts_channel_id = v.safety_alerts_channel_id;
    header.system_channel_flags = v.system_channel_flags;
    header.premium_subscription_count = v.premium_subscription_count;
    header.member_count = v.member_count;
    header.premium_tier = v.premium_tier;
    header.premium_progress_bar_enabled = v.premium_progress_bar_enabled;
    // A splash value that is not a short hash reads as unknown.
    if (v.seen & guild_detail_splash) {
        JsonString text{};
        const auto decoded = json_value_as_string(v.splash, &text)
                                 ? json_string_decode_utf8(text, {header.splash, sizeof(header.splash)})
                                 : JsonStringDecodeResult{};
        if (v.splash.kind != JsonValueKind::null_value) {
            if (decoded.ok())
                header.splash_bytes = static_cast<u8>(decoded.bytes);
            else
                header.known = static_cast<u16>(header.known & ~guild_detail_splash);
        }
    }
    if (!gateway_payload_append(writer, &header, sizeof(header)))
        return rollback(false);

    u32 text = 0;
    Put put = (v.seen & guild_detail_description)
                  ? put_text(writer, v.description, kGuildDetailDescriptionMaximumBytes, &text)
                  : Put::ok;
    if (put == Put::full)
        return rollback(false);
    if (put == Put::ok) {
        header.known |= guild_detail_description;
        header.description_bytes = static_cast<u16>(text);
    }
    text = 0;
    put = (v.seen & guild_detail_vanity) ? put_text(writer, v.vanity_url_code, kGuildDetailVanityMaximumBytes, &text)
                                         : Put::ok;
    if (put == Put::full)
        return rollback(false);
    if (put == Put::ok) {
        header.known |= guild_detail_vanity;
        header.vanity_bytes = static_cast<u8>(text);
    }

    if ((v.seen & guild_detail_incidents) && v.incidents_data.kind == JsonValueKind::object) {
        const auto scan = json_scan_structurals_avx1(v.incidents_data.source, scratch);
        if (!scan.ok() || !json_visit_value_object(v.incidents_data, {scratch.data, scan.count}, maximum_depth,
                                                   visit_incident, &header)
                               .ok()) {
            header.invites_disabled_until_ms = header.dms_disabled_until_ms = 0;
            header.raid_detected_at_ms = header.dm_spam_detected_at_ms = 0;
            header.lockdown_duration_hours = 0;
        }
    }

    const u32 names_mark = writer->used;
    if (!features_seen)
        header.known |= guild_detail_features;
    else if (features.kind == JsonValueKind::array) {
        FeatureNames names{writer, 0, Put::ok};
        const auto scan = json_scan_structurals_avx1(features.source, scratch);
        const bool read = scan.ok() &&
                          json_visit_value_array(features, {scratch.data, scan.count}, maximum_depth, visit_feature,
                                                 &names)
                              .ok();
        if (names.result == Put::full)
            return rollback(false);
        if (read && names.result == Put::ok && names.count <= 0xFFFF && writer->used - names_mark <= 0xFFFF) {
            header.known |= guild_detail_features;
            header.feature_count = static_cast<u16>(names.count);
            header.feature_names_bytes = static_cast<u16>(writer->used - names_mark);
        } else
            writer->used = names_mark;
    }
    const u32 total = writer->used - mark;
    if (total > 0xFFFF)
        return rollback(true);
    copy_bytes(writer->output.data + mark, &header, sizeof(header));
    *bytes = static_cast<u16>(total);
    return true;
}

bool gateway_client_event_guild_details(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                        GatewayGuildDetails* details, Utf8View* description,
                                        Utf8View* vanity_url_code, ConstSlice<u8>* feature_names) noexcept {
    if (!details || !description || !vanity_url_code || !feature_names ||
        event.kind != GatewayClientEventKind::dispatch ||
        (event.dispatch_kind != GatewayClientDispatchKind::guild_create &&
         event.dispatch_kind != GatewayClientDispatchKind::guild_update) ||
        event.dispatch_payload_bytes != payload.count || event.state.guild_details_bytes < sizeof(GatewayGuildDetails) ||
        gateway_client_guild_strings_bytes(event.state) > payload.count || !payload.data)
        return false;
    const u32 offset = static_cast<u32>(event.state.name_bytes) + event.state.icon_bytes +
                       gateway_client_guild_banner_bytes(event.state);
    GatewayGuildDetails header{};
    copy_bytes(&header, payload.data + offset, sizeof(header));
    const u32 text = static_cast<u32>(header.description_bytes) + header.vanity_bytes + header.feature_names_bytes;
    if (sizeof(header) + text != event.state.guild_details_bytes || (header.known & ~guild_detail_all))
        return false;
    const char* base = reinterpret_cast<const char*>(payload.data) + offset + sizeof(header);
    const Utf8View text_description{base, header.description_bytes};
    const Utf8View text_vanity{base + header.description_bytes, header.vanity_bytes};
    const ConstSlice<u8> names{reinterpret_cast<const u8*>(base + header.description_bytes + header.vanity_bytes),
                               header.feature_names_bytes};
    if (!utf8_validate(text_description).valid || !utf8_validate(text_vanity).valid)
        return false;
    u32 cursor = 0, count = 0;
    while (cursor < names.count) {
        const u32 bytes = names[cursor];
        if (!bytes || bytes > names.count - cursor - 1 ||
            !utf8_validate({reinterpret_cast<const char*>(names.data + cursor + 1), bytes}).valid)
            return false;
        cursor += 1 + bytes;
        ++count;
    }
    if (count != header.feature_count)
        return false;
    *details = header;
    *description = text_description;
    *vanity_url_code = text_vanity;
    *feature_names = names;
    return true;
}

} // namespace maicord
