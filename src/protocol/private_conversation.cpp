#include "maicord/protocol/private_conversation.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {
constexpr u64 kPrivateOpenRouteId = 0x444d4f50454e31ull;
bool append_ids(ProtocolWriter* json, const u64* ids, u32 count) noexcept {
    if (!protocol_writer_append_byte(json, '[')) return false;
    for (u32 i = 0; i < count; ++i)
        if ((i && !protocol_writer_append_byte(json, ',')) || !protocol_writer_append_byte(json, '"') ||
            !protocol_writer_append_u64(json, ids[i]) || !protocol_writer_append_byte(json, '"')) return false;
    return protocol_writer_append_byte(json, ']');
}
bool append_channel(ProtocolWriter* path, u64 id) noexcept {
    return protocol_writer_append_literal(path, "/api/v9/channels/") && protocol_writer_append_u64(path, id);
}
}
PrivateConversationWriteResult private_conversation_write(PrivateConversationOperation operation, u64 id,
    Slice<u8> target, Slice<u8> body) noexcept {
    if (!id || !target.data) return {};
    ProtocolWriter path{target, 0}, json{body, 0};
    if (operation == PrivateConversationOperation::open) {
        if (!protocol_writer_append_literal(&path, "/api/v9/users/@me/channels") ||
            !protocol_writer_append_literal(&json, "{\"recipients\":[\"") || !protocol_writer_append_u64(&json, id) ||
            !protocol_writer_append_literal(&json, "\"]}")) return {};
    } else if (operation == PrivateConversationOperation::close) {
        if (!protocol_writer_append_literal(&path, "/api/v9/channels/") || !protocol_writer_append_u64(&path, id) ||
            !protocol_writer_append_literal(&path, "?silent=false")) return {};
    } else return {};
    return {path.used, json.used, true};
}
RestRouteKey private_conversation_route(PrivateConversationOperation operation, u64 id) noexcept {
    return operation == PrivateConversationOperation::open ?
        RestRouteKey{kPrivateOpenRouteId, 0, RestMethod::post, RestMajorParameter::none} :
        RestRouteKey{0x444d434c4f5345ull, id, RestMethod::delete_method, RestMajorParameter::channel};
}

bool private_channel_action_valid(const PrivateChannelActionRequest& r) noexcept {
    if (r.recipient_count > kPrivateChannelRecipientMaximum || r.name_bytes > kPrivateChannelNameMaximumBytes) return false;
    for (u32 i = 0; i < r.recipient_count; ++i) {
        if (!r.recipients[i]) return false;
        for (u32 j = 0; j < i; ++j) if (r.recipients[j] == r.recipients[i]) return false;
    }
    switch (r.kind) {
    case PrivateChannelActionKind::create: return r.recipient_count != 0;
    case PrivateChannelActionKind::add_recipient:
    case PrivateChannelActionKind::remove_recipient: return r.channel_id && r.user_id;
    case PrivateChannelActionKind::rename: {
        const auto checked = utf8_validate({r.name, r.name_bytes});
        return r.channel_id && checked.valid && checked.codepoints <= kPrivateChannelNameMaximumCodepoints;
    }
    case PrivateChannelActionKind::leave:
    case PrivateChannelActionKind::ring:
    case PrivateChannelActionKind::stop_ringing:
    case PrivateChannelActionKind::accept_request:
    case PrivateChannelActionKind::ignore_request: return r.channel_id != 0;
    case PrivateChannelActionKind::ignore_requests: return r.recipient_count != 0;
    case PrivateChannelActionKind::acknowledge_safety: return r.channel_id && r.recipient_count;
    default: return false;
    }
}
PrivateConversationWriteResult private_channel_action_write(const PrivateChannelActionRequest& r,
    Slice<u8> target, Slice<u8> body) noexcept {
    if (!target.data || !private_channel_action_valid(r)) return {};
    ProtocolWriter path{target, 0}, json{body, 0};
    bool ok = false;
    switch (r.kind) {
    case PrivateChannelActionKind::create:
        ok = protocol_writer_append_literal(&path, "/api/v9/users/@me/channels") &&
            protocol_writer_append_literal(&json, "{\"recipients\":") && append_ids(&json, r.recipients, r.recipient_count) &&
            protocol_writer_append_byte(&json, '}');
        break;
    case PrivateChannelActionKind::add_recipient:
    case PrivateChannelActionKind::remove_recipient:
        ok = append_channel(&path, r.channel_id) && protocol_writer_append_literal(&path, "/recipients/") &&
            protocol_writer_append_u64(&path, r.user_id);
        break;
    case PrivateChannelActionKind::rename:
        ok = append_channel(&path, r.channel_id) && protocol_writer_append_literal(&json, "{\"name\":") &&
            protocol_writer_append_json_string(&json, {r.name, r.name_bytes}) && protocol_writer_append_byte(&json, '}');
        break;
    case PrivateChannelActionKind::leave:
        ok = append_channel(&path, r.channel_id) && (r.silent ? protocol_writer_append_literal(&path, "?silent=true")
                                                              : protocol_writer_append_literal(&path, "?silent=false"));
        break;
    case PrivateChannelActionKind::ring:
        // A null recipient list rings every member who is not in the call.
        ok = append_channel(&path, r.channel_id) && protocol_writer_append_literal(&path, "/call/ring") &&
            protocol_writer_append_literal(&json, "{\"recipients\":") &&
            (r.recipient_count ? append_ids(&json, r.recipients, r.recipient_count)
                               : protocol_writer_append_literal(&json, "null")) &&
            protocol_writer_append_byte(&json, '}');
        break;
    case PrivateChannelActionKind::stop_ringing:
        ok = append_channel(&path, r.channel_id) && protocol_writer_append_literal(&path, "/call/stop-ringing") &&
            (r.recipient_count ? protocol_writer_append_literal(&json, "{\"recipients\":") &&
                                     append_ids(&json, r.recipients, r.recipient_count) &&
                                     protocol_writer_append_byte(&json, '}')
                               : protocol_writer_append_literal(&json, "{}"));
        break;
    case PrivateChannelActionKind::accept_request:
        // consent_status 2 is ACCEPTED in the message request consent enum.
        ok = append_channel(&path, r.channel_id) && protocol_writer_append_literal(&path, "/recipients/@me") &&
            protocol_writer_append_literal(&json, "{\"consent_status\":2}");
        break;
    case PrivateChannelActionKind::ignore_request:
        ok = append_channel(&path, r.channel_id) && protocol_writer_append_literal(&path, "/recipients/@me");
        break;
    case PrivateChannelActionKind::ignore_requests:
        ok = protocol_writer_append_literal(&path, "/api/v9/channels/recipients/@me/batch-reject") &&
            protocol_writer_append_literal(&json, "{\"channel_ids\":") &&
            append_ids(&json, r.recipients, r.recipient_count) && protocol_writer_append_byte(&json, '}');
        break;
    case PrivateChannelActionKind::acknowledge_safety:
        ok = append_channel(&path, r.channel_id) && protocol_writer_append_literal(&path, "/safety-warnings/ack") &&
            protocol_writer_append_literal(&json, "{\"warning_ids\":") &&
            append_ids(&json, r.recipients, r.recipient_count) && protocol_writer_append_byte(&json, '}');
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
    case PrivateChannelActionKind::ignore_requests:
        return {0x444d52454242ull, 0, RestMethod::put, RestMajorParameter::none};
    case PrivateChannelActionKind::acknowledge_safety:
        return {0x444d534146ull, r.channel_id, RestMethod::post, RestMajorParameter::channel};
    default: return {};
    }
}
}
