#include "maicord/protocol/gateway_events.hpp"
#include "maicord/protocol/json_ids.hpp"

namespace maicord {
namespace {

struct HelloContext {
    GatewayHello hello;
    GatewayEventDecodeError error;
    bool heartbeat_seen;
};

struct ReadyContext {
    GatewayReady ready;
    GatewayEventDecodeError error;
    u32 fields;
};

struct ResumedContext {
    GatewayResumed resumed;
    GatewayEventDecodeError error;
    bool trace_seen;
};
struct SupplementalPrivateContext { JsonValue channels; bool seen; GatewayEventDecodeError error; };
bool visit_supplemental_private(void* opaque, JsonString name, JsonValue value) noexcept {
    if (!json_string_equals_ascii(name, utf8_literal("lazy_private_channels"))) return true;
    auto& context = *static_cast<SupplementalPrivateContext*>(opaque);
    if (context.seen || (value.kind != JsonValueKind::array && value.kind != JsonValueKind::null_value)) {
        context.error = context.seen ? GatewayEventDecodeError::duplicate_field : GatewayEventDecodeError::invalid_data;
        return false;
    }
    context.channels = value;
    context.seen = true;
    return true;
}

struct ReadyUserContext {
    u64 id;
    GatewayEventDecodeError error;
    bool seen;
};

bool visit_ready_user_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ReadyUserContext*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("id"))) {
        return true;
    }
    if (context->seen) {
        context->error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    context->seen = true;
    if (!json_value_to_snowflake(value, &context->id)) {
        context->error = GatewayEventDecodeError::invalid_user;
        return false;
    }
    return true;
}

bool mark_field(u32* fields, u32 bit, GatewayEventDecodeError* error) noexcept {
    if ((*fields & bit) != 0) {
        *error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    *fields |= bit;
    return true;
}

bool visit_hello_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<HelloContext*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("heartbeat_interval"))) {
        return true;
    }
    if (context->heartbeat_seen) {
        context->error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    context->heartbeat_seen = true;
    u64 interval = 0;
    if (!json_value_to_u64(value, &interval) || interval == 0 || interval > 0xFFFFFFFFull) {
        context->error = GatewayEventDecodeError::invalid_heartbeat_interval;
        return false;
    }
    context->hello.heartbeat_interval_ms = static_cast<u32>(interval);
    return true;
}

bool visit_ready_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ReadyContext*>(opaque);
    // Policy fields are kept raw for the preflight producer, so one malformed
    // optional field cannot fail READY.
    u32 policy_bit=0;JsonValue*policy=nullptr;
    if(json_string_equals_ascii(name,utf8_literal("user_guild_settings"))){policy_bit=1u<<15;policy=&context->ready.user_guild_settings;}
    else if(json_string_equals_ascii(name,utf8_literal("user_settings_proto"))){policy_bit=1u<<16;policy=&context->ready.user_settings_proto;}
    else if(json_string_equals_ascii(name,utf8_literal("notification_settings"))){policy_bit=1u<<17;policy=&context->ready.notification_settings;}
    if(policy){if(context->fields&policy_bit)context->ready.notification_policy_invalid=true;
        context->fields|=policy_bit;*policy=value;return true;}
    if (json_string_equals_ascii(name, utf8_literal("v"))) {
        if (!mark_field(&context->fields, 1u << 0, &context->error)) {
            return false;
        }
        u64 version = 0;
        if (!json_value_to_u64(value, &version) || version == 0 || version > 0xFFFFFFFFull) {
            context->error = GatewayEventDecodeError::invalid_api_version;
            return false;
        }
        context->ready.api_version = static_cast<u32>(version);
    } else if (json_string_equals_ascii(name, utf8_literal("user"))) {
        if (!mark_field(&context->fields, 1u << 1, &context->error)) {
            return false;
        }
        if (value.kind != JsonValueKind::object) {
            context->error = GatewayEventDecodeError::invalid_user;
            return false;
        }
        context->ready.user = value;
    } else if (json_string_equals_ascii(name, utf8_literal("guilds"))) {
        if (!mark_field(&context->fields, 1u << 2, &context->error)) {
            return false;
        }
        if (value.kind != JsonValueKind::array) {
            context->error = GatewayEventDecodeError::invalid_guilds;
            return false;
        }
        context->ready.guilds = value;
    } else if (json_string_equals_ascii(name, utf8_literal("private_channels"))) {
        if (!mark_field(&context->fields, 1u << 5, &context->error)) return false;
        if (value.kind != JsonValueKind::array) {
            context->error = GatewayEventDecodeError::invalid_data;
            return false;
        }
        context->ready.private_channels = value;
    } else if (json_string_equals_ascii(name, utf8_literal("required_action"))) {
        if (!mark_field(&context->fields, 1u << 11, &context->error)) return false;
        context->ready.required_action = decode_account_requirement(value);
    } else if (json_string_equals_ascii(name, utf8_literal("relationships"))) {
        if (!mark_field(&context->fields, 1u << 12, &context->error)) return false;
        if (value.kind != JsonValueKind::array && value.kind != JsonValueKind::null_value) {
            context->error = GatewayEventDecodeError::invalid_data; return false;
        }
        context->ready.relationships = value;
    } else if (json_string_equals_ascii(name, utf8_literal("merged_presences"))) {
        if (!mark_field(&context->fields, 1u << 13, &context->error)) return false;
        if (value.kind != JsonValueKind::object && value.kind != JsonValueKind::null_value) {
            context->error = GatewayEventDecodeError::invalid_data; return false;
        }
        context->ready.merged_presences = value;
    } else if (json_string_equals_ascii(name, utf8_literal("read_state"))) {
        if (!mark_field(&context->fields, 1u << 14, &context->error)) return false;
        if (value.kind != JsonValueKind::object) {
            context->error = GatewayEventDecodeError::invalid_data; return false;
        }
        context->ready.read_state = value;
    } else if (json_string_equals_ascii(name, utf8_literal("apex_experiments"))) {
        if (!mark_field(&context->fields, 1u << 10, &context->error)) return false;
        context->ready.apex_experiments = value;
    } else if (json_string_equals_ascii(name, utf8_literal("users"))) {
        if (!mark_field(&context->fields, 1u << 6, &context->error)) return false;
        if (value.kind != JsonValueKind::array) {
            context->error = GatewayEventDecodeError::invalid_user;
            return false;
        }
        context->ready.users = value;
    } else if (json_string_equals_ascii(name, utf8_literal("session_id"))) {
        if (!mark_field(&context->fields, 1u << 3, &context->error)) {
            return false;
        }
        if (!json_value_as_string(value, &context->ready.session_id) ||
            context->ready.session_id.encoded.bytes == 0) {
            context->error = GatewayEventDecodeError::invalid_session_id;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("resume_gateway_url"))) {
        if (!mark_field(&context->fields, 1u << 4, &context->error)) {
            return false;
        }
        if (!json_value_as_string(value, &context->ready.resume_gateway_url) ||
            context->ready.resume_gateway_url.encoded.bytes == 0) {
            context->error = GatewayEventDecodeError::invalid_resume_gateway_url;
            return false;
        }
    } else if (json_string_equals_ascii(name, utf8_literal("auth_session_id_hash"))) {
        // Optional; identifies this session in the Devices list.
        context->ready.auth_session_id_hash = value;
    }
    return true;
}

bool visit_resumed_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ResumedContext*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("_trace"))) {
        return true;
    }
    if (context->trace_seen) {
        context->error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    context->trace_seen = true;
    if (value.kind != JsonValueKind::array) {
        context->error = GatewayEventDecodeError::invalid_trace;
        return false;
    }
    context->resumed.trace = value;
    context->resumed.has_trace = true;
    return true;
}

GatewayEventDecodeResult failure(GatewayEventDecodeError error, u32 offset = 0,
                                 JsonScanError scan_error = JsonScanError::none,
                                 JsonReadError read_error = JsonReadError::none) noexcept {
    return GatewayEventDecodeResult{offset, error, scan_error, read_error};
}

} // namespace

GatewayEventDecodeResult gateway_decode_hello(JsonValue data, Slice<u32> structural_scratch,
                                              u32 maximum_depth, GatewayHello* output) noexcept {
    if (!output || !data.source.data || data.source.bytes == 0 || !structural_scratch.data ||
        structural_scratch.count == 0 || maximum_depth == 0) {
        return failure(GatewayEventDecodeError::invalid_input);
    }
    if (data.kind != JsonValueKind::object) {
        return failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    }

    const JsonScanResult scan = json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok()) {
        return failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    HelloContext context{};
    const JsonReadResult read =
        json_visit_value_object(data, ConstSlice<u32>{structural_scratch.data, scan.count},
                               maximum_depth, visit_hello_member, &context);
    if (!read.ok()) {
        if (context.error != GatewayEventDecodeError::none) {
            return failure(context.error, read.error_offset, JsonScanError::none, read.error);
        }
        return failure(GatewayEventDecodeError::invalid_json, read.error_offset, JsonScanError::none,
                       read.error);
    }
    if (!context.heartbeat_seen) {
        return failure(GatewayEventDecodeError::missing_heartbeat_interval, data.source_offset);
    }
    *output = context.hello;
    return failure(GatewayEventDecodeError::none);
}

GatewayEventDecodeResult gateway_decode_ready(JsonValue data, Slice<u32> structural_scratch,
                                              u32 maximum_depth, GatewayReady* output) noexcept {
    if (!output || !data.source.data || data.source.bytes == 0 || !structural_scratch.data ||
        structural_scratch.count == 0 || maximum_depth == 0) {
        return failure(GatewayEventDecodeError::invalid_input);
    }
    if (data.kind != JsonValueKind::object) {
        return failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    }
    const JsonScanResult scan = json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok()) {
        return failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    ReadyContext context{};
    const JsonReadResult read =
        json_visit_value_object(data, ConstSlice<u32>{structural_scratch.data, scan.count},
                               maximum_depth, visit_ready_member, &context);
    if (!read.ok()) {
        return context.error != GatewayEventDecodeError::none
                   ? failure(context.error, read.error_offset, JsonScanError::none, read.error)
                   : failure(GatewayEventDecodeError::invalid_json, read.error_offset, JsonScanError::none,
                             read.error);
    }
    constexpr u32 required_fields = (1u << 5) - 1;
    if ((context.fields & required_fields) != required_fields) {
        return failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    }
    const JsonScanResult user_scan =
        json_scan_structurals_avx1(context.ready.user.source, structural_scratch);
    if (!user_scan.ok()) {
        return failure(GatewayEventDecodeError::scan_failed, user_scan.error_offset, user_scan.error);
    }
    ReadyUserContext user{};
    const JsonReadResult user_read = json_visit_value_object(
        context.ready.user, ConstSlice<u32>{structural_scratch.data, user_scan.count},
        maximum_depth, visit_ready_user_member, &user);
    if (!user_read.ok() || user.error != GatewayEventDecodeError::none || !user.seen) {
        return failure(user.error != GatewayEventDecodeError::none
                           ? user.error
                           : GatewayEventDecodeError::invalid_user,
                       context.ready.user.source_offset, JsonScanError::none, user_read.error);
    }
    context.ready.user_id = user.id;
    *output = context.ready;
    return failure(GatewayEventDecodeError::none);
}

GatewayEventDecodeResult gateway_decode_resumed(JsonValue data, Slice<u32> structural_scratch,
                                                u32 maximum_depth, GatewayResumed* output) noexcept {
    if (!output || !data.source.data || data.source.bytes == 0 || !structural_scratch.data ||
        structural_scratch.count == 0 || maximum_depth == 0) {
        return failure(GatewayEventDecodeError::invalid_input);
    }
    if (data.kind != JsonValueKind::object) {
        return failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    }
    const JsonScanResult scan = json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok()) {
        return failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    ResumedContext context{};
    const JsonReadResult read =
        json_visit_value_object(data, ConstSlice<u32>{structural_scratch.data, scan.count},
                               maximum_depth, visit_resumed_member, &context);
    if (!read.ok()) {
        return context.error != GatewayEventDecodeError::none
                   ? failure(context.error, read.error_offset, JsonScanError::none, read.error)
                   : failure(GatewayEventDecodeError::invalid_json, read.error_offset, JsonScanError::none,
                             read.error);
    }
    *output = context.resumed;
    return failure(GatewayEventDecodeError::none);
}

GatewayEventDecodeResult gateway_decode_supplemental_private_channels(JsonValue data, Slice<u32> scratch,
    u32 maximum_depth, JsonValue* channels) noexcept {
    if (!channels || !data.source.data || !scratch.data || !scratch.count || !maximum_depth)
        return failure(GatewayEventDecodeError::invalid_input);
    *channels = {};
    if (data.kind != JsonValueKind::object) return failure(GatewayEventDecodeError::invalid_data);
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok()) return failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    SupplementalPrivateContext context{};
    const auto read = json_visit_value_object(data, {scratch.data, scan.count}, maximum_depth, visit_supplemental_private, &context);
    if (!read.ok()) return failure(context.error == GatewayEventDecodeError::none ? GatewayEventDecodeError::invalid_json :
        context.error, read.error_offset, JsonScanError::none, read.error);
    *channels = context.channels;
    return {};
}

GatewayEventDecodeResult gateway_decode_invalid_session(JsonValue data, bool* resumable) noexcept {
    if (!resumable || !data.source.data || data.source.bytes == 0) {
        return failure(GatewayEventDecodeError::invalid_input);
    }
    if (!json_value_to_bool(data, resumable)) {
        return failure(GatewayEventDecodeError::invalid_resumable, data.source_offset);
    }
    return failure(GatewayEventDecodeError::none);
}

} // namespace maicord
