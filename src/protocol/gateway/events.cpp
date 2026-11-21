#include "maicord/protocol/gateway/events.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "maicord/protocol/auth/account_requirement.hpp"
#include "decode.hpp"
#include "../encoding/json_field_lists.hpp"

// Connection-level events (hello, ready, resumed, rate limits), once per session: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
namespace maicord {
namespace {

using protocol_detail::accept_member;
using protocol_detail::member_bit;
using protocol_detail::member_index;
using protocol_detail::decode_failure;
using protocol_detail::mark_seen;
using protocol_detail::read_failure;

struct HelloContext {
    GatewayHello hello;
    GatewayEventDecodeError error;
    bool heartbeat_seen;
};

struct RateLimitedContext {
    GatewayRateLimited value;
    JsonValue meta;
    u32 fields;
    GatewayEventDecodeError error;
};

bool rate_seconds(JsonValue value, u64* output) noexcept {
    if (value.kind != JsonValueKind::number || !value.source.data || !value.source.bytes ||
        value.source.bytes > 64 || value.source.data[0] == '-')
        return false;
    char digits[64];
    u32 count = 0, before_decimal = 0, at = 0;
    bool decimal = false;
    while (at < value.source.bytes && value.source.data[at] != 'e' && value.source.data[at] != 'E') {
        const char byte = value.source.data[at++];
        if (byte == '.') {
            decimal = true;
            continue;
        }
        if (byte < '0' || byte > '9')
            return false;
        digits[count++] = byte;
        if (!decimal)
            ++before_decimal;
    }
    i64 exponent = 0;
    if (at < value.source.bytes) {
        ++at;
        bool negative = false;
        if (at < value.source.bytes && (value.source.data[at] == '+' || value.source.data[at] == '-'))
            negative = value.source.data[at++] == '-';
        if (at == value.source.bytes)
            return false;
        while (at < value.source.bytes) {
            const char byte = value.source.data[at++];
            if (byte < '0' || byte > '9')
                return false;
            if (exponent < 10000)
                exponent = exponent * 10 + byte - '0';
        }
        if (negative)
            exponent = -exponent;
    }
    u32 leading = 0;
    while (leading < count && digits[leading] == '0')
        ++leading;
    if (leading == count) {
        *output = 0;
        return true;
    }
    const i64 integer_digits = static_cast<i64>(before_decimal) - leading + exponent + 3;
    if (integer_digits > 20)
        return false;
    u64 milliseconds = 0;
    for (i64 index = 0; index < integer_digits; ++index) {
        const u64 digit =
            index < static_cast<i64>(count - leading) ? static_cast<u64>(digits[leading + index] - '0') : 0;
        if (milliseconds > (~u64{0} - digit) / 10)
            return false;
        milliseconds = milliseconds * 10 + digit;
    }
    bool round_up = false;
    for (u32 index = integer_digits > 0 ? static_cast<u32>(integer_digits) : 0; index < count - leading;
         ++index)
        round_up |= digits[leading + index] != '0';
    if (round_up && milliseconds == ~u64{0})
        return false;
    *output = milliseconds + (round_up ? 1 : 0);
    return true;
}

bool visit_rate_limited(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& context = *static_cast<RateLimitedContext*>(opaque);
    switch (member_index<"opcode", "retry_after", "meta">(name)) {
    case 0: {
        u64 opcode = 0;
        if (!mark_seen(&context.fields, 1, &context.error))
            return false;
        if (!json_value_to_u64(value, &opcode) || opcode > ~u32{0})
            return false;
        context.value.opcode = static_cast<u32>(opcode);
        break;
    }
    case 1:
        if (!mark_seen(&context.fields, 2, &context.error) ||
            !rate_seconds(value, &context.value.retry_after_ms))
            return false;
        break;
    case 2:
        if (!mark_seen(&context.fields, 4, &context.error) || value.kind != JsonValueKind::object)
            return false;
        context.meta = value;
        break;
    default: break;
    }
    return true;
}

bool visit_rate_meta(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& context = *static_cast<RateLimitedContext*>(opaque);
    switch (member_index<"guild_id", "nonce">(name)) {
    case 0:
        if (!mark_seen(&context.fields, 8, &context.error) ||
            !json_value_to_snowflake(value, &context.value.guild_id) || !context.value.guild_id)
            return false;
        break;
    case 1: {
        JsonString nonce{};
        if (!mark_seen(&context.fields, 16, &context.error) || !json_value_as_string(value, &nonce))
            return false;
        // Non-decimal peer nonces still carry an authoritative guild deadline.
        if (!json_string_to_u64(nonce, &context.value.nonce))
            context.value.nonce = 0;
        break;
    }
    default: break;
    }
    return true;
}

struct MemberChunkContext {
    GatewayMemberChunkReceipt value;
    u32 fields;
    GatewayEventDecodeError error;
};
bool visit_member_chunk_receipt(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& context = *static_cast<MemberChunkContext*>(opaque);
    const u32 bit = member_bit<"guild_id", "nonce", "chunk_index", "chunk_count">(name);
    if (!bit)
        return true;
    if (!mark_seen(&context.fields, bit, &context.error))
        return false;
    if (bit == 1)
        return json_value_to_snowflake(value, &context.value.guild_id) && context.value.guild_id;
    if (bit == 2) {
        JsonString nonce{};
        if (!json_value_as_string(value, &nonce))
            return false;
        if (!json_string_to_u64(nonce, &context.value.nonce))
            context.value.nonce = 0;
        return true;
    }
    u64 number = 0;
    if (!json_value_to_u64(value, &number) || number > ~u32{0})
        return false;
    if (bit == 4)
        context.value.chunk_index = static_cast<u32>(number);
    else
        context.value.chunk_count = static_cast<u32>(number);
    return true;
}

struct ReadyContext {
    GatewayReady ready;
    GatewayEventDecodeError error;
    u32 fields;
};

struct ResumedContext {
    GatewayEventDecodeError error;
    u32 seen;
};
struct SupplementalPrivateContext {
    JsonValue channels;
    bool seen;
    GatewayEventDecodeError error;
};
bool visit_supplemental_private(void* opaque, JsonString name, JsonValue value) noexcept {
    if (!json_string_equals_ascii(name, utf8_literal("lazy_private_channels")))
        return true;
    auto& context = *static_cast<SupplementalPrivateContext*>(opaque);
    if (context.seen || (value.kind != JsonValueKind::array && value.kind != JsonValueKind::null_value)) {
        context.error =
            context.seen ? GatewayEventDecodeError::duplicate_field : GatewayEventDecodeError::invalid_data;
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

// Optional ancillary member: a repeat discards the value instead of failing READY.
void keep_first(ReadyContext* context, u32 bit, JsonValue* target, JsonValue value) noexcept {
    if (context->fields & bit)
        target->kind = JsonValueKind::null_value;
    else
        *target = value;
    context->fields |= bit;
}

bool visit_ready_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ReadyContext*>(opaque);
    auto& ready = context->ready;
    auto& fields = context->fields;
    auto& error = context->error;
    // Policy fields are kept raw for the preflight producer, so one malformed
    // optional field cannot fail READY.
    const u32 index = member_index<"user_guild_settings", "user_settings_proto", "notification_settings", "v", "user",
                                   "guilds", "private_channels", "required_action", "relationships",
                                   "merged_presences", "merged_members", "read_state", "linked_users",
                                   "regional_feature_config", "apex_experiments", "users", "session_id",
                                   "resume_gateway_url", "auth_session_id_hash", "qos_token", "auth_token">(name);
    if (index < 3) {
        const u32 policy_bit = 1u << (15 + index);
        JsonValue* const policy = index == 0   ? &ready.user_guild_settings
                                  : index == 1 ? &ready.user_settings_proto
                                               : &ready.notification_settings;
        if (fields & policy_bit)
            ready.notification_policy_invalid = true;
        fields |= policy_bit;
        *policy = value;
        return true;
    }
    // A member kept as a raw value of the expected shape. The cases share one copy of the check.
    const bool array = value.kind == JsonValueKind::array, object = value.kind == JsonValueKind::object,
               null = value.kind == JsonValueKind::null_value;
    struct Kept {
        u32 bit;
        bool shape;
        JsonValue* slot;
        GatewayEventDecodeError invalid;
    } kept;
    constexpr auto invalid_data = GatewayEventDecodeError::invalid_data;
    switch (index) {
    case 3: {
        u64 version = 0;
        if (!accept_member(&fields, 1u << 0,
                           json_value_to_u64(value, &version) && version != 0 && version <= 0xFFFFFFFFull,
                           &error, GatewayEventDecodeError::invalid_api_version))
            return false;
        ready.api_version = static_cast<u32>(version);
        return true;
    }
    case 4: kept = {1u << 1, object, &ready.user, GatewayEventDecodeError::invalid_user}; break;
    case 5: kept = {1u << 2, array, &ready.guilds, GatewayEventDecodeError::invalid_guilds}; break;
    case 6: kept = {1u << 5, array, &ready.private_channels, invalid_data}; break;
    case 7:
        if (!mark_seen(&fields, 1u << 11, &error))
            return false;
        ready.required_action = decode_account_requirement(value);
        return true;
    case 8: kept = {1u << 12, array || null, &ready.relationships, invalid_data}; break;
    case 9: kept = {1u << 13, object || null, &ready.merged_presences, invalid_data}; break;
    case 10:
        // Optional member authority is checked per guild during projection.
        keep_first(context, 1u << 18, &ready.merged_members, value);
        return true;
    case 11: kept = {1u << 14, object, &ready.read_state, invalid_data}; break;
    case 12: keep_first(context, 1u << 20, &ready.linked_users, value); return true;
    case 13:
        // Ancillary account policy is normalized during READY projection.
        keep_first(context, 1u << 19, &ready.regional_feature_config, value);
        return true;
    case 14:
        if (!mark_seen(&fields, 1u << 10, &error))
            return false;
        ready.apex_experiments = value;
        return true;
    case 15: kept = {1u << 6, array, &ready.users, GatewayEventDecodeError::invalid_user}; break;
    case 16:
        return accept_member(&fields, 1u << 3,
                             json_value_as_string(value, &ready.session_id) && ready.session_id.encoded.bytes != 0,
                             &error, GatewayEventDecodeError::invalid_session_id);
    case 17:
        return accept_member(&fields, 1u << 4,
                             json_value_as_string(value, &ready.resume_gateway_url) &&
                                 ready.resume_gateway_url.encoded.bytes != 0,
                             &error, GatewayEventDecodeError::invalid_resume_gateway_url);
    case 18:
        // Optional; identifies this session in the Devices list.
        ready.auth_session_id_hash = value;
        return true;
    case 19: ready.qos_token = value; return true;
    case 20: ready.auth_token = value; return true;
    default: return true;
    }
    if (!accept_member(&fields, kept.bit, kept.shape, &error, kept.invalid))
        return false;
    *kept.slot = value;
    return true;
}

bool visit_resumed_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<ResumedContext*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("_trace"))) {
        return true;
    }
    return accept_member(&context->seen, 1, value.kind == JsonValueKind::array, &context->error,
                         GatewayEventDecodeError::invalid_trace);
}

// Scans and visits the members of an object value.
GatewayEventDecodeResult visit_object(JsonValue data, const Slice<u32>& scratch, u32 maximum_depth,
                                      JsonMemberVisitor visitor, void* context,
                                      const GatewayEventDecodeError* visitor_error,
                                      GatewayEventDecodeError fallback) noexcept {
    const JsonScanResult scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok()) {
        return decode_failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    }
    const JsonReadResult read = json_visit_value_object(data, ConstSlice<u32>{scratch.data, scan.count},
                                                        maximum_depth, visitor, context);
    return read.ok() ? decode_failure(GatewayEventDecodeError::none)
                     : read_failure(*visitor_error, read, fallback);
}

} // namespace

GatewayEventDecodeResult gateway_decode_rate_limited(JsonValue data, Slice<u32> structural_scratch,
                                                     u32 maximum_depth,
                                                     GatewayRateLimited* output) noexcept {
    if (data.source.bytes > 1024)
        return decode_failure(GatewayEventDecodeError::invalid_input);
    if (data.kind != JsonValueKind::object)
        return decode_failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    RateLimitedContext context{};
    auto result = visit_object(data, structural_scratch, maximum_depth, visit_rate_limited, &context,
                               &context.error, GatewayEventDecodeError::invalid_data);
    if (!result.ok())
        return result;
    if ((context.fields & 7) != 7)
        return decode_failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    result = visit_object(context.meta, structural_scratch, maximum_depth, visit_rate_meta, &context,
                          &context.error, GatewayEventDecodeError::invalid_data);
    if (!result.ok())
        return result;
    if (!(context.fields & 8))
        return decode_failure(GatewayEventDecodeError::missing_required_field, context.meta.source_offset);
    *output = context.value;
    return decode_failure(GatewayEventDecodeError::none);
}

GatewayEventDecodeResult gateway_decode_member_chunk_receipt(const JsonIndexedDocument& document, JsonValue data,
                                                             u32 maximum_depth,
                                                             GatewayMemberChunkReceipt* output) noexcept {
    if (data.kind != JsonValueKind::object)
        return decode_failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    MemberChunkContext context{};
    const JsonReadResult read =
        json_visit_indexed_object(document, data, maximum_depth, visit_member_chunk_receipt, &context);
    if (!read.ok())
        return read_failure(context.error, read, GatewayEventDecodeError::invalid_data);
    if ((context.fields & 13) != 13)
        return decode_failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    if (!context.value.chunk_count || context.value.chunk_index >= context.value.chunk_count)
        return decode_failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    *output = context.value;
    return decode_failure(GatewayEventDecodeError::none);
}

GatewayEventDecodeResult gateway_decode_hello(JsonValue data, const Slice<u32>& structural_scratch,
                                              u32 maximum_depth, GatewayHello* output) noexcept {
    if (data.kind != JsonValueKind::object) {
        return decode_failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    }
    HelloContext context{};
    const auto result = visit_object(data, structural_scratch, maximum_depth, visit_hello_member, &context,
                                     &context.error, GatewayEventDecodeError::invalid_json);
    if (!result.ok()) {
        return result;
    }
    if (!context.heartbeat_seen) {
        return decode_failure(GatewayEventDecodeError::missing_heartbeat_interval, data.source_offset);
    }
    *output = context.hello;
    return decode_failure(GatewayEventDecodeError::none);
}

GatewayEventDecodeResult gateway_decode_ready(const JsonIndexedDocument& document, JsonValue data,
                                              u32 maximum_depth, GatewayReady* output) noexcept {
    if (data.kind != JsonValueKind::object) {
        return decode_failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    }
    ReadyContext context{};
    const JsonReadResult read = json_visit_indexed_object(document, data, maximum_depth, visit_ready_member, &context);
    if (!read.ok()) {
        return read_failure(context.error, read);
    }
    constexpr u32 required_fields = (1u << 5) - 1;
    if ((context.fields & required_fields) != required_fields) {
        return decode_failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    }
    ReadyUserContext user{};
    const JsonReadResult user_read =
        json_visit_indexed_object(document, context.ready.user, maximum_depth, visit_ready_user_member, &user);
    if (!user_read.ok() || user.error != GatewayEventDecodeError::none || !user.seen) {
        return decode_failure(user.error != GatewayEventDecodeError::none
                                  ? user.error
                                  : GatewayEventDecodeError::invalid_user,
                              context.ready.user.source_offset, JsonScanError::none, user_read.error);
    }
    context.ready.user_id = user.id;
    copy_value(output, context.ready);
    return decode_failure(GatewayEventDecodeError::none);
}

GatewayEventDecodeResult gateway_decode_resumed(JsonValue data, Slice<u32> structural_scratch,
                                                u32 maximum_depth) noexcept {
    if (data.kind != JsonValueKind::object) {
        return decode_failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    }
    ResumedContext context{};
    const auto result = visit_object(data, structural_scratch, maximum_depth, visit_resumed_member,
                                     &context, &context.error, GatewayEventDecodeError::invalid_json);
    return result.ok() ? decode_failure(GatewayEventDecodeError::none) : result;
}

GatewayEventDecodeResult gateway_decode_supplemental_private_channels(JsonValue data, Slice<u32> scratch,
                                                                      u32 maximum_depth,
                                                                      JsonValue* channels) noexcept {
    reset_value(channels);
    if (data.kind != JsonValueKind::object)
        return decode_failure(GatewayEventDecodeError::invalid_data);
    SupplementalPrivateContext context{};
    const auto result = visit_object(data, scratch, maximum_depth, visit_supplemental_private, &context,
                                     &context.error, GatewayEventDecodeError::invalid_json);
    if (!result.ok())
        return result;
    *channels = context.channels;
    return decode_failure(GatewayEventDecodeError::none);
}

GatewayEventDecodeResult gateway_decode_invalid_session(JsonValue data, bool* resumable) noexcept {
    if (!json_value_to_bool(data, resumable)) {
        return decode_failure(GatewayEventDecodeError::invalid_resumable, data.source_offset);
    }
    return decode_failure(GatewayEventDecodeError::none);
}

} // namespace maicord
#pragma clang attribute pop
