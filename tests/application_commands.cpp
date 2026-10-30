#include "maicord/protocol/application_commands.hpp"
#include "maicord/media/asset.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/ui/theme.hpp"
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
    // The built-in /nick and /thread requests: the client's endpoints and
    // methods, JSON bodies, and their server/channel rate-limit majors.
    {
        constexpr auto nick_body = utf8_literal(R"({"nick":"Maj"})");
        constexpr auto thread_body =
            utf8_literal(R"({"name":"Plans","type":11,"auto_archive_duration":4320,"location":"Slash Command"})");
        u8 target[96];
        const auto nick = app_detail::builtin_command_request(
            app_detail::kBuiltinCommandNick, 55, 9, {reinterpret_cast<const u8*>(nick_body.data), nick_body.bytes}, 3,
            {target, sizeof(target)});
        RestRequestHead head{nick.target,
            {reinterpret_cast<const u8*>(fixture_token.data), fixture_token.bytes}, nick.body.count,
            nick.route.method, RestAuthorization::normal_user, nick.json_body, false, &fixture_headers, false};
        check(utf8_equal({reinterpret_cast<const char*>(nick.target.data), nick.target.count},
                         utf8_literal("/api/v9/guilds/55/members/@me/nick")) &&
                  nick.route.method == RestMethod::patch && nick.route.major == RestMajorParameter::guild &&
                  nick.route.major_key == 55 && nick.json_body && rest_route_valid(nick.route) &&
                  write_rest_request_head(head, {request_head, sizeof(request_head)}).ok(),
              "built-in /nick patches the server nickname endpoint with a JSON body");
        u8 thread_target[96];
        const auto thread = app_detail::builtin_command_request(
            app_detail::kBuiltinCommandThread, 55, 9,
            {reinterpret_cast<const u8*>(thread_body.data), thread_body.bytes}, 4, {thread_target, sizeof(thread_target)});
        head.target = thread.target;
        head.content_length = thread.body.count;
        head.method = thread.route.method;
        check(utf8_equal({reinterpret_cast<const char*>(thread.target.data), thread.target.count},
                         utf8_literal("/api/v9/channels/9/threads")) &&
                  thread.route.method == RestMethod::post && thread.route.major == RestMajorParameter::channel &&
                  thread.route.major_key == 9 && thread.route.route_id == kThreadCreationRouteId && thread.json_body &&
                  write_rest_request_head(head, {request_head, sizeof(request_head)}).ok(),
              "built-in /thread posts to the channel's threads with the thread creation bucket");
        check(!app_detail::builtin_command_request(app_detail::kBuiltinCommandNick, 0, 9, {}, 5, {target, sizeof(target)})
                   .target.count &&
                  !app_detail::builtin_command_request(2, 55, 9, {}, 5, {target, sizeof(target)}).target.count,
              "built-in requests need their server or channel and a built-in operation");
    }
    // The request body: key order, guild only in servers, the
    // analytics location on executions and never on autocomplete.
    u8 written_body[512];
    struct BodyCase {
        bool autocomplete;
        u64 guild;
        Utf8View expected;
    };
    const BodyCase body_cases[]{
        {false, 55,
         utf8_literal(R"({"type":2,"application_id":"10","guild_id":"55","channel_id":"9","session_id":"s\"1","data":{"id":"1"},"nonce":"77","analytics_location":"slash_ui"})")},
        {false, 0,
         utf8_literal(R"({"type":2,"application_id":"10","channel_id":"9","session_id":"s\"1","data":{"id":"1"},"nonce":"77","analytics_location":"slash_ui"})")},
        {true, 55,
         utf8_literal(R"({"type":4,"application_id":"10","guild_id":"55","channel_id":"9","session_id":"s\"1","data":{"id":"1"},"nonce":"77"})")},
    };
    for (const auto& test : body_cases) {
        ProtocolWriter writer{{written_body, sizeof(written_body)}, 0};
        check(app_detail::application_command_interaction_body(test.autocomplete, 10, test.guild, 9,
                                                               utf8_literal("s\"1"), 77,
                                                               utf8_literal(R"({"id":"1"})"), &writer) &&
                  utf8_equal({reinterpret_cast<const char*>(written_body), writer.used}, test.expected),
              "command interaction bodies match the official client's fields and order");
    }
    {
        u32 failure_scratch[256];
        char failure_text[128];
        CommandInteractionFailure failure{};
        check(command_interaction_failure(
                  utf8_literal(R"({"code":50035,"errors":{"data":{"options":{"0":{"_errors":[{"code":"INTERACTION_APPLICATION_COMMAND_INVALID","message":"Unknown command"}]}}}},"message":"Invalid Form Body"})"),
                  {failure_scratch, 256}, {failure_text, sizeof(failure_text)}, &failure) &&
                  failure.code == 50035 && failure.stale_index &&
                  utf8_equal(failure.message, utf8_literal("Unknown command")),
              "an invalid command form error shows its nested message and refetches the index");
        check(command_interaction_failure(utf8_literal(R"({"code":10005,"message":"Unknown Integration"})"),
                                          {failure_scratch, 256}, {failure_text, sizeof(failure_text)},
                                          &failure) &&
                  failure.stale_index && utf8_equal(failure.message, utf8_literal("Unknown Integration")),
              "an unknown integration refetches the index and shows the API message");
        check(command_interaction_failure(utf8_literal(R"({"code":50001,"message":"Missing Access"})"),
                                          {failure_scratch, 256}, {failure_text, sizeof(failure_text)},
                                          &failure) &&
                  !failure.stale_index && utf8_equal(failure.message, utf8_literal("Missing Access")),
              "other refusals keep the index and show the API message");
        check(command_interaction_failure(
                  utf8_literal(R"({"code":50035,"errors":{},"message":"Invalid Form Body"})"),
                  {failure_scratch, 256}, {failure_text, sizeof(failure_text)}, &failure) &&
                  !failure.message.bytes && !failure.stale_index,
              "a form error without entries falls back to the generic failure text");
        check(!command_interaction_failure(utf8_literal("oops"), {failure_scratch, 256},
                                           {failure_text, sizeof(failure_text)}, &failure),
              "a non-JSON refusal has no API message");
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
            utf8_literal(R"(user:<@123> reason:a "quote" and spaces count:-2 enabled:true ratio:1.5)"),
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
        utf8_literal("user:123 unknown:hello"), utf8_literal("user:123 ratio:")};
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
    {
        constexpr auto chooser = utf8_literal(
            R"({"applications":[{"id":"10","name":"First","bot":{"id":"11","username":"first_bot","global_name":"First Bot"}}],"application_commands":[{"id":"400","application_id":"10","version":"1","name":"size","type":1,"nsfw":true,"options":[{"name":"size","type":3,"choices":[{"name":"Extra large","value":"xl"}]},{"name":"count","type":4,"choices":[{"name":"Two","value":2}]},{"name":"flag","type":5}]}]})");
        CommandIndex choices{};
        check(command_index_decode(chooser, {scratch, 2048}, &choices) && choices.command_count == 1 &&
                  choices.commands[0].nsfw && choices.application_count == 1 &&
                  utf8_equal(choices.applications[0].bot_name, utf8_literal("First Bot")),
              "command index keeps the nsfw flag and the bot's display name");
        w.used = 0;
        check(choices.command_count == 1 &&
                  command_arguments(choices, choices.commands[0],
                                    utf8_literal(R"(size:Extra large count:Two flag:True)"), &w, &error) &&
                  utf8_equal({reinterpret_cast<const char*>(data), w.used},
                             utf8_literal(R"([{"name":"size","type":3,"value":"xl"},{"name":"count","type":4,"value":2},{"name":"flag","type":5,"value":true}])")),
              "choice names send their values and booleans accept the official True/False labels");
        w.used = 0;
        check(choices.command_count == 1 &&
                  !command_arguments(choices, choices.commands[0], utf8_literal("size:custom count:3"), &w,
                                     &error) &&
                  utf8_equal(error, utf8_literal("Not a valid choice.")),
              "text that names no choice is refused like the client");
        command_index_release(&choices);
    }
    {
        constexpr auto uploader = utf8_literal(
            R"({"applications":[{"id":"10","name":"First"}],"application_commands":[{"id":"500","application_id":"10","version":"3","guild_id":"55","name":"send","type":1,"options":[{"name":"main","type":11,"required":true},{"name":"note","type":3},{"name":"extra","type":11}]}]})");
        CommandIndex uploads{};
        const Utf8View files[]{utf8_literal("a.png"), utf8_literal("b.png")};
        CommandAttachmentPlan plan{};
        w.used = 0;
        const bool decoded = command_index_decode(uploader, {scratch, 2048}, &uploads) && uploads.command_count == 1;
        check(decoded && command_arguments(uploads, uploads.commands[0], utf8_literal("extra:b.png note:hi"), &w,
                                           &error, {}, {}, {files, 2}, &plan) &&
                  plan.count == 2 && plan.file[0] == 0 && plan.file[1] == 1 &&
                  utf8_equal({reinterpret_cast<const char*>(data), w.used},
                             utf8_literal(R"([{"name":"main","type":11,"value":0},{"name":"note","type":3,"value":"hi"},{"name":"extra","type":11,"value":1}])")),
              "attachment options take the named upload, and a required one the next unused upload");
        w.used = 0;
        check(decoded && !command_arguments(uploads, uploads.commands[0], {}, &w, &error, {}, {}, {}, &plan) &&
                  utf8_equal(error, utf8_literal("Please attach a file")),
              "a required attachment option without an upload asks for a file");
        u8 sent[4096];
        ProtocolWriter sent_writer{{sent, sizeof(sent)}, 0};
        constexpr auto head = utf8_literal(
            R"({"version":"3","id":"500","guild_id":"55","name":"send","type":1,"options":[],"application_command":{)");
        constexpr auto tail = utf8_literal(R"(},"attachments":[]})");
        const char* sent_text = reinterpret_cast<const char*>(sent);
        check(decoded && command_data(uploads.commands[0], utf8_literal("[]"), &sent_writer, utf8_literal("[]")) &&
                  sent_writer.used > head.bytes + tail.bytes && utf8_equal({sent_text, head.bytes}, head) &&
                  utf8_equal({sent_text + sent_writer.used - tail.bytes, tail.bytes}, tail),
              "command data follows the client's key order and ends with its attachments");
        command_index_release(&uploads);
    }
    {
        constexpr u64 day = 24ull * 60 * 60 * 1000, start = 1'700'000'000'000ull;
        constexpr auto key = utf8_literal("42\0sub:9");
        CommandFrecencyEntry table[2]{};
        u32 count = 0;
        command_frecency_track({table, 2}, &count, key, start);
        command_frecency_track({table, 2}, &count, key, start + 1);
        count = command_frecency_compute({table, 2}, count, start + 20 * day);
        check(count == 1 && table[0].total_uses == 2 && table[0].recent_count == 2 && table[0].score == 100 &&
                  table[0].frecency == 1,
              "command frecency weighs uses by age like the client (20 days: 50% each)");
        u8 proto[1024], json[2048];
        const u32 bytes = command_frecency_write_patch({table, count}, 7, {proto, sizeof(proto)}, {json, sizeof(json)});
        const Utf8View body{reinterpret_cast<const char*>(json), bytes};
        CommandFrecencyEntry decoded[2]{};
        u32 decoded_count = 0, version = 0;
        static u8 extracted[65536];
        check(bytes && command_frecency_patch_valid(body) &&
                  command_frecency_decode_response(body, {extracted, sizeof(extracted)}, {decoded, 2},
                                                   &decoded_count, &version) &&
                  decoded_count == 1 && decoded[0].key_bytes == key.bytes &&
                  bytes_equal(decoded[0].key, key.data, key.bytes) && decoded[0].total_uses == 2 &&
                  decoded[0].recent_count == 2 && decoded[0].recent_uses[1] == start + 1,
              "command frecency round-trips application_command_frecency with subcommand keys");
        check(!command_frecency_patch_valid(utf8_literal(R"({"settings":"KgA="})")) &&
                  !command_frecency_patch_valid(utf8_literal(R"({"settings":"OgA=","extra":1})")),
              "a command frecency save may carry only application_command_frecency");
    }
    {
        // Unsaved uses survive a restart in a small checked record.
        static CommandFrecencyUse uses[kCommandFrecencyPendingUses], read[kCommandFrecencyPendingUses];
        uses[0] = {1700000000123ull, 2, {'-', '1'}};
        uses[1] = {1700000000456ull, 3, {'1', '0', '1'}};
        u8 record[kCommandFrecencyPendingFileBytes];
        const u32 bytes = command_frecency_pending_encode({uses, 2}, {record, sizeof(record)});
        u32 count = 0;
        const bool decoded = bytes && command_frecency_pending_decode({record, bytes}, {read, 16}, &count);
        check(decoded && count == 2 && read[0].at == uses[0].at && read[1].key_bytes == 3 &&
                  bytes_equal(read[1].key, "101", 3),
              "pending command uses round-trip through their record");
        record[7] ^= 1;
        u32 untouched = 9;
        check(!command_frecency_pending_decode({record, bytes}, {read, 16}, &untouched) && untouched == 9 &&
                  !command_frecency_pending_encode({uses, kCommandFrecencyPendingUses + 1}, {record, sizeof(record)}),
              "a damaged pending-use record is ignored and an oversized list is not written");
    }
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
    {
        // The discovery browser: a 420-DIP panel, 32-DIP rail icons
        // from 16 DIPs down 40 apart (8 more before Built-in after an app),
        // 32-DIP section headers and 56-DIP rows inset 8, sections 16 apart.
        PopupItem picker[] = {
            {utf8_literal("Frequently Used"), 1, popup_item_enabled | popup_item_command_category},
            {utf8_literal("App"), 2, popup_item_enabled | popup_item_command_category | popup_item_checked},
            {utf8_literal("Built-in"), 3,
             popup_item_enabled | popup_item_command_category | popup_item_separator_before},
            {utf8_literal("Frequently Used"), 10, popup_item_command_section},
            {utf8_literal("/ban"), 11, popup_item_enabled | popup_item_command_row},
            {utf8_literal("App"), 12, popup_item_command_section},
            {utf8_literal("/wordle"), 13, popup_item_enabled | popup_item_command_row},
            {utf8_literal("Built-in"), 14, popup_item_command_section},
            {utf8_literal("/gif"), 15, popup_item_enabled | popup_item_command_row}};
        picker[0].presentation = ui_menu_row_presentation(UiIcon::recent);
        picker[2].presentation = ui_menu_row_presentation(UiIcon::command);
        picker[3].presentation = picker[0].presentation;
        picker[7].presentation = picker[2].presentation;
        picker[4].detail = utf8_literal("Ban user\x1F" "Built-in\x1Fuser\x1E+1 optional");
        picker[6].detail = utf8_literal("Play wordle\x1F" "App\x1F");
        picker[8].detail = utf8_literal("Search Animated GIFs on the Web\x1F" "Built-in\x1Fquery");
        const UiRect anchor{20, 540, 780, 590}, viewport{0, 0, 800, 600};
        const auto opened = popup_scene_open(&scene, PopupKind::autocomplete, 7, {picker, 9}, anchor, viewport,
                                             760, true, 0.f, 0.f);
        PopupItemLayout layout[9]{};
        bool resolved = opened.ok();
        for (u32 n = 0; resolved && n < 9; ++n)
            resolved = popup_scene_resolve_item_layout(scene, n, layout + n);
        const UiRect panel = scene.rectangle;
        const auto at = [&](u32 n) noexcept { return layout[n].row->rectangle; };
        check(resolved && ui_rect_height(panel) == kCommandPickerHeight && scene.selected_index == ~0u,
              "the picker opens at the client's fixed height without a selection");
        check(resolved && at(0).left == panel.left + 8 && at(0).top == panel.top + 16 && ui_rect_width(at(0)) == 32 &&
                  ui_rect_height(at(0)) == 32 && at(1).top == panel.top + 56 && at(2).top == panel.top + 104,
              "rail icons sit 40 DIPs apart with the divider's 8 DIPs before Built-in");
        check(resolved && at(3).left == panel.left + 56 && at(3).top == panel.top + 8 && ui_rect_height(at(3)) == 32 &&
                  at(4).top == at(3).bottom && ui_rect_height(at(4)) == 56 && at(4).right == panel.right - 8 &&
                  at(5).top == at(4).bottom + 16 && at(6).top == at(5).bottom && at(7).top == at(6).bottom + 16,
              "sections stack header and rows inset 8 DIPs with 16 DIPs between sections");
        UiTextCommand commands[16]{};
        UiTextCommandStream stream{};
        UiTextOutput output{&stream, nullptr};
        bool texts = ui_text_command_stream_initialize(&stream, {commands, 16}) &&
                     popup_text_presentation_build_output(scene, kDefaultTheme, &output);
        bool rail_text = false, summary = false, header_icon_gap = false, row_text_inset = false;
        for (u32 n = 0; texts && n < stream.count; ++n) {
            const auto& command = commands[n];
            rail_text = rail_text ||
                        (utf8_equal(command.text, utf8_literal("App")) && command.rectangle.right <= at(3).left);
            summary = summary || utf8_equal(command.text, utf8_literal("+1 optional"));
            header_icon_gap = header_icon_gap || (utf8_equal(command.text, utf8_literal("Built-in")) &&
                                                  command.rectangle.left == at(7).left + 28);
            row_text_inset = row_text_inset ||
                             (utf8_equal(command.text, utf8_literal("/wordle")) && command.rectangle.left == at(6).left + 8);
        }
        check(texts && !rail_text && !summary && header_icon_gap && row_text_inset,
              "rows draw no per-row icon, headers carry the icon, rail entries draw no text, and the usage "
              "summary waits for selection");
        scene.selected_index = 4;
        ui_text_command_stream_reset(&stream);
        texts = popup_text_presentation_build_output(scene, kDefaultTheme, &output);
        UiRect name{}, chip{}, optional{};
        for (u32 n = 0; texts && n < stream.count; ++n) {
            if (utf8_equal(commands[n].text, utf8_literal("/ban")))
                name = commands[n].rectangle;
            if (utf8_equal(commands[n].text, utf8_literal("user")))
                chip = commands[n].rectangle;
            if (utf8_equal(commands[n].text, utf8_literal("+1 optional")))
                optional = commands[n].rectangle;
        }
        bool source_end = false;
        for (u32 n = 0; texts && n < stream.count; ++n)
            source_end = source_end || (utf8_equal(commands[n].text, utf8_literal("App")) &&
                                        commands[n].horizontal_alignment == UiTextAlignment::end &&
                                        commands[n].rectangle.right == at(6).right - 8);
        check(texts && ui_rect_has_area(chip) && chip.left >= name.right + 8 && optional.left > chip.right &&
                  source_end,
              "the selected row shows its required options as chips, then the optional count, and every "
              "row its app right-aligned");
        // The rail gap before Built-in holds a divider unless only the clock leads.
        const auto rail_divider = [&](ConstSlice<PopupItem> rail_items) noexcept {
            PaintStream rail_paint{};
            bool line = false;
            if (popup_scene_open(&scene, PopupKind::autocomplete, 7, rail_items, anchor, viewport, 760, true, 0.f,
                                 0.f)
                    .ok() &&
                paint_stream_initialize(&rail_paint) && paint_stream_reset(&rail_paint, 800, 600) &&
                popup_presentation_build(scene, kDefaultTheme, &rail_paint))
                for (u32 n = 0; n < rail_paint.count; ++n) {
                    const auto& q = rail_paint.quads[n];
                    line = line || (q.bottom - q.top == 1 && q.right <= scene.rectangle.left + kCommandRailWidth &&
                                    bytes_equal(&q.fill, &kDefaultTheme.tokens.border.subtle, sizeof(Color)));
                }
            paint_stream_release(&rail_paint);
            return line;
        };
        const PopupItem clock_only[] = {picker[0], picker[2], picker[8]};
        check(rail_divider({picker, 9}) && !rail_divider({clock_only, 3}),
              "Built-in's rail divider shows after an app, and only the gap after Frequently Used");
        // Loading: skeleton rows draw bars and no text.
        PopupItem loading[] = {{utf8_literal("COMMANDS MATCHING /B"), 30, popup_item_command_section},
                               popup_placeholder_row(31, 0, popup_item_command_row),
                               popup_placeholder_row(32, 1, popup_item_command_row)};
        PaintStream skeleton{};
        u32 bars = 0;
        if (popup_scene_open(&scene, PopupKind::autocomplete, 7, {loading, 3}, anchor, viewport, 760, true, 0.f, 0.f)
                .ok() &&
            paint_stream_initialize(&skeleton) && paint_stream_reset(&skeleton, 800, 600) &&
            popup_presentation_build(scene, kDefaultTheme, &skeleton))
            for (u32 n = 0; n < skeleton.count; ++n)
                bars += skeleton.quads[n].bottom - skeleton.quads[n].top == 16 && skeleton.quads[n].radius == 8 ? 1u : 0u;
        paint_stream_release(&skeleton);
        ui_text_command_stream_reset(&stream);
        check(bars == 6 && popup_text_presentation_build_output(scene, kDefaultTheme, &output) && stream.count == 1 &&
                  !popup_scene_hit_test(scene, (scene.rectangle.left + scene.rectangle.right) * .5f,
                                        scene.rectangle.top + 60)
                       .enabled,
              "placeholder rows show the client's usage, description and source bars, no text, and never act");
        // The selected command's lists: Generic rows (36 DIPs) under a title.
        PopupItem options[] = {{utf8_literal("OPTIONS"), 40, popup_item_command_section},
                               {utf8_literal("reason"), 41, popup_item_enabled},
                               popup_placeholder_row(42, 0)};
        options[1].detail = utf8_literal("The reason for banning, if any");
        PopupItemLayout option{}, option_loading{};
        ui_text_command_stream_reset(&stream);
        bool option_text = false, option_detail = false;
        const bool listed =
            popup_scene_open(&scene, PopupKind::autocomplete, 7, {options, 3}, anchor, viewport, 760, true, 0.f, 0.f)
                .ok() &&
            popup_scene_resolve_item_layout(scene, 1, &option) &&
            popup_scene_resolve_item_layout(scene, 2, &option_loading) &&
            popup_text_presentation_build_output(scene, kDefaultTheme, &output);
        for (u32 n = 0; listed && n < stream.count; ++n) {
            option_text = option_text || (utf8_equal(commands[n].text, utf8_literal("reason")) &&
                                           commands[n].rectangle.left == option.row->rectangle.left + 8);
            option_detail = option_detail || (utf8_equal(commands[n].text, options[1].detail) &&
                                               commands[n].horizontal_alignment == UiTextAlignment::end);
        }
        check(listed && ui_rect_height(option.row->rectangle) == kCommandGenericRowHeight &&
                  ui_rect_height(option_loading.row->rectangle) == kCommandGenericLoadingHeight &&
                  ui_rect_height(scene.rectangle) == 40 + 36 + 40 + 8 && option_text && option_detail,
              "option lists use the client's Generic and Loading rows under their title");
        // Scrolled into App's rows: its header stays pinned and hides the row
        // part beneath it, until the next section pushes it up.
        PopupItem scrolled[] = {picker[0], picker[1], picker[2], picker[6], picker[7], picker[8], picker[5]};
        scrolled[6].action = 20;
        scrolled[6].flags |= popup_item_command_pinned;
        const auto pinned = popup_scene_open(&scene, PopupKind::autocomplete, 7, {scrolled, 7}, anchor, viewport,
                                             760, true, 20.f, 0.f);
        PopupItemLayout row{}, header{}, next{};
        check(pinned.ok() && popup_scene_resolve_item_layout(scene, 3, &row) &&
                  popup_scene_resolve_item_layout(scene, 6, &header) && popup_scene_resolve_item_layout(scene, 4, &next) &&
                  header.row->rectangle.top == scene.rectangle.top && row.row->rectangle.top < header.row->rectangle.bottom &&
                  row.clip_rectangle.top == header.row->rectangle.bottom && next.row->rectangle.top > header.row->rectangle.bottom,
              "a pinned section header stays on top and clips the rows scrolled beneath it");
        const auto pushed = popup_scene_open(&scene, PopupKind::autocomplete, 7, {scrolled, 7}, anchor, viewport,
                                             760, true, 50.f, 0.f);
        check(pushed.ok() && popup_scene_resolve_item_layout(scene, 6, &header) &&
                  popup_scene_resolve_item_layout(scene, 4, &next) &&
                  header.row->rectangle.bottom == next.row->rectangle.top - 16 &&
                  header.row->rectangle.top < scene.rectangle.top,
              "the next section pushes the pinned header up");
        check(!popup_scene_open(&scene, PopupKind::autocomplete, 7, {picker, 9}, anchor, viewport, 760, true, 80.f,
                                0.f)
                   .ok(),
              "the body fraction stays within a row, its gap and the list inset");
        // Query results: the title, then every ranked row; no rail.
        PopupItem results[] = {{utf8_literal("COMMANDS MATCHING"), 30, popup_item_command_section},
                               picker[4], picker[8]};
        results[0].detail = utf8_literal("/ba");
        PopupItemLayout title{}, first{};
        check(popup_scene_open(&scene, PopupKind::autocomplete, 7, {results, 3}, anchor, viewport, 760, true, 0.f,
                               0.f)
                      .ok() &&
                  ui_rect_height(scene.rectangle) == 40 + 2 * 56 + 8 &&
                  popup_scene_resolve_item_layout(scene, 0, &title) &&
                  popup_scene_resolve_item_layout(scene, 1, &first) && title.row->rectangle.left == scene.rectangle.left &&
                  first.row->rectangle.top == scene.rectangle.top + 40 &&
                  first.row->rectangle.left == scene.rectangle.left + 8,
              "query results lead with the client's title and drop the rail");
        PopupItem misplaced = results[0];
        misplaced.flags |= popup_item_command_pinned;
        PopupItem orphan[] = {{utf8_literal("Menu"), 40, popup_item_enabled}, results[0]};
        orphan[1].action = 41;
        check(!popup_scene_open(&scene, PopupKind::autocomplete, 7, {&misplaced, 1}, anchor, viewport, 760, true, 0.f,
                                0.f)
                       .ok() &&
                  !popup_scene_open(&scene, PopupKind::autocomplete, 7, {orphan, 2}, anchor, viewport, 760, true, 0.f,
                                    0.f)
                       .ok(),
              "pinned headers belong to discovery and section rows to the picker");
    }
    popup_scene_release(&scene);
}
} // namespace maicord
