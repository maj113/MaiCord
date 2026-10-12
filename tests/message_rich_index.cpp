#include "maicord/protocol/gateway_message_events.hpp"

namespace maicord {
void test_message_rich_index(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr Utf8View body = utf8_literal(R"({"ignored":{"deep":[{"value":"before"}]},"message_reference":{"message_id":"10","channel_id":"20"},"referenced_message":{"id":"10","channel_id":"20","content":"old text","author":{"id":"30","username":"old author"}},"reactions":[{"count":2,"me":false,"emoji":{"id":null,"name":"ok"}}],"embeds":[{"title":"title","fields":[{"name":"first","value":"one","inline":true},{"name":"second","value":"two"}],"thumbnail":{"url":"https://example.test/icon.png","width":32,"height":32}}],"components":[{"type":1,"components":[{"type":2,"style":1,"label":"Go","custom_id":"go"}]}],"attachments":[]})");
    u32 original[512]{}, scratch[512]{};
    char text[2048]{};
    GatewayMessageReaction reactions[1]{};
    GatewayMessageEmbed embeds[1]{};
    GatewayMessageEmbedField fields[2]{};
    GatewayMessageComponent components[2]{};
    const GatewayMessageRichScratch storage{{reactions, 1}, {}, {embeds, 1}, {fields, 2}, {}, {components, 2}};
    const auto scanned = json_scan_structurals_avx1(body, {original, 512});
    u32 used = 0;
    GatewayMessageEvent message{};
    auto result = gateway_decode_message_rich_fields({body, 1234, JsonValueKind::object},
        {scratch, 512}, 32, {text, sizeof(text)}, &used, storage, &message);
    check(scanned.ok() && result.ok(), "rich_index_nested_decode");
    check(scanned.ok() && bytes_equal(original, scratch, scanned.count * sizeof(u32)),
          "rich_index_children_preserve_full_document_index");
    check(message.reference.state == GatewayMessageReferenceState::available &&
          message.reference.author_id == 30 && message.reactions.count == 1 &&
          reactions[0].count == 2 && message.embeds.count == 1 && embeds[0].field_count == 2 &&
          fields[0].inline_field && fields[1].value.bytes == 3 &&
          embeds[0].thumbnail.width == 32 && message.components.count == 2 &&
          components[1].parent == 0 && components[1].custom_id.bytes == 2,
          "rich_index_nested_siblings_and_owned_output");

    constexpr Utf8View empty = utf8_literal("{\"reactions\":[ \n\t ],\"attachments\":[],\"embeds\":[ ],\"components\":[\r\n]}");
    used = 0;
    result = gateway_decode_message_rich_fields({empty, 0, JsonValueKind::object},
        {scratch, 512}, 32, {text, sizeof(text)}, &used, storage, &message);
    check(result.ok() && message.reactions.count == 0 && message.attachments.count == 0 &&
          message.embeds.count == 0 && message.components.count == 0,
          "rich_index_empty_arrays_with_json_whitespace");

    constexpr Utf8View wrong = utf8_literal(R"({"embeds":[{"title":"valid","fields":[{"name":"field","value":7}]}]})");
    used = 0;
    result = gateway_decode_message_rich_fields({wrong, 0, JsonValueKind::object},
        {scratch, 512}, 32, {text, sizeof(text)}, &used, storage, &message);
    check(!result.ok() && result.read_error == JsonReadError::visitor_rejected &&
          bytes_equal(result.schema_field, "value", 6), "rich_index_nested_schema_diagnostic");

    constexpr Utf8View malformed = utf8_literal(R"({"embeds":[ ],"components":[,]})");
    used = 0;
    result = gateway_decode_message_rich_fields({malformed, 0, JsonValueKind::object},
        {scratch, 512}, 32, {text, sizeof(text)}, &used, storage, &message);
    check(!result.ok() && result.error == GatewayEventDecodeError::invalid_json,
          "rich_index_empty_fast_path_does_not_bypass_root_validation");
    used = 0;
    result = gateway_decode_message_rich_fields({body, 0, JsonValueKind::object},
        {scratch, 512}, 2, {text, sizeof(text)}, &used, storage, &message);
    check(!result.ok() && result.read_error == JsonReadError::depth_limit,
          "rich_index_preserves_document_depth_limit");
}
} // namespace maicord
