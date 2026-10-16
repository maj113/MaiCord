#include "../src/app/application_commands.hpp"
#include "../src/app/application_state.hpp"
#include "../src/app/application_plugins.hpp"
#include "maicord/app/message_composer_controller.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/writer.hpp"
#include "test_output.hpp"

namespace maicord::app_detail {
void test_activity_retry(void (*)(bool, const char*) noexcept) noexcept;
void test_custom_status_retry(void (*)(bool, const char*) noexcept) noexcept;
void test_notification_retry(void (*)(bool, const char*) noexcept) noexcept;
void test_community_retry(void (*)(bool, const char*) noexcept) noexcept;
void test_invite_retry(void (*)(bool, const char*) noexcept) noexcept;
u32 edits_committed;
UiRect command_viewport{0, 0, 800, 600};
bool refresh_message_composer_ui() noexcept {
    MessageComposerProjectionContext projection{};
    application_commands_composer(&projection);
    if (!message_composer_scene_update(&g_app.message_composer_scene, &g_app.message_composer,
                                       {0, 540, 800, 600}, true, 0, {}, &projection).ok()) return false;
    bool handled = false;
    return application_commands_sync(&handled);
}
bool composer_apply_result(MessageComposerResult result, bool) noexcept {
    if (result == MessageComposerResult::changed) {
        ++edits_committed;
        if (!message_composer_controller_commit_draft(&g_app.account_ui->message_composer_controller).ok())
            return false;
    }
    return refresh_message_composer_ui();
}
UiRect application_viewport_dips() noexcept { return command_viewport; }
void set_popup_owner(PopupOwnerKind owner) noexcept {
    if (g_app.popup_owner_kind == PopupOwnerKind::commands && owner != PopupOwnerKind::commands)
        application_commands_dismiss();
    g_app.popup_owner_kind = owner;
}
bool reveal_command_row(float, float) noexcept { return true; }
// No plugins are loaded in this fixture.
u32 application_plugins_commands(Utf8View, Slice<NativePluginSdkCommandView>) noexcept { return 0; }
bool application_plugins_command_pending() noexcept { return false; }
void application_plugins_menu_dismissed() noexcept {}
bool queue_message_send() noexcept { return true; }
u32 context_changes;
void application_user_actions_commands_changed() noexcept { ++context_changes; }
} // namespace maicord::app_detail

namespace maicord {
namespace {
using namespace app_detail;
u32 failures, calls, operation;
char payload[65536];
u32 payload_bytes;
u64 submitted_application, submitted_nonce;
RestClientSubmitError submit_error;
bool measure_header_name(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    if (!request.no_wrap || request.styles.count != 1 ||
        request.styles[0].byte_count != request.text.bytes ||
        request.styles[0].flags != text_style_bold)
        return false;
    *value = {{47, 20}, 16, 1};
    return true;
}
void check(bool ok, const char* label) noexcept {
    test_check(&failures, ok, label);
}
RestClientSubmitResult WINAPI submit(void*, u64, u64, u64 application, u64 nonce, Utf8View data,
                                          u64, u8 op) noexcept {
    ++calls; operation = op;
    submitted_application = application; submitted_nonce = nonce;
    payload_bytes = data.bytes;
    if (data.bytes) copy_bytes(payload, data.data, data.bytes);
    return {submit_error};
}
u32 action_named(Utf8View label, u32 ordinal = 0) noexcept {
    for (u32 n = 0; n < g_app.popup_scene.item_count; ++n)
        if (utf8_equal(g_app.popup_scene.items[n].label, label) &&
            (g_app.popup_scene.items[n].flags & popup_item_enabled)) {
            if (!ordinal) return g_app.popup_scene.items[n].action;
            --ordinal;
        }
    return 0;
}
bool payload_contains(Utf8View expected) noexcept {
    for (u32 at = 0; at + expected.bytes <= payload_bytes; ++at)
        if (bytes_equal(payload + at, expected.data, expected.bytes)) return true;
    return false;
}
void edit(Utf8View text) noexcept {
    message_composer_select_all(&g_app.message_composer);
    const auto result = message_composer_insert_utf8(&g_app.message_composer, text);
    check(composer_apply_result(result, true), "edit refresh");
}
void respond(u16 status, Utf8View body = {}) noexcept {
    RestClientResponse response{}; response.status_code = status;
    g_app.details_request_owner = MessageHistoryRequestOwner::none;
    check(application_commands_response(response, body), "response refresh");
}
void command_retry_cases() noexcept {
    constexpr u64 deadline = 900000000000ull;
    for (u32 kind = 0; kind < 4; ++kind) {
        application_commands_release();
        popup_scene_close(&g_app.popup_scene);
        set_popup_owner(PopupOwnerKind::none);
        edit({});
        edit(utf8_literal("/"));
        const u32 before = calls;
        check(application_commands_submit(700 + kind) && operation == 0,
              "command retry fixture submits server index");
        RestClientResponse response{};
        response.status_code = 429;
        response.rate_limited = kind != 1;
        response.retry_at_ms = kind == 2 ? 0 : deadline;
        response.rejection.captcha = kind == 0;
        g_app.details_request_owner = MessageHistoryRequestOwner::none;
        check(application_commands_response(response, {}), "command retry response refresh");
        if (kind == 3) {
            check(!application_commands_pending(deadline - 1) && application_commands_pending(deadline) &&
                      application_commands_next_wake() == deadline && calls == before + 1,
                  "command genuine 429 waits for the exact server deadline without another request");
            check(application_commands_submit(deadline) && operation == 0 && calls == before + 2,
                  "command genuine 429 retries the same index once admitted");
            respond(403);
        }
        // The independent user index is still needed; its failure must not
        // turn the terminal server-index response into another fetch.
        check(application_commands_submit(deadline + 1) && operation == 1,
              "command terminal response leaves only the independent user index");
        respond(403);
        check(!application_commands_pending(deadline) && !application_commands_next_wake() &&
                  calls == before + (kind == 3 ? 3u : 2u),
              "command challenge or missing retry classification leaves no automatic replay");
    }
    application_commands_release();
    popup_scene_close(&g_app.popup_scene);
    set_popup_owner(PopupOwnerKind::none);
    edit({});
}
void run() noexcept {
    Arena arena{};
    const usize bytes = sizeof(AccountUiRuntime);
    check(arena_initialize(&arena, bytes + 4096, 0, 4096), "fixture arena");
    g_app.account_ui = arena_allocate_zero<AccountUiRuntime>(&arena);
    if (!g_app.account_ui) { arena_release(&arena); return; }
    g_app.account.api.application_command = submit;
    g_app.renderer.dpi = 96;
    char input[kMessageComposerMaximumBytes]{};
    UiWidget widgets[70]{}; UiLayoutNode nodes[70]{};
    MessageRecord records[4]{}; u32 order[4]{}, heights[4]{}; u64 fenwick[4]{};
    u8 text[512]{}, draft_text[512]{}, request_text[kMessageComposerMaximumBytes]{};
    MessageDraftRecord draft_records[4]{};
    UserRecord bot_records[2]{}; u32 bot_order[2]{}; u8 bot_names[128]{};
    MessageStore store{}; UiVariableList list{}; MessageTimeline timeline{}; MessageDraftStore drafts{};
    auto& composer = g_app.message_composer;
    auto& controller = g_app.account_ui->message_composer_controller;
    const bool ready = message_composer_initialize(&composer, {input, sizeof(input)}) &&
        user_store_initialize(&g_app.account_ui->user_store, {bot_records, 2}, {bot_order, 2},
                              {bot_names, sizeof(bot_names)}) &&
        user_store_upsert(&g_app.account_ui->user_store, 11, utf8_literal("Test bot")).ok() &&
        user_store_upsert(&g_app.account_ui->user_store, 21, utf8_literal("Other bot")).ok() &&
        message_composer_scene_initialize_owned(&g_app.message_composer_scene) &&
        popup_scene_initialize_owned(&g_app.popup_scene, 32, {widgets, 70}, {nodes, 70}) &&
        message_store_initialize(&store, {records, 4}, {order, 4}, {text, sizeof(text)}) &&
        ui_variable_list_initialize(&list, {heights, 4}, {fenwick, 4}, 0, 100) &&
        message_timeline_initialize(&timeline, &store, &list, 100, 100, true) &&
        message_draft_store_initialize(&drafts, {draft_records, 4}, {draft_text, sizeof(draft_text)}) &&
        message_composer_controller_initialize(&controller, &composer, &drafts, &timeline,
                                               {request_text, sizeof(request_text)}) &&
        message_composer_controller_select_channel(&controller, 7).ok();
    check(ready, "command UI fixture");
    if (ready) {
        composer.focused = true;
        command_retry_cases();
        edit(utf8_literal("/"));
        check(application_commands_submit(1) && operation == 0, "load server command index");
        respond(200, utf8_literal(R"({"applications":[{"id":"10","name":"Test","bot":{"id":"11"}},{"id":"20","name":"Other","bot":{"id":"21"}}],"application_commands":[{"id":"100","application_id":"10","version":"7","name":"wordle","description":"Play wordle","type":1},{"id":"101","application_id":"10","version":"7","name":"echo","description":"Echo","type":1,"options":[{"name":"text","type":3,"required":true}]},{"id":"102","application_id":"10","version":"7","name":"pick","description":"Pick","type":1,"options":[{"name":"mode","type":4,"required":true,"choices":[{"name":"One","value":1},{"name":"Two","value":2}]}]},{"id":"103","application_id":"10","version":"7","name":"admin","description":"Admin","type":1,"options":[{"name":"group","type":2,"options":[{"name":"run","description":"Run","type":1,"options":[{"name":"count","type":4,"required":true}]}]}]},{"id":"104","application_id":"10","version":"7","name":"bump","description":"First bump","type":1},{"id":"105","application_id":"20","version":"7","name":"bump","description":"Second bump","type":1},{"id":"106","application_id":"10","version":"7","name":"search","description":"Search","type":1,"options":[{"name":"query","type":3,"required":true,"autocomplete":true},{"name":"count","type":4}]}]})"));
        check(application_commands_submit(2) && operation == 1, "load user command index");
        respond(200, utf8_literal(R"({"applications":[],"application_commands":[]})"));
        edit(utf8_literal("/word"));
        check(application_commands_key(VK_RETURN) && utf8_equal(message_composer_text(composer),
              utf8_literal("/wordle ")), "Enter selects command without submitting");
        check(g_app.popup_scene.item_count == 2 &&
              (g_app.popup_scene.items[0].flags & popup_item_command_header) &&
              utf8_equal(g_app.popup_scene.items[0].detail, utf8_literal("Play wordle")) &&
              ui_rect_height(g_app.popup_scene.rectangle) <= kUiMenuRowHeight + 2 * kPopupPanelPadding,
              "selected no-option command collapses into a compact description and close header");
        const auto& selected_scene = g_app.message_composer_scene;
        check(selected_scene.command.name_bytes == 7 && selected_scene.command.bot_id == 11 &&
              selected_scene.display_style_count == 1 && selected_scene.display_styles[0].byte_offset == 0 &&
              selected_scene.display_styles[0].byte_count == 7 &&
              (selected_scene.display_styles[0].flags & text_style_bold) &&
              ui_rect_has_area(message_composer_scene_command_avatar(selected_scene)),
              "selected command projects its bot avatar and bold editable name into the composer");
        const auto* close_layout = popup_scene_layout_node(g_app.popup_scene, g_app.popup_scene.rows[1].widget);
        bool hover_changed = false;
        check(close_layout && popup_scene_set_hover(&g_app.popup_scene,
              (close_layout->rectangle.left + close_layout->rectangle.right) * .5f,
              (close_layout->rectangle.top + close_layout->rectangle.bottom) * .5f, &hover_changed) &&
              popup_scene_selected_action(g_app.popup_scene) == action_named(utf8_literal("Clear command")),
              "compact close control owns only its trailing hit target");
        command_viewport.right = 240;
        check(refresh_message_composer_ui() && g_app.popup_scene.rectangle.right <= 240 &&
              ui_rect_height(g_app.popup_scene.rectangle) <= kUiMenuRowHeight + 2 * kPopupPanelPadding,
              "compact command header stays inside a narrow composer viewport");
        command_viewport.right = 800;
        check(refresh_message_composer_ui(), "restore command viewport");
        UiResolvedThemeStyles command_styles{};
        command_styles.ready = true;
        command_styles.geometry_revision = 1;
        command_styles.metrics.menu_row_height = 44;
        command_styles.metrics.menu_padding = 8;
        check(popup_scene_bind_theme_styles(&g_app.popup_scene, &command_styles) &&
              ui_rect_height(g_app.popup_scene.rectangle) == 60,
              "compact command header follows resolved menu geometry");
        check(popup_scene_bind_theme_styles(&g_app.popup_scene, nullptr), "restore command menu geometry");
        UiTextMeasureRecord header_measure_records[4]{};
        UiTextMeasureCache header_measure{};
        UiTextCommand header_text[4]{}; UiTextCommandStream header_stream{};
        const auto original_header_name = g_app.popup_scene.items[0].label;
        g_app.popup_scene.items[0].label = utf8_literal("/caf\xC3\xA9");
        bool measured_name = false, measured_description = false;
        const bool header_shaped = ui_text_measure_cache_initialize(&header_measure, {header_measure_records, 4},
            measure_header_name, nullptr) && ui_text_command_stream_initialize(&header_stream, {header_text, 4}) &&
            popup_text_presentation_build(g_app.popup_scene, kDefaultTheme, &header_stream, &header_measure);
        float name_right = 0, description_left = 0;
        for (u32 i = 0; header_shaped && i < header_stream.count; ++i) {
            const auto& item = header_text[i];
            if (utf8_equal(item.text, g_app.popup_scene.items[0].label)) {
                measured_name = ui_rect_width(item.rectangle) == 47;
                name_right = item.rectangle.right;
            } else if (utf8_equal(item.text, g_app.popup_scene.items[0].detail)) {
                measured_description = true;
                description_left = item.rectangle.left;
            }
        }
        check(header_shaped && measured_name && measured_description && header_measure.backend_calls == 1 &&
              description_left - name_right == UiThemeMetrics{}.menu_gap,
              "command summary joins Unicode bold name and description using shaped width and theme gap");
        g_app.popup_scene.items[0].label = original_header_name;
        const u32 before = calls;
        check(application_commands_key(VK_RETURN) && !g_app.popup_scene.active && composer.bytes &&
              calls == before, "Enter queues and dismisses picker before network completion");
        message_composer_select_all(&composer);
        refresh_message_composer_ui();
        check(!g_app.popup_scene.active && composer.anchor != composer.caret,
              "pending command remains editable without reopening picker");
        check(application_commands_submit(3) && operation == 2 && payload_bytes,
              "queued command uses deterministic fake account boundary");
        const u32 committed = edits_committed;
        respond(204);
        check(!composer.bytes && !g_app.popup_scene.active && edits_committed == committed + 1,
              "selection changes do not prevent success clearing through normal edit path");
        message_composer_controller_select_channel(&controller, 8);
        message_composer_controller_select_channel(&controller, 7);
        check(!composer.bytes, "successful command clears retained channel draft");
        edit(utf8_literal("/word")); application_commands_key(VK_RETURN); application_commands_key(VK_RETURN);
        check(application_commands_submit(4), "second command submits");
        edit(utf8_literal("new draft")); respond(204);
        check(utf8_equal(message_composer_text(composer), utf8_literal("new draft")),
              "late success preserves edits made after submit");
        edit(utf8_literal("/word")); application_commands_key(VK_RETURN); application_commands_key(VK_RETURN);
        check(application_commands_submit(4), "command submits before channel change");
        message_composer_controller_select_channel(&controller, 8);
        edit(utf8_literal("another channel")); respond(204);
        check(composer.channel_id == 8 && utf8_equal(message_composer_text(composer), utf8_literal("another channel")),
              "late success cannot erase another channel draft");
        message_composer_controller_select_channel(&controller, 7);
        edit(utf8_literal("/word")); application_commands_key(VK_RETURN); application_commands_key(VK_RETURN);
        check(application_commands_submit(5), "failed command submits"); respond(400);
        check(composer.bytes && g_app.popup_scene.active, "failure preserves arguments and restores picker");
        bool error_visible = false;
        for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
            error_visible |= utf8_equal(g_app.popup_scene.items[i].detail,
                utf8_literal("Command failed. Try again."));
        check(error_visible, "failure explanation is retained");
        edit(utf8_literal("/word")); application_commands_key(VK_RETURN); application_commands_key(VK_RETURN);
        submit_error = RestClientSubmitError::busy;
        check(application_commands_submit(6) && !g_app.popup_scene.active && composer.bytes,
              "busy account lane preserves pending draft without reopening picker");
        submit_error = RestClientSubmitError::invalid_input;
        check(application_commands_submit(106) && g_app.popup_scene.active && composer.bytes,
              "immediate submit failure restores an editable picker");
        submit_error = RestClientSubmitError::none;
        popup_scene_close(&g_app.popup_scene); set_popup_owner(PopupOwnerKind::none);
        message_composer_select_all(&composer); refresh_message_composer_ui();
        check(!g_app.popup_scene.active, "dismissed picker stays closed during selection edits");
        check(g_app.message_composer_scene.command.name_bytes == 7 && composer.bytes,
              "Escape-style popup dismissal keeps the selected editable command identity");
        edit(utf8_literal("/wor"));
        check(g_app.popup_scene.active && !g_app.message_composer_scene.command.name_bytes &&
              (g_app.popup_scene.items[0].flags & popup_item_command_category),
              "backspacing through a selected command name restores discovery and clears its decoration");
        application_commands_key(VK_RETURN);
        const u32 clear_action = action_named(utf8_literal("Clear command"));
        check(clear_action && application_commands_activate(clear_action) && !composer.bytes &&
              !g_app.popup_scene.active && !g_app.message_composer_scene.command.name_bytes,
              "close clears selected command and restores the empty composer");

        edit(utf8_literal("/echo \t"));
        check(action_named(utf8_literal("/echo")) != 0,
              "trailing whitespace does not hide an exact command from discovery");
        edit(utf8_literal("/"));
        edit(utf8_literal("/echo text:\"keep quoted value\""));
        const u32 echo_action = action_named(utf8_literal("/echo"));
        check(echo_action && application_commands_activate(echo_action) &&
              utf8_equal(message_composer_text(composer), utf8_literal("/echo text:\"keep quoted value\"")),
              "selecting an exact typed command preserves its existing named arguments");
        check(application_commands_key(VK_RETURN) && application_commands_submit(1000) && operation == 2 &&
              payload_contains(utf8_literal("\"value\":\"keep quoted value\"")),
              "typed arguments survive discovery, selection and interaction serialization");
        respond(204);

        edit(utf8_literal("/pick"));
        application_commands_key(VK_RETURN);
        const u32 old_option_action = action_named(utf8_literal("mode"));
        application_commands_key(VK_TAB);
        check((g_app.popup_scene.items[0].flags & popup_item_command_header) &&
              utf8_equal(g_app.popup_scene.items[0].label, utf8_literal("mode")) &&
              g_app.popup_scene.item_count == 4 && ui_rect_height(g_app.popup_scene.rectangle) < 200,
              "focused option shows its compact summary above only its two choices");
        const u32 choice_action = action_named(utf8_literal("Two"));
        check(choice_action && application_commands_activate(choice_action) &&
              utf8_equal(message_composer_text(composer), utf8_literal("/pick mode:2 ")),
              "static numeric choice maps its visible row to the typed value");
        check(application_commands_key(VK_RETURN) && application_commands_submit(1001) &&
              payload_contains(utf8_literal("\"type\":4,\"value\":2")),
              "chosen numeric option remains a JSON number at execution");
        respond(204);
        check(old_option_action && application_commands_activate(old_option_action) && !composer.bytes,
              "a retired option action cannot dereference or edit a completed command");

        edit(utf8_literal("/admin group run count:4"));
        const u32 nested_action = action_named(utf8_literal("/admin group run"));
        check(nested_action && application_commands_activate(nested_action) &&
              application_commands_key(VK_RETURN) && application_commands_submit(1002) &&
              payload_contains(utf8_literal("\"type\":2,\"name\":\"group\",\"options\":[{\"type\":1,\"name\":\"run\"")),
              "flattened subcommand selection preserves arguments and serializes its nested wire scope");
        respond(204);

        edit(utf8_literal("/bump"));
        const u32 second_app_action = action_named(utf8_literal("/bump"), 1);
        check(second_app_action && application_commands_activate(second_app_action) &&
              g_app.message_composer_scene.command.bot_id == 21 &&
              application_commands_key(VK_RETURN) && application_commands_submit(1003) &&
              submitted_application == 20 && payload_contains(utf8_literal("\"id\":\"105\"")),
              "duplicate command labels retain the selected application and command identity");
        respond(204);

        edit(utf8_literal("/search"));
        application_commands_key(VK_RETURN);
        edit(utf8_literal("/search query:\"old\" count:2"));
        const u32 query_action = action_named(utf8_literal("query"));
        check(query_action && application_commands_activate(query_action),
              "an existing middle option can be focused without duplicating it");
        const u64 suggestion_due = application_commands_next_wake();
        check(suggestion_due && application_commands_submit(suggestion_due) && operation == 3 &&
              submitted_nonce && payload_contains(utf8_literal("\"value\":\"old\",\"focused\":true")) &&
              payload_contains(utf8_literal("\"name\":\"count\",\"type\":4,\"value\":2")),
              "dynamic autocomplete keeps completed later options and marks only the focused value");
        respond(204);
        u8 gateway[512]{};
        ProtocolWriter gateway_writer{{gateway, sizeof(gateway)}, 0};
        const bool gateway_ok = protocol_writer_append_literal(&gateway_writer, "{\"nonce\":\"") &&
            protocol_writer_append_u64(&gateway_writer, submitted_nonce) &&
            protocol_writer_append_literal(&gateway_writer,
                "\",\"choices\":[{\"name\":\"Picked\",\"value\":\"picked value\"}]}");
        if (gateway_ok) application_commands_gateway({reinterpret_cast<const char*>(gateway), gateway_writer.used});
        check(gateway_ok && application_commands_tick(clock_monotonic_milliseconds()),
              "controlled Gateway autocomplete completion updates the active option picker");
        const u32 dynamic_action = action_named(utf8_literal("Picked"));
        check(dynamic_action && application_commands_activate(dynamic_action) &&
              utf8_equal(message_composer_text(composer), utf8_literal("/search query:\"picked value\" count:2")),
              "dynamic choice replaces only the middle value and preserves later arguments");
        check(application_commands_key(VK_RETURN) && application_commands_submit(clock_monotonic_milliseconds()) &&
              operation == 2 && payload_contains(utf8_literal("\"value\":\"picked value\"")),
              "accepted dynamic choice is execution data rather than another autocomplete request");
        respond(204);

        edit(utf8_literal("/echo")); application_commands_key(VK_RETURN);
        const u32 unchanged = calls;
        check(application_commands_key(VK_RETURN) && composer.bytes && g_app.popup_scene.active && calls == unchanged,
              "invalid arguments preserve editor and never submit");
        UiTextCommand command_text[96]{}; UiTextCommandStream stream{};
        ui_text_command_stream_initialize(&stream, {command_text, 96});
        check(popup_text_presentation_build(g_app.popup_scene, kDefaultTheme, &stream) && stream.count,
              "command error picker retains visible text commands");

        // Context menu commands (Apps): user (type 2) and message (type 3).
        message_composer_clear(&composer);
        popup_scene_close(&g_app.popup_scene); set_popup_owner(PopupOwnerKind::none);
        g_app.account_ui->active_channel_id = 9;
        message_composer_controller_select_channel(&controller, 9);
        ApplicationContextCommand found[4]{};
        bool loading = false;
        check(application_commands_context(2, {found, 4}, &loading) == 0 && loading,
              "Apps asks for the active channel's command index");
        const u32 changes = context_changes;
        check(application_commands_submit(2000) && operation == 0, "a context menu loads the index without the picker");
        respond(200, utf8_literal(R"({"applications":[{"id":"30","name":"Helper"}],"application_commands":[{"id":"300","application_id":"30","version":"9","name":"Inspect user","type":2},{"id":"301","application_id":"30","version":"9","name":"Quote","type":3,"dm_permission":true},{"id":"302","application_id":"30","version":"9","name":"Guild only","type":3,"guild_id":"55"},{"id":"303","application_id":"30","version":"9","name":"ping","description":"Ping","type":1}]})"));
        check(context_changes == changes + 1, "the landed index refreshes an open Apps list");
        u32 count = application_commands_context(2, {found, 4}, &loading);
        check(count == 1 && !loading && found[0].command_id == 300 && utf8_equal(found[0].name, utf8_literal("Inspect user")) &&
              utf8_equal(found[0].application_name, utf8_literal("Helper")),
              "user commands list by name with their application");
        count = application_commands_context(3, {found, 4}, &loading);
        check(count == 1 && found[0].command_id == 301, "message commands skip other servers' commands");
        check(application_commands_context_invoke(3, 301, 777) && application_commands_submit(2002) && operation == 2 &&
              submitted_application == 30 &&
              utf8_equal({payload, payload_bytes},
                         utf8_literal(R"({"version":"9","id":"301","name":"Quote","type":3,"options":[],"application_command":{"id":"301","application_id":"30","version":"9","name":"Quote","type":3,"dm_permission":true},"target_id":"777"})")),
              "a message command sends the official context data with the target message");
        edit(utf8_literal("draft stays"));
        respond(204);
        check(utf8_equal(message_composer_text(composer), utf8_literal("draft stays")),
              "running a context command never clears the composer");
        check(application_commands_context_invoke(2, 300, 888) && application_commands_submit(2003) &&
              payload_contains(utf8_literal("\"type\":2,\"options\":[]")) &&
              payload_contains(utf8_literal("\"target_id\":\"888\"")),
              "a user command targets the user");
        respond(204);
        check(!application_commands_context_invoke(2, 999, 1), "unknown commands never submit");
    }
    if (ready)
        test_invite_retry(check);
    application_commands_release(); popup_scene_release(&g_app.popup_scene);
    message_composer_scene_release(&g_app.message_composer_scene); message_composer_release(&composer);
    g_app.account_ui = nullptr; arena_release(&arena);
}
}
}
extern "C" void maicord_command_ui_tests_entry() noexcept {
    maicord::run();
    maicord::app_detail::test_activity_retry(maicord::check);
    maicord::app_detail::test_custom_status_retry(maicord::check);
    maicord::app_detail::test_notification_retry(maicord::check);
    maicord::app_detail::test_community_retry(maicord::check);
    ExitProcess(maicord::failures ? 1 : 0);
}
