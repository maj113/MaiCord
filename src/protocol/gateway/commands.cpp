#include "maicord/protocol/gateway/commands.hpp"
#include "maicord/protocol/gateway/voice.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"

#include "../encoding/json_command_writer.hpp"

namespace maicord {
namespace {

using protocol_detail::encoded_string;
using protocol_detail::EncodedString;
using protocol_detail::write_encoded;
using protocol_detail::write_literal;
using Writer = protocol_detail::JsonCommandWriter;

// A command measured before it is written: output stays untouched and the result carries no bytes unless the
// whole command fits.
template <typename Emit> GatewayCommandWriteResult write_command(Slice<u8> output, Emit emit) noexcept {
    GatewayCommandWriteResult result = protocol_detail::write_json_command(output, emit);
    if (!result.ok())
        result.bytes = 0;
    return result;
}

GatewayCommandWriteResult invalid_string_result(const Utf8View& value, bool require_nonempty) noexcept {
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

bool launch_id_valid(Utf8View value) noexcept {
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

} // namespace

GatewayCommandWriteResult gateway_write_voice_state(const GatewayVoiceStateUpdate& state, u32 flags,
                                                    Slice<u8> output) noexcept {
    if (!output.data)
        return {0, GatewayCommandError::invalid_input};
    return write_command(output, [&](Writer& writer) noexcept {
        const auto id = [&](u64 value) noexcept {
            if (value)
                writer.id(value);
            else
                writer.put("null");
        };
        writer.put("{\"op\":4,\"d\":{\"guild_id\":");
        id(state.guild_id);
        writer.put(",\"channel_id\":");
        id(state.channel_id);
        writer.put(",\"self_mute\":", state.self_mute, ",\"self_deaf\":", state.self_deaf, ",\"self_video\":",
                   state.self_video, ",\"flags\":", flags, "}}");
    });
}

namespace {

// One key and its value: an encoded string, or `raw` bytes (numbers and
// literals) copied as they are. Keys carry the separator before them.
struct PropertySegment {
    Utf8View key;
    Utf8View value;
    u32 maximum;
    bool raw;
};

// Properties of a user identify, or with identify null of the request-time
// header. A fast-connect identify carries the page's reduced object.
GatewayCommandWriteResult write_properties(const GatewayUserProperties& properties,
                                           const GatewayUserIdentify* identify, Slice<u8> output) noexcept {
    const bool fast = identify && identify->fast_connect;
    if (properties.client_build_number == 0 || !launch_id_valid(properties.client_launch_id) ||
        (properties.launch_signature.bytes && !launch_id_valid(properties.launch_signature)) ||
        (properties.client_heartbeat_session_id.bytes &&
         !launch_id_valid(properties.client_heartbeat_session_id))) {
        return GatewayCommandWriteResult{0, GatewayCommandError::invalid_input};
    }
    char build[10];
    char native_build[10];
    const Utf8View build_text{build, decimal_write(build, properties.client_build_number)};
    const Utf8View native_text{
        native_build,
        properties.native_build_number ? decimal_write(native_build, properties.native_build_number) : 0u};
    const Utf8View app_state = fast                                      ? Utf8View{}
                               : identify && identify->client_app_state.bytes ? identify->client_app_state
                                                                             : properties.client_app_state;
    const Utf8View heartbeat = fast ? Utf8View{} : properties.client_heartbeat_session_id;
    const PropertySegment markers =
        fast ? PropertySegment{utf8_literal(",\"is_fast_connect\":"), utf8_literal("true"), 0, true}
        : identify
            ? PropertySegment{utf8_literal(",\"is_fast_connect\":false,\"gateway_connect_reasons\":"),
                              identify->connect_reasons.bytes ? identify->connect_reasons : utf8_literal("COLD_START"),
                              kGatewayUserMaximumConnectReasonsBytes, false}
            : PropertySegment{};
    const PropertySegment app_state_segment{utf8_literal(",\"client_app_state\":"), app_state,
                                            kGatewayUserMaximumAppStateBytes, false};
    const PropertySegment heartbeat_segment{utf8_literal(",\"client_heartbeat_session_id\":"), heartbeat,
                                            kGatewayUserLaunchIdBytes, false};
    // An app state the request-time object lacks follows its other properties.
    const bool app_state_first = properties.app_state_first && properties.client_app_state.bytes && !fast;
    // The live client's order: os_version, os_version_is_release, then on Windows os_sdk_version, os_arch ...
    // browser_version, client_build_number. The Linux desktop client sends no os_sdk_version and adds the host's
    // window manager, distro, runtime environment and display server after browser_version.
    constexpr u32 segment_capacity = 24;
    PropertySegment segments[segment_capacity];
    u32 segment_count = 0;
    const auto add = [&](const PropertySegment& segment) { segments[segment_count++] = segment; };
    add({utf8_literal("{\"os\":"), properties.operating_system, kGatewayUserMaximumOperatingSystemBytes, false});
    add({utf8_literal(",\"browser\":"), properties.browser, kGatewayUserMaximumBrowserBytes, false});
    add({utf8_literal(",\"release_channel\":"), properties.release_channel, kGatewayUserMaximumReleaseChannelBytes,
         false});
    add({utf8_literal(",\"client_version\":"), properties.client_version, kGatewayUserMaximumVersionBytes, false});
    add({utf8_literal(",\"os_version\":"), properties.os_version, kGatewayUserMaximumVersionBytes, false});
    add({utf8_literal(",\"os_version_is_release\":"), utf8_literal("true"), 0, true});
    if (!properties.linux_desktop)
        add({utf8_literal(",\"os_sdk_version\":"), properties.os_sdk_version, kGatewayUserMaximumVersionBytes,
             false});
    add({utf8_literal(",\"os_arch\":"), properties.os_arch, kGatewayUserMaximumArchitectureBytes, false});
    add({utf8_literal(",\"app_arch\":"), properties.app_arch, kGatewayUserMaximumArchitectureBytes, false});
    add({utf8_literal(",\"system_locale\":"), properties.system_locale, kGatewayUserMaximumLocaleBytes, false});
    add({utf8_literal(",\"has_client_mods\":false,\"client_launch_id\":"), properties.client_launch_id,
         kGatewayUserLaunchIdBytes, false});
    add({utf8_literal(",\"browser_user_agent\":"), properties.browser_user_agent, kGatewayUserMaximumUserAgentBytes,
         false});
    add({utf8_literal(",\"browser_version\":"), properties.browser_version, kGatewayUserMaximumVersionBytes, false});
    // Fields before client_build_number are required on Windows, the ones before the host's own on Linux; later
    // ones are omitted when empty.
    u32 required_count = segment_count;
    if (properties.linux_desktop) {
        add({utf8_literal(",\"window_manager\":"), properties.window_manager, kGatewayUserMaximumHostBytes, false});
        add({utf8_literal(",\"distro\":"), properties.distro, kGatewayUserMaximumHostBytes, false});
        add({utf8_literal(",\"runtime_environment\":"), properties.runtime_environment,
             kGatewayUserMaximumRuntimeBytes, false});
        add({utf8_literal(",\"display_server\":"), properties.display_server, kGatewayUserMaximumRuntimeBytes,
             false});
    }
    add({utf8_literal(",\"client_build_number\":"), build_text, 0, true});
    if (!properties.linux_desktop)
        required_count = segment_count;
    add({utf8_literal(",\"native_build_number\":"), native_text, 0, true});
    add({utf8_literal(",\"client_event_source\":"), utf8_literal("null"), 0, true});
    add({utf8_literal(",\"launch_signature\":"), fast ? Utf8View{} : properties.launch_signature,
         kGatewayUserLaunchIdBytes, false});
    add(app_state_first ? app_state_segment : heartbeat_segment);
    add(app_state_first ? heartbeat_segment : app_state_segment);
    add(markers);
    EncodedString encoded[segment_capacity]{};
    for (u32 index = 0; index < segment_count; ++index) {
        const PropertySegment& segment = segments[index];
        if (segment.raw || (index >= required_count && segment.value.bytes == 0))
            continue;
        const GatewayCommandWriteResult field = encode_bounded(segment.value, segment.maximum, &encoded[index]);
        if (!field.ok())
            return field;
    }
    const auto emit = [&](Writer& writer) noexcept {
        for (u32 index = 0; index < segment_count; ++index) {
            const PropertySegment& segment = segments[index];
            if (index >= required_count && segment.value.bytes == 0)
                continue;
            writer.put(segment.key);
            if (segment.raw)
                writer.put(segment.value);
            else
                writer.put(encoded[index]);
        }
        writer.byte('}');
    };
    Writer measure{};
    emit(measure);
    if (!output.data)
        return GatewayCommandWriteResult{static_cast<u32>(measure.at), GatewayCommandError::none};
    if (measure.at > output.count)
        return GatewayCommandWriteResult{0, GatewayCommandError::output_too_small};
    Writer writer{output.data, 0};
    emit(writer);
    return GatewayCommandWriteResult{static_cast<u32>(writer.at), GatewayCommandError::none};
}

// Standard base64 with padding, as the client encodes the token.
bool qos_token_valid(Utf8View token) noexcept {
    if (!token.bytes)
        return true;
    if (!token.data || token.bytes > kGatewayUserMaximumQosTokenBytes || token.bytes % 4)
        return false;
    for (u32 i = 0; i < token.bytes; ++i) {
        const char c = token.data[i];
        const bool pad = c == '=' && i + 2 >= token.bytes && (i + 1 == token.bytes || token.data[i + 1] == '=');
        if (!pad && !ascii_is_alnum(c) && c != '+' && c != '/')
            return false;
    }
    return true;
}

} // namespace

GatewayCommandWriteResult gateway_write_user_properties(const GatewayUserProperties& properties,
                                                        const Slice<u8>& output) noexcept {
    return write_properties(properties, nullptr, output);
}

GatewayCommandWriteResult gateway_write_user_identify(const GatewayUserIdentify& identify,
                                                      const Slice<u8>& output) noexcept {
    if (!output.data) {
        return GatewayCommandWriteResult{0, GatewayCommandError::invalid_input};
    }
    const GatewayCommandWriteResult properties =
        write_properties(identify.properties, &identify, {});
    if (!properties.ok()) {
        return properties;
    }
    const Utf8View status_view = gateway_user_status_name(identify.status);
    if (!status_view.data) {
        return GatewayCommandWriteResult{0, GatewayCommandError::invalid_input};
    }

    EncodedString token{};
    EncodedString status{};
    const GatewayCommandWriteResult fields[] = {
        encode_bounded(identify.token, kGatewayUserMaximumTokenBytes, &token),
        encode_bounded(status_view, 9, &status),
    };
    for (u32 index = 0; index < static_cast<u32>(sizeof(fields) / sizeof(fields[0])); ++index) {
        if (!fields[index].ok()) {
            return fields[index];
        }
    }

    if (!qos_token_valid(identify.qos_token))
        return GatewayCommandWriteResult{0, GatewayCommandError::invalid_input};
    const auto emit = [&](Writer& writer) noexcept {
        writer.put("{\"op\":2,\"d\":{\"token\":", token, ",\"capabilities\":", kGatewayUserCapabilitiesDefault,
                   ",\"properties\":");
        if (writer.output)
            write_properties(identify.properties, &identify, {writer.output + writer.at, properties.bytes});
        writer.at += properties.bytes;
        if (identify.fast_connect) {
            writer.put(",\"client_state\":{\"guild_versions\":{}}}}");
            return;
        }
        // The presence in the desktop client's order; since is 0 unless idle.
        writer.put(",\"presence\":{\"status\":", status, ",\"since\":", identify.has_since ? identify.since_ms : u64{0},
                   ",\"activities\":[],\"afk\":", identify.afk,
                   "},\"compress\":false,\"client_state\":{\"guild_versions\":{}}");
        if (identify.qos_token.bytes)
            writer.put(",\"qos_token\":\"", identify.qos_token, '"');
        writer.put("}}");
    };
    Writer measure{};
    emit(measure);
    if (measure.at > kGatewayUserMaximumCommandBytes)
        return GatewayCommandWriteResult{0, GatewayCommandError::value_too_large};
    if (measure.at > output.count)
        return GatewayCommandWriteResult{0, GatewayCommandError::output_too_small};
    Writer writer{output.data, 0};
    emit(writer);
    return GatewayCommandWriteResult{static_cast<u32>(writer.at), GatewayCommandError::none};
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

GatewayCommandWriteResult gateway_write_time_spent_session(u64 initialization_ms, Utf8View session_id,
                                                           Utf8View client_launch_id,
                                                           Slice<u8> output) noexcept {
    if (!output.data || !launch_id_valid(session_id) || !launch_id_valid(client_launch_id))
        return {0, GatewayCommandError::invalid_input};
    constexpr u32 fixed_bytes = sizeof("{\"op\":41,\"d\":{\"initialization_timestamp\":,\"session_id\":"
                                       "\"\",\"client_launch_id\":\"\"}}") -
                                1;
    const u32 required = fixed_bytes + decimal_digits(initialization_ms) + 2 * kGatewayUserLaunchIdBytes;
    if (required > output.count)
        return {0, GatewayCommandError::output_too_small};
    u32 offset = 0;
    write_literal(output.data, &offset, "{\"op\":41,\"d\":{\"initialization_timestamp\":");
    offset += decimal_write(output.data + offset, initialization_ms);
    write_literal(output.data, &offset, ",\"session_id\":\"");
    copy_bytes(output.data + offset, session_id.data, session_id.bytes);
    offset += session_id.bytes;
    write_literal(output.data, &offset, "\",\"client_launch_id\":\"");
    copy_bytes(output.data + offset, client_launch_id.data, client_launch_id.bytes);
    offset += client_launch_id.bytes;
    write_literal(output.data, &offset, "\"}}");
    return {offset, GatewayCommandError::none};
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
    for (u32 i = 0; i < range_count; ++i) {
        const auto range =
            subscription.range_count
                ? subscription.ranges[i]
                : GatewayMemberSubscriptionRange{subscription.range_first, subscription.range_last};
        if (range.last < range.first || range.last - range.first > 999u ||
            (subscription.range_count && range.last - range.first >= 100u) ||
            (i && range.first <= subscription.ranges[i - 1].last))
            return {0, GatewayCommandError::invalid_input};
    }
    // The guild's other remembered channels: distinct, never empty, each with
    // ascending bounded ranges.
    if (subscription.channel_count > kGatewayGuildSubscriptionMaximumChannels - 1 ||
        (subscription.channel_count && subscription.omit_channel_ranges))
        return {0, GatewayCommandError::invalid_input};
    for (u32 c = 0; c < subscription.channel_count; ++c) {
        const auto& other = subscription.channels[c];
        if (!other.channel_id || other.channel_id == subscription.channel_id || !other.range_count ||
            other.range_count > kGatewayMemberSubscriptionMaximumRanges)
            return {0, GatewayCommandError::invalid_input};
        for (u32 earlier = 0; earlier < c; ++earlier)
            if (subscription.channels[earlier].channel_id == other.channel_id)
                return {0, GatewayCommandError::invalid_input};
        for (u32 i = 0; i < other.range_count; ++i) {
            const auto range = other.ranges[i];
            if (range.last < range.first || range.last - range.first >= 100u ||
                (i && range.first <= other.ranges[i - 1].last))
                return {0, GatewayCommandError::invalid_input};
        }
    }
    // The users whose presence and member updates the client follows in the guild: bounded, distinct, nonzero.
    if (subscription.member_count > kGatewayGuildSubscriptionMaximumMembers ||
        (subscription.member_count && !subscription.members_present))
        return {0, GatewayCommandError::invalid_input};
    for (u32 i = 0; i < subscription.member_count; ++i) {
        if (!subscription.members[i])
            return {0, GatewayCommandError::invalid_input};
        for (u32 earlier = 0; earlier < i; ++earlier)
            if (subscription.members[earlier] == subscription.members[i])
                return {0, GatewayCommandError::invalid_input};
    }
    if (subscription.omit_features && !subscription.members_present && subscription.omit_channel_ranges &&
        !subscription.thread_member_lists_present)
        return {0, GatewayCommandError::invalid_input};
    return write_command(output, [&](Writer& writer) noexcept {
        const auto ranges = [&](const GatewayMemberSubscriptionRange* list, u32 count) noexcept {
            for (u32 i = 0; i < count; ++i) {
                if (i)
                    writer.byte(',');
                writer.put('[', list[i].first, ',', list[i].last, ']');
            }
        };
        writer.put("{\"op\":37,\"d\":{\"subscriptions\":{");
        if (subscription.clear_thread_guild_id)
            writer.put('"', subscription.clear_thread_guild_id, "\":{\"thread_member_lists\":[]},");
        writer.put('"', subscription.guild_id, "\":{");
        bool comma = false;
        if (!subscription.omit_features) {
            writer.put("\"typing\":", subscription.typing, ",\"activities\":", subscription.activities,
                       ",\"threads\":", subscription.threads);
            comma = true;
        }
        if (subscription.members_present) {
            if (comma)
                writer.byte(',');
            writer.put("\"members\":[");
            for (u32 i = 0; i < subscription.member_count; ++i) {
                if (i)
                    writer.byte(',');
                writer.id(subscription.members[i]);
            }
            writer.byte(']');
            comma = true;
        }
        if (!subscription.omit_channel_ranges) {
            const GatewayMemberSubscriptionRange single{subscription.range_first, subscription.range_last};
            if (comma)
                writer.byte(',');
            writer.put("\"channels\":{\"", subscription.channel_id, "\":[");
            ranges(subscription.range_count ? subscription.ranges : &single, range_count);
            writer.byte(']');
            for (u32 c = 0; c < subscription.channel_count; ++c) {
                const auto& other = subscription.channels[c];
                writer.put(",\"", other.channel_id, "\":[");
                ranges(other.ranges, other.range_count);
                writer.byte(']');
            }
            writer.byte('}');
            comma = true;
        }
        if (subscription.thread_member_lists_present) {
            if (comma)
                writer.byte(',');
            writer.put("\"thread_member_lists\":[");
            if (subscription.thread_id)
                writer.id(subscription.thread_id);
            writer.byte(']');
        }
        writer.put("}}}}");
    });
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
    EncodedString text{};
    const bool query = gateway_guild_member_request_is_query(request);
    if (query && !encoded_string(Utf8View{request.query, request.query_bytes}, false, &text))
        return {0, GatewayCommandError::invalid_utf8};
    if (!output.data)
        return {0, GatewayCommandError::output_too_small};
    return write_command(output, [&](Writer& writer) noexcept {
        writer.put("{\"op\":8,\"d\":{\"guild_id\":\"", request.guild_id);
        if (query) {
            // The composer's member search: {"guild_id","query","limit","presences":false,"nonce"}.
            writer.put("\",\"query\":", text, ",\"limit\":", request.limit, ",\"presences\":false,\"nonce\":\"");
        } else {
            writer.put("\",\"presences\":false,\"user_ids\":[");
            for (u32 i = 0; i < request.user_count; ++i) {
                if (i)
                    writer.byte(',');
                writer.id(request.user_ids[i]);
            }
            writer.put("],\"nonce\":\"");
        }
        writer.put(request.nonce, "\"}}");
    });
}
} // namespace maicord
