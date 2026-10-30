#include "maicord/app/gateway_client.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/client_status.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "../protocol/json_field_lists.hpp"

#include "gateway_dispatch_common.hpp"
#include "maicord/model/user_profile.hpp"

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
    switch (protocol_detail::member_index<"guild_id", "user", "status", "client_status">(key)) {
    case 0:
        if (!mark(context, 1)) return false;
        valid = value.kind == JsonValueKind::null_value ||
                json_value_to_snowflake(value, &context->value.guild_id);
        break;
    case 1:
        if (!mark(context, 2)) return false;
        valid = value.kind == JsonValueKind::object;
        context->user = value;
        break;
    case 2: {
        if (!mark(context, 4)) return false;
        JsonString status{};
        valid = json_value_as_string(value, &status);
        if (valid)
            context->value.status = gateway_member_status(true, status);
        break;
    }
    case 3:
        if (!mark(context, 16)) return false;
        // Per-platform detail is advisory: a malformed object leaves every
        // platform offline rather than dropping the presence.
        if (!client_status_decode(value, &context->value.client_status)) context->value.client_status = 0;
        break;
    default: break;
    }
    if (!valid) context->error = GatewayEventDecodeError::invalid_data;
    return valid;
}

bool visit_user(void* opaque, JsonString key, JsonValue value) noexcept {
    auto* context = static_cast<PresenceContext*>(opaque);
    if (!json_name_is(key, utf8_literal("id"))) return true;
    if (!mark(context, 8)) return false;
    if (!json_value_to_snowflake(value, &context->value.user_id)) {
        context->error = GatewayEventDecodeError::invalid_data;
        return false;
    }
    return true;
}

GatewayEventDecodeResult read_object(const JsonIndexedDocument& document, JsonValue value, u32 depth,
                                    bool user, PresenceContext* context) noexcept {
    const auto read = json_visit_indexed_object(document, value, depth, user ? visit_user : visit_presence, context);
    if (!read.ok()) return {read.error_offset, context->error != GatewayEventDecodeError::none
        ? context->error : GatewayEventDecodeError::invalid_json, JsonScanError::none, read.error};
    return {};
}

} // namespace

GatewayEventDecodeResult gateway_client_decode_presence(
    const JsonIndexedDocument& document, JsonValue data, u32 maximum_depth,
    GatewayClientPresenceUpdate* output, UserAppearance* appearance) noexcept {
    if (!output || maximum_depth == 0 || !data.source.data || data.source.bytes == 0)
        return {0, GatewayEventDecodeError::invalid_input};
    reset_value(output);
    PresenceContext context{};
    auto decoded = read_object(document, data, maximum_depth, false, &context);
    if (!decoded.ok()) return decoded;
    if ((context.fields & 6) != 6) return {0, GatewayEventDecodeError::missing_required_field};
    // The presence user object is partial: only id is required.
    decoded = read_object(document, context.user, maximum_depth, true, &context);
    if (!decoded.ok()) return decoded;
    if ((context.fields & 8) == 0) return {0, GatewayEventDecodeError::missing_required_field};
    if (appearance) {
        reset_value(appearance); appearance->user_id = context.value.user_id; appearance->guild_id = context.value.guild_id;
        if (!activity_decode(document, data, appearance)) return {0, GatewayEventDecodeError::invalid_data};
    }
    *output = context.value;
    return {};
}

} // namespace maicord
