#include "maicord/app/gateway_client.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/client_status.hpp"
#include "maicord/protocol/json_ids.hpp"

#include "gateway_dispatch_common.hpp"

namespace maicord {
namespace {

struct PresenceContext {
    GatewayClientPresenceUpdate value;
    JsonValue user;
    GatewayEventDecodeError error;
    u8 fields;
};

bool mark(PresenceContext* context, u8 field) noexcept {
    if ((context->fields & field) != 0) {
        context->error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    context->fields |= field;
    return true;
}

bool visit_presence(void* opaque, JsonString key, JsonValue value) noexcept {
    auto* context = static_cast<PresenceContext*>(opaque);
    bool valid = true;
    if (json_string_equals_ascii(key, utf8_literal("guild_id"))) {
        if (!mark(context, 1)) return false;
        valid = value.kind == JsonValueKind::null_value ||
                json_value_to_snowflake(value, &context->value.guild_id);
    } else if (json_string_equals_ascii(key, utf8_literal("user"))) {
        if (!mark(context, 2)) return false;
        valid = value.kind == JsonValueKind::object;
        context->user = value;
    } else if (json_string_equals_ascii(key, utf8_literal("status"))) {
        if (!mark(context, 4)) return false;
        JsonString status{};
        valid = json_value_as_string(value, &status);
        if (valid)
            context->value.status = gateway_member_status(true, status);
    } else if (json_string_equals_ascii(key, utf8_literal("client_status"))) {
        if (!mark(context, 16)) return false;
        // Per-platform detail is advisory: a malformed object leaves every
        // platform offline rather than dropping the presence.
        if (!client_status_decode(value, &context->value.client_status)) context->value.client_status = 0;
    }
    if (!valid) context->error = GatewayEventDecodeError::invalid_data;
    return valid;
}

bool visit_user(void* opaque, JsonString key, JsonValue value) noexcept {
    auto* context = static_cast<PresenceContext*>(opaque);
    if (!json_string_equals_ascii(key, utf8_literal("id"))) return true;
    if (!mark(context, 8)) return false;
    if (!json_value_to_snowflake(value, &context->value.user_id)) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    return true;
}

GatewayEventDecodeResult read_object(JsonValue value, Slice<u32> scratch, u32 depth,
                                    bool user, PresenceContext* context) noexcept {
    const auto scan = json_scan_structurals_avx1(value.source, scratch);
    if (!scan.ok()) return {scan.error_offset, GatewayEventDecodeError::scan_failed, scan.error};
    const auto read = json_visit_value_object(value, {scratch.data, scan.count}, depth,
                                              user ? visit_user : visit_presence, context);
    if (!read.ok()) return {read.error_offset, context->error != GatewayEventDecodeError::none
        ? context->error : GatewayEventDecodeError::invalid_json, JsonScanError::none, read.error};
    return {};
}

} // namespace

GatewayEventDecodeResult gateway_client_decode_presence(
    JsonValue data, Slice<u32> scratch, u32 maximum_depth,
    GatewayClientPresenceUpdate* output, UserAppearance* appearance) noexcept {
    if (!output || !scratch.data || scratch.count == 0 || maximum_depth == 0 ||
        !data.source.data || data.source.bytes == 0) return {0, GatewayEventDecodeError::invalid_input};
    reset_value(output);
    PresenceContext context{};
    auto decoded = read_object(data, scratch, maximum_depth, false, &context);
    if (!decoded.ok()) return decoded;
    if ((context.fields & 6) != 6) return {0, GatewayEventDecodeError::missing_required_field};
    // The presence user object is partial: only id is required. Reuse the scanner
    // after the outer visitor is finished; no nested scratch arena or strings.
    decoded = read_object(context.user, scratch, maximum_depth, true, &context);
    if (!decoded.ok()) return decoded;
    if ((context.fields & 8) == 0) return {0, GatewayEventDecodeError::missing_required_field};
    if (appearance) {
        reset_value(appearance); appearance->user_id = context.value.user_id; appearance->guild_id = context.value.guild_id;
        const auto scan = json_scan_structurals_avx1(data.source, scratch);
        if (!scan.ok() || !activity_decode({data.source, {scratch.data, scan.count}},
            {data.source, 0, JsonValueKind::object}, appearance)) return {0, GatewayEventDecodeError::invalid_data};
    }
    *output = context.value;
    return {};
}

} // namespace maicord
