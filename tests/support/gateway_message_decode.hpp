#pragma once
// Message decodes of a value that is a JSON document of its own, for fixtures: the value is indexed here, then
// decoded by the client's decoders of an indexed page or frame.
#include "maicord/protocol/gateway/message_events.hpp"
#include "maicord/core/json_scan.hpp"

namespace maicord {
namespace gateway_message_decode_test {
// A failure leaves `output` reset.
inline GatewayEventDecodeResult index(JsonValue data, Slice<u32> structural_scratch, JsonIndexedDocument* document,
                                      GatewayMessageEvent* output) noexcept {
    reset_value(output);
    if (data.kind != JsonValueKind::object)
        return {data.source_offset, GatewayEventDecodeError::invalid_data};
    const JsonScanResult scan = json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok())
        return {scan.error_offset, GatewayEventDecodeError::scan_failed, scan.error};
    *document = {data.source, {structural_scratch.data, scan.count}};
    return {};
}
} // namespace gateway_message_decode_test

inline GatewayEventDecodeResult gateway_decode_message_create(JsonValue data, Slice<u32> structural_scratch,
                                                              u32 maximum_depth, Slice<char> content_scratch,
                                                              GatewayMessageEvent* output) noexcept {
    JsonIndexedDocument document;
    const auto indexed = gateway_message_decode_test::index(data, structural_scratch, &document, output);
    return indexed.ok() ? gateway_decode_message_create(document, data, maximum_depth, content_scratch, output)
                        : indexed;
}

inline GatewayEventDecodeResult gateway_decode_message_create(JsonValue data, Slice<u32> structural_scratch,
                                                              u32 maximum_depth, Slice<char> content_scratch,
                                                              Slice<u64> mention_role_scratch,
                                                              Slice<GatewayMessageNamedUser> mention_user_scratch,
                                                              u64 current_user_id,
                                                              GatewayMessageEvent* output) noexcept {
    JsonIndexedDocument document;
    const auto indexed = gateway_message_decode_test::index(data, structural_scratch, &document, output);
    return indexed.ok() ? gateway_decode_message_create(document, data, maximum_depth, content_scratch,
                                                        mention_role_scratch, mention_user_scratch, current_user_id,
                                                        output)
                        : indexed;
}

inline GatewayEventDecodeResult gateway_decode_message_update(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> content_scratch,
    Slice<u64> mention_role_scratch, Slice<u64> member_role_scratch,
    Slice<GatewayMessageNamedUser> mention_user_scratch, const GatewayMessageRichScratch& rich_scratch,
    u64 current_user_id, GatewayMessageEvent* output) noexcept {
    JsonIndexedDocument document;
    const auto indexed = gateway_message_decode_test::index(data, structural_scratch, &document, output);
    return indexed.ok() ? gateway_decode_message_update(document, data, maximum_depth, content_scratch,
                                                        mention_role_scratch, member_role_scratch,
                                                        mention_user_scratch, rich_scratch, current_user_id, output)
                        : indexed;
}

// The update decode the Gateway client runs, without mention users or rich storage; the role views, which point
// into this frame, are cleared.
inline GatewayEventDecodeResult gateway_decode_message_update(JsonValue data, Slice<u32> structural_scratch,
                                                              u32 maximum_depth, Slice<char> content_scratch,
                                                              GatewayMessageEvent* output) noexcept {
    u64 mention_roles[kGatewayMessageMentionRoleMaximumIds];
    u64 member_roles[kGatewayMessageMemberRoleMaximumIds];
    const auto result = gateway_decode_message_update(data, structural_scratch, maximum_depth, content_scratch,
                                                      {mention_roles, kGatewayMessageMentionRoleMaximumIds},
                                                      {member_roles, kGatewayMessageMemberRoleMaximumIds}, {},
                                                      GatewayMessageRichScratch{}, 0, output);
    if (result.ok()) {
        output->mention_roles = {};
        output->member_roles = {};
    }
    return result;
}

inline GatewayEventDecodeResult gateway_decode_message_rich_fields(JsonValue data, Slice<u32> structural_scratch,
                                                                   u32 maximum_depth, Slice<char> text_output,
                                                                   u32* text_used,
                                                                   const GatewayMessageRichScratch& scratch,
                                                                   GatewayMessageEvent* output) noexcept {
    if (!data.source.data || data.source.bytes == 0 || !structural_scratch.data || structural_scratch.count == 0)
        return {0, GatewayEventDecodeError::invalid_input};
    const JsonScanResult scan = json_scan_structurals_avx1(data.source, structural_scratch);
    if (!scan.ok())
        return {scan.error_offset, GatewayEventDecodeError::scan_failed, scan.error};
    return gateway_decode_message_rich_fields({data.source, {structural_scratch.data, scan.count}}, data,
                                              maximum_depth, text_output, text_used, scratch, output);
}
} // namespace maicord
