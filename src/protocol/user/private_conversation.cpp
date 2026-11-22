#include "maicord/protocol/user/private_conversation.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
constexpr u64 kPrivateOpenRouteId = 0x444d4f50454e31ull;
}
PrivateConversationWriteResult private_conversation_write(PrivateConversationOperation operation, u64 id,
    Slice<u8> target, Slice<u8> body) noexcept {
    if (!id) return {};
    ProtocolWriter path{target, 0}, json{body, 0};
    if (operation == PrivateConversationOperation::open) {
        if (!protocol_write(&path, "/api/v9/users/@me/channels") ||
            !protocol_write(&json, "{\"recipients\":[", snowflake_text(id), "]}")) return {};
    } else {
        if (!protocol_write(&path, "/api/v9/channels/", id, "?silent=false")) return {};
    }
    return {path.used, json.used, true};
}
RestRouteKey private_conversation_route(PrivateConversationOperation operation, u64 id) noexcept {
    return operation == PrivateConversationOperation::open ?
        RestRouteKey{kPrivateOpenRouteId, 0, RestMethod::post, RestMajorParameter::none} :
        RestRouteKey{0x444d434c4f5345ull, id, RestMethod::delete_method, RestMajorParameter::channel};
}

bool private_channel_name_valid(Utf8View name) noexcept {
    if (name.bytes > kPrivateChannelNameMaximumBytes) return false;
    const auto checked = utf8_validate(name);
    return checked.valid && checked.codepoints <= kPrivateChannelNameMaximumCodepoints;
}
// The action controller builds each request from its picker and stores (private_channel_actions.cpp).
PrivateConversationWriteResult private_channel_action_write(const PrivateChannelActionRequest& r,
    Slice<u8> target, Slice<u8> body) noexcept {
    ProtocolWriter path{target, 0}, json{body, 0};
    bool ok = false;
    const auto channel = [&]() __attribute__((noinline)) noexcept {
        return protocol_write(&path, "/api/v9/channels/", r.channel_id);
    };
    switch (r.kind) {
    case PrivateChannelActionKind::create:
        ok = protocol_write(&path, "/api/v9/users/@me/channels") &&
            protocol_write(&json, "{\"recipients\":", snowflake_array(r.recipients, r.recipient_count), '}');
        break;
    case PrivateChannelActionKind::add_recipient:
    case PrivateChannelActionKind::remove_recipient:
        ok = channel() && protocol_write(&path, "/recipients/", r.user_id);
        break;
    case PrivateChannelActionKind::rename:
        ok = channel() && protocol_write(&json, "{\"name\":", json_text({r.name, r.name_bytes}), '}');
        break;
    case PrivateChannelActionKind::leave:
        ok = channel() && protocol_write(&path, r.silent ? "?silent=true" : "?silent=false");
        break;
    case PrivateChannelActionKind::ring:
        // A null recipient list rings every member who is not in the call.
        ok = channel() && protocol_write(&path, "/call/ring") &&
            protocol_write(&json, "{\"recipients\":",
                           or_null(r.recipient_count != 0, snowflake_array(r.recipients, r.recipient_count)), '}');
        break;
    case PrivateChannelActionKind::stop_ringing:
        ok = channel() && protocol_write(&path, "/call/stop-ringing") &&
            (r.recipient_count
                 ? protocol_write(&json, "{\"recipients\":", snowflake_array(r.recipients, r.recipient_count), '}')
                 : protocol_write(&json, "{}"));
        break;
    case PrivateChannelActionKind::accept_request:
        // consent_status 2 is ACCEPTED in the message request consent enum.
        ok = channel() && protocol_write(&path, "/recipients/@me") && protocol_write(&json, "{\"consent_status\":2}");
        break;
    case PrivateChannelActionKind::ignore_request:
        ok = channel() && protocol_write(&path, "/recipients/@me");
        break;
    case PrivateChannelActionKind::acknowledge_safety:
        ok = channel() && protocol_write(&path, "/safety-warnings/ack") &&
            protocol_write(&json, "{\"warning_ids\":", snowflake_array(r.recipients, r.recipient_count), '}');
        break;
    default: break;
    }
    if (!ok) return {};
    return {path.used, json.used, true};
}
RestRouteKey private_channel_action_route(const PrivateChannelActionRequest& r) noexcept {
    switch (r.kind) {
    case PrivateChannelActionKind::create:
        return {kPrivateOpenRouteId, 0, RestMethod::post, RestMajorParameter::none};
    case PrivateChannelActionKind::add_recipient:
        return {0x444d524350ull, r.channel_id, RestMethod::put, RestMajorParameter::channel};
    case PrivateChannelActionKind::remove_recipient:
        return {0x444d524350ull, r.channel_id, RestMethod::delete_method, RestMajorParameter::channel};
    case PrivateChannelActionKind::rename:
        return {0x444d4e414d45ull, r.channel_id, RestMethod::patch, RestMajorParameter::channel};
    case PrivateChannelActionKind::leave:
        return {0x444d434c4f5345ull, r.channel_id, RestMethod::delete_method, RestMajorParameter::channel};
    case PrivateChannelActionKind::ring:
        return {0x444d52494e47ull, r.channel_id, RestMethod::post, RestMajorParameter::channel};
    case PrivateChannelActionKind::stop_ringing:
        return {0x444d53544f50ull, r.channel_id, RestMethod::post, RestMajorParameter::channel};
    case PrivateChannelActionKind::accept_request:
        return {0x444d52455141ull, r.channel_id, RestMethod::put, RestMajorParameter::channel};
    case PrivateChannelActionKind::ignore_request:
        return {0x444d52455149ull, r.channel_id, RestMethod::delete_method, RestMajorParameter::channel};
    case PrivateChannelActionKind::acknowledge_safety:
        return {0x444d534146ull, r.channel_id, RestMethod::post, RestMajorParameter::channel};
    default: return {};
    }
}
}
