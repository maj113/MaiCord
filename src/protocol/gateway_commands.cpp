#include "maicord/protocol/gateway_commands.hpp"
#include "maicord/protocol/gateway_voice.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"

#include "json_write.hpp"

namespace maicord {
namespace {

using protocol_detail::encoded_string;
using protocol_detail::EncodedString;
using protocol_detail::write_encoded;
using protocol_detail::write_literal;

GatewayCommandWriteResult invalid_string_result(Utf8View value, bool require_nonempty) noexcept {
    return (!value.data && value.bytes != 0) || (require_nonempty && value.bytes == 0)
               ? GatewayCommandWriteResult{0, GatewayCommandError::invalid_input}
               : GatewayCommandWriteResult{0, GatewayCommandError::invalid_utf8};
}

GatewayCommandWriteResult encode_bounded(Utf8View value, u32 maximum, EncodedString* output) noexcept {
    if (value.bytes > maximum) {
        return GatewayCommandWriteResult{0, GatewayCommandError::value_too_large};
    }
    if (!encoded_string(value, true, output)) {
        return invalid_string_result(value, true);
    }
    return GatewayCommandWriteResult{0, GatewayCommandError::none};
}

Utf8View user_status(GatewayUserStatus status) noexcept {
    switch (status) {
    case GatewayUserStatus::online:
        return utf8_literal("online");
    case GatewayUserStatus::idle:
        return utf8_literal("idle");
    case GatewayUserStatus::dnd:
        return utf8_literal("dnd");
    case GatewayUserStatus::invisible:
        return utf8_literal("invisible");
    default:
        return {};
    }
}

} // namespace

GatewayCommandWriteResult gateway_write_voice_state(const GatewayVoiceStateUpdate& state,
                                                    Slice<u8> output) noexcept {
    if (!output.data)
        return {0, GatewayCommandError::invalid_input};
    const u32 guild_bytes = state.guild_id ? decimal_digits(state.guild_id) + 2 : 4;
    const u32 channel_bytes = state.channel_id ? decimal_digits(state.channel_id) + 2 : 4;
    const u32 required = sizeof("{\"op\":4,\"d\":{\"guild_id\":,\"channel_id\":,\"self_mute\":,\"self_"
                                "deaf\":,\"self_video\":}}") -
                         1 + guild_bytes + channel_bytes + (state.self_mute ? 4 : 5) +
                         (state.self_deaf ? 4 : 5) + (state.self_video ? 4 : 5);
    if (required > output.count)
        return {0, GatewayCommandError::output_too_small};
    u32 at = 0;
    auto id = [&](u64 value) noexcept {
        if (!value) {
            write_literal(output.data, &at, "null");
            return;
        }
        output.data[at++] = '"';
        at += decimal_write(output.data + at, value);
        output.data[at++] = '"';
    };
    auto boolean = [&](bool value) noexcept {
        if (value)
            write_literal(output.data, &at, "true");
        else
            write_literal(output.data, &at, "false");
    };
    write_literal(output.data, &at, "{\"op\":4,\"d\":{\"guild_id\":");
    id(state.guild_id);
    write_literal(output.data, &at, ",\"channel_id\":");
    id(state.channel_id);
    write_literal(output.data, &at, ",\"self_mute\":");
    boolean(state.self_mute);
    write_literal(output.data, &at, ",\"self_deaf\":");
    boolean(state.self_deaf);
    write_literal(output.data, &at, ",\"self_video\":");
    boolean(state.self_video);
    write_literal(output.data, &at, "}}");
    return {at, GatewayCommandError::none};
}

bool gateway_user_launch_id_valid(Utf8View value) noexcept {
    if (!value.data || value.bytes != kGatewayUserLaunchIdBytes) {
        return false;
    }
    for (u32 index = 0; index < value.bytes; ++index) {
        const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
        if (separator ? value.data[index] != '-' : !ascii_is_hex_digit(value.data[index])) {
            return false;
        }
    }
    return true;
}

GatewayCommandWriteResult gateway_write_user_identify(const GatewayUserIdentify& identify,
                                                      Slice<u8> output) noexcept {
    if (!output.data || identify.properties.client_build_number == 0 ||
        !gateway_user_launch_id_valid(identify.properties.client_launch_id)) {
        return GatewayCommandWriteResult{0, GatewayCommandError::invalid_input};
    }
    const Utf8View status_view = user_status(identify.status);
    if (!status_view.data) {
        return GatewayCommandWriteResult{0, GatewayCommandError::invalid_input};
    }

    EncodedString token{};
    EncodedString operating_system{};
    EncodedString browser{};
    EncodedString device{};
    EncodedString locale{};
    EncodedString user_agent{};
    EncodedString browser_version{};
    EncodedString os_version{};
    EncodedString release_channel{};
    EncodedString launch_id{};
    EncodedString status{};
    const GatewayCommandWriteResult fields[] = {
        encode_bounded(identify.token, kGatewayUserMaximumTokenBytes, &token),
        encode_bounded(identify.properties.operating_system, kGatewayUserMaximumOperatingSystemBytes,
                       &operating_system),
        encode_bounded(identify.properties.browser, kGatewayUserMaximumBrowserBytes, &browser),
        encode_bounded(identify.properties.device, kGatewayUserMaximumDeviceBytes, &device),
        encode_bounded(identify.properties.system_locale, kGatewayUserMaximumLocaleBytes, &locale),
        encode_bounded(identify.properties.browser_user_agent, kGatewayUserMaximumUserAgentBytes,
                       &user_agent),
        encode_bounded(identify.properties.browser_version, kGatewayUserMaximumVersionBytes,
                       &browser_version),
        encode_bounded(identify.properties.os_version, kGatewayUserMaximumVersionBytes, &os_version),
        encode_bounded(identify.properties.release_channel, kGatewayUserMaximumReleaseChannelBytes,
                       &release_channel),
        encode_bounded(identify.properties.client_launch_id, kGatewayUserLaunchIdBytes, &launch_id),
        encode_bounded(status_view, 9, &status),
    };
    for (u32 index = 0; index < static_cast<u32>(sizeof(fields) / sizeof(fields[0])); ++index) {
        if (!fields[index].ok()) {
            return fields[index];
        }
    }

    constexpr u32 prefix_bytes = sizeof("{\"op\":2,\"d\":{\"token\":") - 1;
    constexpr u32 capabilities_bytes = sizeof(",\"capabilities\":,\"properties\":{\"os\":") - 1;
    constexpr u32 browser_bytes = sizeof(",\"browser\":") - 1;
    constexpr u32 device_bytes = sizeof(",\"device\":") - 1;
    constexpr u32 locale_bytes = sizeof(",\"system_locale\":") - 1;
    constexpr u32 user_agent_bytes = sizeof(",\"browser_user_agent\":") - 1;
    constexpr u32 browser_version_bytes = sizeof(",\"browser_version\":") - 1;
    constexpr u32 os_version_bytes = sizeof(",\"os_version\":") - 1;
    constexpr u32 release_bytes = sizeof(",\"release_channel\":") - 1;
    constexpr u32 build_bytes = sizeof(",\"client_build_number\":") - 1;
    constexpr u32 fixed_properties_bytes =
        sizeof(",\"client_event_source\":null,\"has_client_mods\":false,\"client_launch_id\":") - 1;
    constexpr u32 presence_bytes = sizeof("},\"presence\":{\"since\":") - 1;
    constexpr u32 activities_bytes = sizeof(",\"activities\":[],\"status\":") - 1;
    constexpr u32 afk_bytes = sizeof(",\"afk\":") - 1;
    constexpr u32 suffix_bytes =
        sizeof("},\"compress\":false,\"client_state\":{\"guild_versions\":{}}}}") - 1;
    const u32 capability_digits = decimal_digits(kGatewayUserCapabilitiesDefault);
    const u32 build_digits = decimal_digits(identify.properties.client_build_number);
    const u32 since_digits = identify.has_since ? decimal_digits(identify.since_ms) : 4;
    const u32 afk_literal_bytes = identify.afk ? 4 : 5;
    const u64 required = static_cast<u64>(prefix_bytes) + token.bytes + capabilities_bytes +
                         capability_digits + operating_system.bytes + browser_bytes + browser.bytes +
                         device_bytes + device.bytes + locale_bytes + locale.bytes + user_agent_bytes +
                         user_agent.bytes + browser_version_bytes + browser_version.bytes +
                         os_version_bytes + os_version.bytes + release_bytes + release_channel.bytes +
                         build_bytes + build_digits + fixed_properties_bytes + launch_id.bytes +
                         presence_bytes + since_digits + activities_bytes + status.bytes + afk_bytes +
                         afk_literal_bytes + suffix_bytes;
    if (required > kGatewayUserMaximumCommandBytes) {
        return GatewayCommandWriteResult{0, GatewayCommandError::value_too_large};
    }
    if (required > output.count) {
        return GatewayCommandWriteResult{0, GatewayCommandError::output_too_small};
    }

    u32 offset = 0;
    write_literal(output.data, &offset, "{\"op\":2,\"d\":{\"token\":");
    write_encoded(output.data, &offset, token);
    write_literal(output.data, &offset, ",\"capabilities\":");
    decimal_write(output.data + offset, kGatewayUserCapabilitiesDefault);
    offset += capability_digits;
    write_literal(output.data, &offset, ",\"properties\":{\"os\":");
    write_encoded(output.data, &offset, operating_system);
    write_literal(output.data, &offset, ",\"browser\":");
    write_encoded(output.data, &offset, browser);
    write_literal(output.data, &offset, ",\"device\":");
    write_encoded(output.data, &offset, device);
    write_literal(output.data, &offset, ",\"system_locale\":");
    write_encoded(output.data, &offset, locale);
    write_literal(output.data, &offset, ",\"browser_user_agent\":");
    write_encoded(output.data, &offset, user_agent);
    write_literal(output.data, &offset, ",\"browser_version\":");
    write_encoded(output.data, &offset, browser_version);
    write_literal(output.data, &offset, ",\"os_version\":");
    write_encoded(output.data, &offset, os_version);
    write_literal(output.data, &offset, ",\"release_channel\":");
    write_encoded(output.data, &offset, release_channel);
    write_literal(output.data, &offset, ",\"client_build_number\":");
    decimal_write(output.data + offset, identify.properties.client_build_number);
    offset += build_digits;
    write_literal(output.data, &offset,
                  ",\"client_event_source\":null,\"has_client_mods\":false,\"client_launch_id\":");
    write_encoded(output.data, &offset, launch_id);
    write_literal(output.data, &offset, "},\"presence\":{\"since\":");
    if (identify.has_since) {
        decimal_write(output.data + offset, identify.since_ms);
        offset += since_digits;
    } else {
        write_literal(output.data, &offset, "null");
    }
    write_literal(output.data, &offset, ",\"activities\":[],\"status\":");
    write_encoded(output.data, &offset, status);
    write_literal(output.data, &offset, ",\"afk\":");
    if (identify.afk) {
        write_literal(output.data, &offset, "true");
    } else {
        write_literal(output.data, &offset, "false");
    }
    write_literal(output.data, &offset, "},\"compress\":false,\"client_state\":{\"guild_versions\":{}}}}");
    return GatewayCommandWriteResult{offset, GatewayCommandError::none};
}

GatewayCommandWriteResult gateway_write_resume(const GatewayResume& resume, Slice<u8> output) noexcept {
    if (!output.data) {
        return GatewayCommandWriteResult{0, GatewayCommandError::invalid_input};
    }
    EncodedString token{};
    EncodedString session{};
    if (!encoded_string(resume.token, true, &token)) {
        return invalid_string_result(resume.token, true);
    }
    if (!encoded_string(resume.session_id, true, &session)) {
        return invalid_string_result(resume.session_id, true);
    }

    constexpr u32 fixed_bytes = sizeof("{\"op\":6,\"d\":{\"token\":,\"session_id\":,\"seq\":}}") - 1;
    const u32 sequence_digits = decimal_digits(resume.sequence);
    const u64 required = static_cast<u64>(fixed_bytes) + token.bytes + session.bytes + sequence_digits;
    if (required > output.count) {
        return GatewayCommandWriteResult{0, GatewayCommandError::output_too_small};
    }

    u32 offset = 0;
    write_literal(output.data, &offset, "{\"op\":6,\"d\":{\"token\":");
    write_encoded(output.data, &offset, token);
    write_literal(output.data, &offset, ",\"session_id\":");
    write_encoded(output.data, &offset, session);
    write_literal(output.data, &offset, ",\"seq\":");
    decimal_write(output.data + offset, resume.sequence);
    offset += sequence_digits;
    write_literal(output.data, &offset, "}}");
    return GatewayCommandWriteResult{offset, GatewayCommandError::none};
}

GatewayCommandWriteResult gateway_write_guild_subscriptions_bulk(
    const GatewayGuildSubscription& subscription, Slice<u8> output) noexcept {
    if (!output.data || !subscription.guild_id ||
        (subscription.omit_channel_ranges ? subscription.channel_id != 0 : !subscription.channel_id) ||
        (!subscription.thread_member_lists_present &&
         (subscription.thread_id || subscription.clear_thread_guild_id)) ||
        subscription.clear_thread_guild_id == subscription.guild_id)
        return {0, GatewayCommandError::invalid_input};
    const u32 range_count =
        subscription.omit_channel_ranges ? 0 : (subscription.range_count ? subscription.range_count : 1);
    if (subscription.range_count > kGatewayMemberSubscriptionMaximumRanges ||
        (subscription.omit_channel_ranges && subscription.range_count))
        return {0, GatewayCommandError::invalid_input};
    u32 range_digits = 0;
    for (u32 i = 0; i < range_count; ++i) {
        const auto range =
            subscription.range_count
                ? subscription.ranges[i]
                : GatewayMemberSubscriptionRange{subscription.range_first, subscription.range_last};
        if (range.last < range.first || range.last - range.first > 999u ||
            (subscription.range_count && range.last - range.first >= 100u) ||
            (i && range.first <= subscription.ranges[i - 1].last))
            return {0, GatewayCommandError::invalid_input};
        range_digits += decimal_digits(range.first) + decimal_digits(range.last);
    }
    // The guild's other remembered channels: distinct, never empty, each with
    // ascending bounded ranges.
    if (subscription.channel_count > kGatewayGuildSubscriptionMaximumChannels - 1 ||
        (subscription.channel_count && subscription.omit_channel_ranges))
        return {0, GatewayCommandError::invalid_input};
    u32 other_bytes = 0;
    for (u32 c = 0; c < subscription.channel_count; ++c) {
        const auto& other = subscription.channels[c];
        if (!other.channel_id || other.channel_id == subscription.channel_id || !other.range_count ||
            other.range_count > kGatewayMemberSubscriptionMaximumRanges)
            return {0, GatewayCommandError::invalid_input};
        for (u32 earlier = 0; earlier < c; ++earlier)
            if (subscription.channels[earlier].channel_id == other.channel_id)
                return {0, GatewayCommandError::invalid_input};
        other_bytes += sizeof(",\"\":[]") - 1 + decimal_digits(other.channel_id) + other.range_count - 1;
        for (u32 i = 0; i < other.range_count; ++i) {
            const auto range = other.ranges[i];
            if (range.last < range.first || range.last - range.first >= 100u ||
                (i && range.first <= other.ranges[i - 1].last))
                return {0, GatewayCommandError::invalid_input};
            other_bytes += sizeof("[,]") - 1 + decimal_digits(range.first) + decimal_digits(range.last);
        }
    }
    u32 field_bytes = 0, field_count = 0;
    if (!subscription.omit_features) {
        field_bytes += sizeof("\"typing\":,\"activities\":,\"threads\":") - 1 +
                       (subscription.typing ? 4u : 5u) + (subscription.activities ? 4u : 5u) +
                       (subscription.threads ? 4u : 5u);
        ++field_count;
    }
    if (!subscription.omit_channel_ranges) {
        field_bytes += sizeof("\"channels\":{\":[[]]}") - 1 + decimal_digits(subscription.channel_id) +
                       range_digits + range_count + 1 + (range_count - 1) * 3 + other_bytes;
        ++field_count;
    }
    if (subscription.thread_member_lists_present) {
        field_bytes += sizeof("\"thread_member_lists\":[]") - 1 +
                       (subscription.thread_id ? decimal_digits(subscription.thread_id) + 2 : 0);
        ++field_count;
    }
    if (!field_count)
        return {0, GatewayCommandError::invalid_input};
    field_bytes += field_count - 1;
    const u32 clear_bytes = subscription.clear_thread_guild_id
                                ? decimal_digits(subscription.clear_thread_guild_id) + 5 +
                                      sizeof("\"thread_member_lists\":[]") - 1 + 1
                                : 0;
    const u64 required = sizeof("{\"op\":37,\"d\":{\"subscriptions\":{") - 1 +
                         static_cast<u64>(decimal_digits(subscription.guild_id)) + 5 + field_bytes +
                         clear_bytes + 3;
    if (required > output.count)
        return {0, GatewayCommandError::output_too_small};
    u32 offset = 0;
    const auto number = [&](u64 value) noexcept {
        offset += decimal_write(output.data + offset, value);
    };
    const auto guild = [&](u64 id) noexcept {
        write_literal(output.data, &offset, "\"");
        number(id);
        write_literal(output.data, &offset, "\":{");
    };
    const auto boolean = [&](bool value) noexcept {
        if (value)
            write_literal(output.data, &offset, "true");
        else
            write_literal(output.data, &offset, "false");
    };
    write_literal(output.data, &offset, "{\"op\":37,\"d\":{\"subscriptions\":{");
    if (subscription.clear_thread_guild_id) {
        guild(subscription.clear_thread_guild_id);
        write_literal(output.data, &offset, "\"thread_member_lists\":[]},");
    }
    guild(subscription.guild_id);
    bool comma = false;
    if (!subscription.omit_features) {
        write_literal(output.data, &offset, "\"typing\":");
        boolean(subscription.typing);
        write_literal(output.data, &offset, ",\"activities\":");
        boolean(subscription.activities);
        write_literal(output.data, &offset, ",\"threads\":");
        boolean(subscription.threads);
        comma = true;
    }
    if (!subscription.omit_channel_ranges) {
        if (comma)
            write_literal(output.data, &offset, ",");
        write_literal(output.data, &offset, "\"channels\":{\"");
        number(subscription.channel_id);
        write_literal(output.data, &offset, "\":[");
        for (u32 i = 0; i < range_count; ++i) {
            const auto range =
                subscription.range_count
                    ? subscription.ranges[i]
                    : GatewayMemberSubscriptionRange{subscription.range_first, subscription.range_last};
            if (i)
                write_literal(output.data, &offset, ",");
            write_literal(output.data, &offset, "[");
            number(range.first);
            write_literal(output.data, &offset, ",");
            number(range.last);
            write_literal(output.data, &offset, "]");
        }
        write_literal(output.data, &offset, "]");
        for (u32 c = 0; c < subscription.channel_count; ++c) {
            const auto& other = subscription.channels[c];
            write_literal(output.data, &offset, ",\"");
            number(other.channel_id);
            write_literal(output.data, &offset, "\":[");
            for (u32 i = 0; i < other.range_count; ++i) {
                if (i)
                    write_literal(output.data, &offset, ",");
                write_literal(output.data, &offset, "[");
                number(other.ranges[i].first);
                write_literal(output.data, &offset, ",");
                number(other.ranges[i].last);
                write_literal(output.data, &offset, "]");
            }
            write_literal(output.data, &offset, "]");
        }
        write_literal(output.data, &offset, "}");
        comma = true;
    }
    if (subscription.thread_member_lists_present) {
        if (comma)
            write_literal(output.data, &offset, ",");
        write_literal(output.data, &offset, "\"thread_member_lists\":[");
        if (subscription.thread_id) {
            write_literal(output.data, &offset, "\"");
            number(subscription.thread_id);
            write_literal(output.data, &offset, "\"");
        }
        write_literal(output.data, &offset, "]");
    }
    write_literal(output.data, &offset, "}}}}");
    return {offset, GatewayCommandError::none};
}

bool gateway_guild_member_request_valid(const GatewayGuildMemberRequest& request) noexcept {
    if (gateway_guild_member_request_is_query(request))
        return request.guild_id && request.channel_id && request.nonce &&
               request.limit <= kGatewayGuildMemberRequestMaximum &&
               request.query_bytes <= kGatewayGuildMemberQueryMaximumBytes &&
               utf8_validate(Utf8View{request.query, request.query_bytes}).valid;
    if (!request.guild_id || !request.channel_id || !request.nonce || !request.user_count ||
        request.user_count > kGatewayGuildMemberRequestMaximum)
        return false;
    for (u32 i = 0; i < request.user_count; ++i) {
        if (!request.user_ids[i])
            return false;
        for (u32 j = 0; j < i; ++j)
            if (request.user_ids[i] == request.user_ids[j])
                return false;
    }
    return true;
}

GatewayCommandWriteResult gateway_write_guild_member_request(const GatewayGuildMemberRequest& request,
                                                             Slice<u8> output) noexcept {
    if (!gateway_guild_member_request_valid(request))
        return {0, GatewayCommandError::invalid_input};
    constexpr char prefix[] = "{\"op\":8,\"d\":{\"guild_id\":\"";
    if (gateway_guild_member_request_is_query(request)) {
        // The composer's member search: {"guild_id","query","limit","presences":false,"nonce"}.
        constexpr char query[] = "\",\"query\":";
        constexpr char limit[] = ",\"limit\":";
        constexpr char tail[] = ",\"presences\":false,\"nonce\":\"";
        EncodedString text{};
        if (!encoded_string(Utf8View{request.query, request.query_bytes}, false, &text))
            return {0, GatewayCommandError::invalid_utf8};
        const u32 required = static_cast<u32>(sizeof(prefix) + sizeof(query) + sizeof(limit) + sizeof(tail) - 4) +
                             decimal_digits(request.guild_id) + text.bytes + decimal_digits(request.limit) +
                             decimal_digits(request.nonce) + 3;
        if (!output.data || output.count < required)
            return {0, GatewayCommandError::output_too_small};
        u32 offset = 0;
        write_literal(output.data, &offset, prefix);
        offset += decimal_write(output.data + offset, request.guild_id);
        write_literal(output.data, &offset, query);
        write_encoded(output.data, &offset, text);
        write_literal(output.data, &offset, limit);
        offset += decimal_write(output.data + offset, request.limit);
        write_literal(output.data, &offset, tail);
        offset += decimal_write(output.data + offset, request.nonce);
        write_literal(output.data, &offset, "\"}}");
        return {offset, GatewayCommandError::none};
    }
    constexpr char users[] = "\",\"presences\":false,\"user_ids\":[";
    constexpr char nonce[] = "],\"nonce\":\"";
    u32 required = static_cast<u32>(sizeof(prefix) + sizeof(users) + sizeof(nonce) - 3) +
                   decimal_digits(request.guild_id) + decimal_digits(request.nonce) + 3;
    for (u32 i = 0; i < request.user_count; ++i)
        required += decimal_digits(request.user_ids[i]) + 2 + (i ? 1u : 0u);
    if (!output.data || output.count < required)
        return {0, GatewayCommandError::output_too_small};
    u32 offset = 0;
    const auto number = [&](u64 value) noexcept {
        offset += decimal_write(output.data + offset, value);
    };
    write_literal(output.data, &offset, prefix);
    number(request.guild_id);
    write_literal(output.data, &offset, users);
    for (u32 i = 0; i < request.user_count; ++i) {
        if (i)
            write_literal(output.data, &offset, ",");
        write_literal(output.data, &offset, "\"");
        number(request.user_ids[i]);
        write_literal(output.data, &offset, "\"");
    }
    write_literal(output.data, &offset, nonce);
    number(request.nonce);
    write_literal(output.data, &offset, "\"}}");
    return {offset, GatewayCommandError::none};
}
} // namespace maicord
