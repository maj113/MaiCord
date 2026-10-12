#include "application_state.hpp"

// Client-only definitions of the Gateway message JSON decoders and the channel
// router. The account module owns the only copy (AccountModuleDecoders); the
// client decodes REST bodies only after it took them from a loaded module, so
// every caller runs while the module is resident. Without a module each entry
// fails like invalid input, which every caller already handles.
namespace maicord {
namespace {

const AccountModuleDecoders* decoders() noexcept {
    const AccountHost& host = app_detail::g_app.account;
    return host.module ? &host.api.decoders : nullptr;
}

GatewayEventDecodeResult unavailable() noexcept {
    GatewayEventDecodeResult result{};
    result.error = GatewayEventDecodeError::invalid_input;
    return result;
}

GatewayClientDispatchRouteResult route_unavailable() noexcept {
    GatewayClientDispatchRouteResult result{};
    result.error = GatewayClientDispatchRouteError::invalid_input;
    return result;
}

} // namespace

GatewayEventDecodeResult gateway_decode_message_create(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       GatewayMessageEvent* output) noexcept {
    const auto* d = decoders();
    return d ? d->message_create(data, structural_scratch, maximum_depth, content_scratch, output)
             : unavailable();
}

GatewayEventDecodeResult gateway_decode_message_create(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       Slice<u64> mention_role_scratch, u64 current_user_id,
                                                       GatewayMessageEvent* output) noexcept {
    const auto* d = decoders();
    return d ? d->message_create_mentions(data, structural_scratch, maximum_depth, content_scratch,
                                          mention_role_scratch, current_user_id, output)
             : unavailable();
}

GatewayEventDecodeResult gateway_decode_message_create(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       Slice<u64> mention_role_scratch,
                                                       Slice<GatewayMessageNamedUser> mention_user_scratch,
                                                       u64 current_user_id,
                                                       GatewayMessageEvent* output) noexcept {
    const auto* d = decoders();
    return d ? d->message_create_users(data, structural_scratch, maximum_depth, content_scratch,
                                       mention_role_scratch, mention_user_scratch, current_user_id, output)
             : unavailable();
}

GatewayEventDecodeResult gateway_decode_message_create(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> content_scratch,
    Slice<u64> mention_role_scratch, Slice<u64> member_role_scratch,
    Slice<GatewayMessageNamedUser> mention_user_scratch, const GatewayMessageRichScratch& rich_scratch,
    u64 current_user_id, GatewayMessageEvent* output) noexcept {
    const auto* d = decoders();
    return d ? d->message_create_rich(data, structural_scratch, maximum_depth, content_scratch,
                                      mention_role_scratch, member_role_scratch, mention_user_scratch,
                                      rich_scratch, current_user_id, output)
             : unavailable();
}

GatewayEventDecodeResult gateway_decode_message_snapshot_core(JsonValue data, Slice<u32> structural_scratch,
                                                              u32 maximum_depth,
                                                              Slice<char> content_scratch,
                                                              Slice<GatewayMessageNamedUser> mention_users,
                                                              GatewayMessageEvent* output) noexcept {
    const auto* d = decoders();
    return d ? d->message_snapshot_core(data, structural_scratch, maximum_depth, content_scratch, mention_users,
                                        output)
             : unavailable();
}

GatewayEventDecodeResult gateway_extract_message_snapshot(JsonValue array, Slice<u32> structural_scratch,
                                                          u32 maximum_depth, JsonValue* body,
                                                          bool* present) noexcept {
    const auto* d = decoders();
    return d ? d->message_snapshot_extract(array, structural_scratch, maximum_depth, body, present)
             : unavailable();
}

GatewayEventDecodeResult gateway_decode_message_poll(JsonValue data, const JsonIndexedDocument& document,
                                                     u32 maximum_depth, Slice<char> text_output,
                                                     u32* text_used,
                                                     const GatewayMessagePollScratch& scratch,
                                                     GatewayMessagePoll* output,
                                                     bool require_complete) noexcept {
    const auto* d = decoders();
    return d ? d->message_poll(data, document, maximum_depth, text_output, text_used, scratch, output,
                               require_complete)
             : unavailable();
}

GatewayClientDispatchRouteResult gateway_client_route_channel_create(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> storage,
    u64 self_user_id, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    const auto* d = decoders();
    return d ? d->channel_create(data, structural_scratch, maximum_depth, storage, self_user_id, event, payload)
             : route_unavailable();
}

GatewayClientDispatchRouteResult gateway_client_route_private_channel(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> storage,
    GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    const auto* d = decoders();
    return d ? d->private_channel(data, structural_scratch, maximum_depth, storage, event, payload)
             : route_unavailable();
}

} // namespace maicord
