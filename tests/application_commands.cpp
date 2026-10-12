#include "maicord/protocol/application_commands.hpp"
#include "maicord/media/asset.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/core/memory.hpp"
#include "../src/app/application_command_request.hpp"
namespace maicord {
void test_application_commands(void (*check)(bool, const char*) noexcept) noexcept {
    // The controller's fake account callback cannot observe the transport
    // flags. Exercise the same request factory as Account and the real HTTP
    // head validator, without starting a client or sending any bytes.
    constexpr auto interaction = utf8_literal("{\"type\":2,\"data\":{}}");
    constexpr auto fixture_token = utf8_literal("offline-fixture");
    constexpr auto fixture_properties = utf8_literal("e30=");
    const RestUserRequestHeaders fixture_headers{
        {reinterpret_cast<const u8*>(fixture_properties.data), fixture_properties.bytes},
        utf8_literal("en-US"), {}};
    u8 request_head[2048]{};
    const bool variants[]{false, true};
    for (bool autocomplete : variants) {
        const auto request = app_detail::application_command_interaction_request(
            {reinterpret_cast<const u8*>(interaction.data), interaction.bytes}, 1, autocomplete);
        RestRequestHead head{request.target,
            {reinterpret_cast<const u8*>(fixture_token.data), fixture_token.bytes}, request.body.count,
            request.route.method, RestAuthorization::normal_user, request.json_body, false, &fixture_headers, false};
        const auto written = write_rest_request_head(head, {request_head, sizeof(request_head)});
        constexpr auto json_header = utf8_literal("Content-Type: application/json\r\n");
        bool has_json_header = false;
        for (u32 at = 0; written.ok() && at + json_header.bytes <= written.bytes; ++at)
            if (bytes_equal(request_head + at, json_header.data, json_header.bytes)) has_json_header = true;
        check(request.route.method == RestMethod::post && request.json_body && written.ok() &&
              has_json_header && request.timeout_ms == (autocomplete ? 3000u : kRestClientDefaultTimeoutMs),
              "command execution and autocomplete construct admitted JSON interaction requests");
        head.json_body = false;
        check(write_rest_request_head(head, {request_head, sizeof(request_head)}).error ==
                  RestRequestError::invalid_input,
              "real HTTP validator rejects the former nonempty non-JSON interaction request");
    }
    constexpr auto body = utf8_literal(
        R"({"applications":[{"id":"10","name":"First","bot":{"id":"11","avatar":"abc"}},{"id":"20","name":"Second"}],"application_commands":[{"id":"100","application_id":"10","version":"7","name":"bump","description":"Bump first","type":1},{"id":"200","application_id":"20","version":"8","name":"bump","description":"Bump second","type":1},{"id":"300","application_id":"10","version":"9","name":"admin","description":"Admin","type":1,"options":[{"name":"member","type":2,"options":[{"name":"set","description":"Set member","type":1,"options":[{"name":"user","type":6,"required":true},{"name":"reason","type":3},{"name":"count","type":4},{"name":"enabled","type":5},{"name":"ratio","type":10}]}]}]}]})");
    u32 scratch[2048];
    CommandIndex index{};
    check(command_index_decode(body, {scratch, 2048}, &index), "command_index_decodes");
    if (index.command_count != 3) {
        check(false, "command_leaf_count");
        command_index_release(&index);
        return;
    }
    check(index.commands[0].application_id != index.commands[1].application_id &&
              utf8_equal(index.commands[0].name, index.commands[1].name),
          "same_command_name_retains_app_identity");
    const auto& command = index.commands[2];
    check(utf8_equal(command.name, utf8_literal("admin member set")) &&
              utf8_equal(command.group_name, utf8_literal("member")) && command.id == 300,
          "command_nested_leaf_retains_root_identity");
    u8 data[8192];
    ProtocolWriter w{{data, sizeof(data)}, 0};
    Utf8View error{};
    check(
        command_arguments(
            index, command,
            utf8_literal(R"(user:<@123> reason:"a \"quote\" and spaces" count:-2 enabled:true ratio:1.5)"),
            &w, &error),
        "command_named_arguments_typed_and_escaped");
    const auto expected = utf8_literal(
        R"([{"name":"user","type":6,"value":"123"},{"name":"reason","type":3,"value":"a \"quote\" and spaces"},{"name":"count","type":4,"value":-2},{"name":"enabled","type":5,"value":true},{"name":"ratio","type":10,"value":1.5}])");
    check(utf8_equal({reinterpret_cast<const char*>(data), w.used}, expected),
          "command_values_preserve_wire_types");
    u8 payload[16384];
    ProtocolWriter out{{payload, sizeof(payload)}, 0};
    check(command_data(command, {reinterpret_cast<const char*>(data), w.used}, &out),
          "command_payload_serializes_nested_options");
    const auto scan =
        json_scan_structurals_avx1({reinterpret_cast<const char*>(payload), out.used}, {scratch, 2048});
    const JsonIndexedDocument doc{{reinterpret_cast<const char*>(payload), out.used},
                                  {scratch, scan.count}};
    const JsonValue root{doc.input, 0, JsonValueKind::object};
    check(scan.ok() && command_member(doc, root, utf8_literal("application_command")).kind ==
                           JsonValueKind::object,
          "interaction_contains_original_versioned_command");
    const Utf8View invalid[] = {
        utf8_literal("reason:missing user"),    utf8_literal("user:123 user:456"),
        utf8_literal("user:123 count:2.2"),     utf8_literal("user:123 enabled:yes"),
        utf8_literal("user:123 unknown:hello"), utf8_literal("user:123 reason:\"unfinished")};
    for (const auto input : invalid) {
        w.used = 0;
        check(!command_arguments(index, command, input, &w, &error) && error.bytes,
              "command_invalid_arguments_rejected_before_network");
    }
    w.used = 0;
    check(command_arguments(index, index.commands[0], {}, &w, &error) && w.used == 2,
          "zero_option_command_executes_empty_options");
    const auto* retained = index.commands;
    check(!command_index_decode(utf8_literal("{broken"), {scratch, 2048}, &index) &&
              index.commands == retained,
          "bad_index_does_not_replace_loaded_commands");
    command_index_release(&index);
    const auto video = media_asset_attachment(70, MediaAttachmentFormat::mp4);
    check(media_asset_key_valid(video) && (video.flags & media_asset_flag_animated),
          "mp4_attachment_reaches_timed_decoder");
    PopupScene scene{};
    PopupItem items[16]{};
    PopupRowWidget rows[16]{};
    UiWidget widgets[36]{};
    UiLayoutNode nodes[36]{};
    check(popup_scene_initialize(&scene, {items, 16}, {rows, 16}, {widgets, 36}, {nodes, 36}),
          "command_popup_initialize");
    const PopupItem menu[] = {{utf8_literal("All"), 1, popup_item_enabled | popup_item_command_category},
                              {utf8_literal("App"), 2, popup_item_enabled | popup_item_command_category},
                              {utf8_literal("/bump"), 3, popup_item_enabled | popup_item_command_row},
                              {utf8_literal("/bump"), 4, popup_item_enabled | popup_item_command_row},
                              {utf8_literal("/a"), 5, popup_item_enabled | popup_item_command_row},
                              {utf8_literal("/b"), 6, popup_item_enabled | popup_item_command_row},
                              {utf8_literal("/c"), 7, popup_item_enabled | popup_item_command_row},
                              {utf8_literal("/d"), 8, popup_item_enabled | popup_item_command_row},
                              {utf8_literal("/e"), 9, popup_item_enabled | popup_item_command_row},
                              {utf8_literal("/f"), 10, popup_item_enabled | popup_item_command_row},
                              {utf8_literal("/g"), 11, popup_item_enabled | popup_item_command_row}};
    const auto opened = popup_scene_open(&scene, PopupKind::autocomplete, 7, {menu, 11},
                                         {20, 550, 780, 551}, {0, 0, 800, 550}, 760, true, 47.5f, 7.5f);
    check(opened.ok(), "command_popup_open_fractional");
    PopupItemLayout rail{}, row{};
    check(opened.ok() && popup_scene_resolve_item_layout(scene, 0, &rail) &&
              popup_scene_resolve_item_layout(scene, 2, &row) &&
              rail.clip_rectangle.right <= row.clip_rectangle.left &&
              row.row->rectangle.top < row.clip_rectangle.top,
          "command_rail_and_rows_clip_independently_at_fractional_scroll");
    popup_scene_release(&scene);
}
} // namespace maicord
