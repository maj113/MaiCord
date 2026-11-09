#include "commands_ui_internal.hpp"
#include "../../../../src/app/chat/composer/commands.hpp"
#include "maicord/app/gateway/client_event.hpp"
#include "../../../../src/app/plugins/plugins.hpp"
#include "../../../../src/app/chat/composer/text_form.hpp"
#include "maicord/protocol/messages/application_commands.hpp"
#include "maicord/ui/shell/popup_presentation.hpp"
#include "../../../support/test_output.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_user.hpp"
#endif
#include "maicord/platform/window/key.hpp"
#include "../../../../src/app/account/account_ui_runtime.hpp"
#include "maicord/gpu/api.hpp"

namespace maicord {
// The app shake moves the composition visual; the fixture counts the moves.
u32 shake_offsets, shake_rests;
GpuStatus gpu_surface_set_offset(GpuSurface*, float x, float y, bool* needs_present) {
    *needs_present = false;
    ++(x == 0 && y == 0 ? shake_rests : shake_offsets);
    return 0;
}
GpuStatus gpu_surface_present(GpuSurface*, u32, const GpuRect*, u32) {
    return 0;
}
#if !defined(_WIN32)
// The Wayland backend holds the clipboard and the headless window backend of the tests has none; no case reads it.
bool clipboard_write_utf8(const Utf8View&) noexcept {
    return true;
}
#endif

} // namespace maicord

namespace maicord::app_detail {
void test_activity_retry(void (*)(bool, const char*) noexcept) noexcept;
void test_custom_status_retry(void (*)(bool, const char*) noexcept) noexcept;
void test_notification_retry(void (*)(bool, const char*) noexcept) noexcept;
void test_community_retry(void (*)(bool, const char*) noexcept) noexcept;
void test_invite_retry(void (*)(bool, const char*) noexcept) noexcept;
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
bool reveal_command_row(float, float, bool) noexcept { return true; }
void application_plugins_menu_dismissed() noexcept {}
bool queue_message_send() noexcept { return true; }
// Slowmode tests leave reaction, edit, history and connection UI effects idle.
bool queue_message_reaction_emoji(u64, u64, const Utf8View&, bool) noexcept { return false; }
u64 application_message_edit_latest_own() noexcept { return 0; }
bool queue_message_edit_content(u64, const Utf8View&) noexcept { return false; }
Utf8View application_account_notice() noexcept { return {}; }
u32 context_changes;
void application_user_actions_commands_changed() noexcept { ++context_changes; }
u32 frame_refreshes, modal_nonces;
u64 expected_modal;
ApplicationTextFormRequest confirm_request;
bool refresh_account_frame(bool) noexcept { ++frame_refreshes; return true; }
void application_message_components_expect_modal(u64 nonce, u64) noexcept {
    ++modal_nonces;
    expected_modal = nonce;
}
// Declined unless a case opts in; the invite fixture expects no modal.
bool confirm_opens;
bool application_text_form_open(const ApplicationTextFormRequest& request) noexcept {
    if (!confirm_opens)
        return false;
    confirm_request = request;
    return true;
}
// Staged uploads of the composer's channel, as the attachment pipeline reports them.
UploadedAttachment staged_uploads[2];
u32 staged_upload_count, picker_opens, uploads_completed;
u64 uploads_queued_nonce;
bool application_attachment_prepare_send(u64, Slice<UploadedAttachment> references, Slice<u64> ids, u32* count,
                                         Slice<u64>) noexcept {
    for (u32 i = 0; i < staged_upload_count; ++i) {
        references[i] = staged_uploads[i];
        references[i].id = i;
        ids[i] = 50 + i;
    }
    *count = staged_upload_count;
    return true;
}
void application_attachment_did_queue(ConstSlice<u64> ids, u64 nonce) noexcept {
    if (ids.count)
        uploads_queued_nonce = nonce;
}
void application_attachment_complete(u64) noexcept { ++uploads_completed; }
bool application_pick_attachments() noexcept {
    ++picker_opens;
    return true;
}
bool application_composer_fill_command(Utf8View name) noexcept {
    char text[160];
    text[0] = '/';
    copy_bytes(text + 1, name.data, name.bytes);
    text[name.bytes + 1] = ' ';
    return composer_apply_result(message_composer_replace_range(&g_app.message_composer, 0,
                                                                g_app.message_composer.bytes,
                                                                {text, name.bytes + 2}),
                                 true);
}
// Built-in commands: the inline results, group leave and DM open they start.
UiMediaPickerKind searched_kind;
char searched[64];
u32 searched_bytes, searches;
bool application_media_picker_inline(UiMediaPickerKind kind, Utf8View query, UiRect, bool) noexcept {
    searched_kind = kind;
    searched_bytes = utf8_copy_truncated(searched, query);
    ++searches;
    return true;
}
void application_media_picker_inline_close() noexcept {}
// /tts stays out of these channels' built-ins (its playback switch reads off).
bool application_user_settings_tts_command() noexcept { return false; }
// The inline results' Enter contract as the commands see it (the real picker has
// its own runtime test): ready results send the selected one and end the
// command, loading ones are waited for, anything else is unhandled.
FakeResults fake_results;
bool fake_waiting;
u32 fake_selected, fake_sent_index = ~u32{0}, fake_sends, fake_waits;
bool send_fake_result() noexcept {
    fake_sent_index = fake_selected;
    ++fake_sends;
    fake_results = FakeResults::none;
    return application_commands_builtin_used({});
}
bool application_media_picker_inline_key(u32 key, bool* handled) noexcept {
    *handled = key == key::enter && fake_results != FakeResults::none;
    if (!*handled)
        return true;
    if (fake_results == FakeResults::ready)
        return send_fake_result();
    fake_waiting = true;
    ++fake_waits;
    return true;
}
Utf8View application_media_picker_inline_message() noexcept {
    return fake_empty_message;
}
bool fake_results_arrive(u32 count) noexcept {
    if (!fake_waiting)
        return true;
    fake_waiting = false;
    if (!count) {
        fake_results = FakeResults::none;
        return application_commands_results_refused();
    }
    return send_fake_result();
}
u64 left_group;
bool left_silent;
bool application_private_channel_leave(u64 channel_id, bool silent) noexcept {
    left_group = channel_id;
    left_silent = silent;
    return true;
}
u64 opened_dm_user;
bool queue_private_conversation(PrivateConversationOperation, u64 id) noexcept {
    opened_dm_user = id;
    return true;
}
bool thread_sendable;
bool application_thread_actions_channel_sendable(u64) noexcept { return thread_sendable; }

} // namespace maicord::app_detail

namespace maicord {
namespace commands_ui_detail {

u32 failures, calls, operation;
char payload[65536];
u32 payload_bytes;
u64 submitted_channel, submitted_guild, submitted_application, submitted_nonce, submitted_request;

} // namespace commands_ui_detail

namespace {

RestClientSubmitError submit_error;

} // namespace

namespace commands_ui_detail {

void check(bool ok, const char* label) noexcept {
    test_check(&failures, ok, label);
}

} // namespace commands_ui_detail

namespace {

RestClientSubmitResult WINAPI submit(void*, u64 channel, u64 guild, u64 application, u64 nonce,
                                     Utf8View data, u64 request_id, u8 op) noexcept {
    ++calls;
    operation = op;
    submitted_request = request_id;
    submitted_channel = channel;
    submitted_guild = guild;
    submitted_application = application;
    submitted_nonce = nonce;
    payload_bytes = data.bytes;
    if (data.bytes)
        copy_bytes(payload, data.data, data.bytes);
    return {submit_error};
}

} // namespace

namespace commands_ui_detail {

u32 action_named(Utf8View label, u32 ordinal) noexcept {
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

} // namespace commands_ui_detail

namespace {

// The one local command message on the visible timeline, if any.
const MessageRecord* only_echo() noexcept {
    const auto& store = *g_app.message_view.timeline.store;
    for (u32 i = 0; i < store.capacity; ++i)
        if (store.records[i].occupied)
            return &store.records[i];
    return nullptr;
}
bool only_echo_with(u64 id) noexcept {
    return id && message_store_find(*g_app.message_view.timeline.store, id).ok();
}
Utf8View echo_text(const MessageRecord& record) noexcept {
    return {reinterpret_cast<const char*>(g_app.message_view.timeline.store->text + record.content_offset),
            record.content_bytes};
}
void interaction(GatewayClientDispatchKind kind, u64 id, u64 nonce) noexcept {
    u8 body[128]{};
    ProtocolWriter writer{{body, sizeof(body)}, 0};
    bool ok = protocol_writer_append_literal(&writer, "{\"id\":\"") &&
              protocol_writer_append_u64(&writer, id ? id : 1) && protocol_writer_append_literal(&writer, "\"");
    if (nonce)
        ok = ok && protocol_writer_append_literal(&writer, ",\"nonce\":\"") &&
             protocol_writer_append_u64(&writer, nonce) && protocol_writer_append_literal(&writer, "\"");
    ok = ok && protocol_writer_append_literal(&writer, "}");
    check(ok, "interaction event fixture");
    application_commands_interaction(kind, {reinterpret_cast<const char*>(body), writer.used});
}

} // namespace

namespace commands_ui_detail {

// The URL of a command app's picture fetched by URL (empty for a user avatar).
Utf8View picture_url(MediaAssetKey key, char (&url)[160]) noexcept {
    return key.kind == MediaAssetKind::profile_image
               ? Utf8View{url, application_commands_icon_url(key.id, {url, sizeof(url)})}
               : Utf8View{};
}
void edit(Utf8View text) noexcept {
    message_composer_select_all(&g_app.message_composer);
    const auto result = message_composer_insert_utf8(&g_app.message_composer, text);
    check(composer_apply_result(result, true), "edit refresh");
}

void respond(u16 status, Utf8View body) noexcept {
    RestClientResponse response{}; response.status_code = status;
    g_app.details_request_owner = MessageHistoryRequestOwner::none;
    check(application_commands_response(response, body), "response refresh");
}

} // namespace commands_ui_detail

namespace {

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
    UserRecord bot_records[4]{}; u32 bot_order[4]{}; u8 bot_names[256]{};
    MessageStore store{}; UiVariableList list{}; MessageTimeline timeline{}; MessageDraftStore drafts{};
    MessageRecord echo_records[16]{}; u32 echo_order[16]{}, echo_heights[16]{}; u64 echo_fenwick[16]{};
    u8 echo_text_bank[1024]{};
    MessageStore echo_store{}; UiVariableList echo_list{};
    auto& composer = g_app.message_composer;
    auto& controller = g_app.account_ui->message_composer_controller;
    const bool ready = message_composer_initialize(&composer, {input, sizeof(input)}) &&
        user_store_initialize(&g_app.account_ui->user_store, {bot_records, 4}, {bot_order, 4},
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
        command_index_invalidation_cases();
        // Local command messages land on the visible channel's timeline.
        check(message_store_initialize(&echo_store, {echo_records, 16}, {echo_order, 16},
                                       {echo_text_bank, sizeof(echo_text_bank)}) &&
                  ui_variable_list_initialize(&echo_list, {echo_heights, 16}, {echo_fenwick, 16}, 0, 100) &&
                  message_timeline_initialize(&g_app.message_view.timeline, &echo_store, &echo_list, 100, 100, true),
              "command echo timeline fixture");
        g_app.account_ui->active_channel_id = 7;
        edit(utf8_literal("/"));
        check(application_commands_submit(1) && operation == 0, "load server command index");
        respond(200, fixture_index);
        check(application_commands_submit(2) && operation == 1, "load user command index");
        respond(200, utf8_literal(R"({"applications":[],"application_commands":[]})"));
        {
            // Keyboard navigation follows two lists: the discovery
            // list (empty query) and the results of a typed query.
            const auto row_action = [](bool last) noexcept {
                u32 action = 0;
                for (u32 n = 0; n < g_app.popup_scene.item_count; ++n)
                    if ((g_app.popup_scene.items[n].flags & popup_item_command_row) &&
                        (g_app.popup_scene.items[n].flags & popup_item_enabled)) {
                        action = g_app.popup_scene.items[n].action;
                        if (!last)
                            break;
                    }
                return action;
            };
            const auto selected = [] noexcept { return popup_scene_selected_action(g_app.popup_scene); };
            edit(utf8_literal("/"));
            check(g_app.popup_scene.active && !selected(), "discovery opens without a keyboard selection");
            check(!application_commands_key(key::enter) && utf8_equal(message_composer_text(composer), utf8_literal("/")),
                  "Enter without a discovery selection keeps its composer meaning");
            check(application_commands_key(key::tab) && selected() == row_action(false) &&
                      utf8_equal(message_composer_text(composer), utf8_literal("/")),
                  "Tab without a selection selects the top discovery row without choosing it");
            check(application_commands_key(key::up) && selected() == row_action(false),
                  "Up stops at the top of the discovery list");
            check(application_commands_key(key::down) && selected() != row_action(false) && selected(),
                  "Down moves the discovery selection");
            application_commands_hover();
            check(application_commands_key(key::down) && selected() == row_action(false),
                  "after hovering, Down starts again at the top of the discovery list");
            edit(utf8_literal("/o"));
            check(selected() == row_action(false), "typed query results focus their first row");
            check(application_commands_key(key::up) && selected() == row_action(true),
                  "Up from the first query result wraps to the last");
            check(application_commands_key(key::down) && selected() == row_action(false),
                  "Down from the last query result wraps to the first");
            application_commands_hover();
            check(application_commands_key(key::up) && selected() == row_action(true),
                  "after hovering, Up starts from the end of the query results");
            application_commands_hover();
            check(!application_commands_key(key::tab) && utf8_equal(message_composer_text(composer), utf8_literal("/o")),
                  "Tab after hovering keeps its composer meaning instead of choosing the hovered row");
        }
        edit(utf8_literal("/word"));
        check(application_commands_key(key::enter) && utf8_equal(message_composer_text(composer),
              utf8_literal("/wordle ")), "Enter selects command without submitting");
        const auto& selected_scene = g_app.message_composer_scene;
        check(!g_app.popup_scene.active &&
                  utf8_equal(message_composer_scene_command_bar_name(selected_scene), utf8_literal("/wordle")) &&
                  utf8_equal(message_composer_scene_command_bar_detail(selected_scene), utf8_literal("Play wordle")) &&
                  !selected_scene.command_bar_error &&
                  ui_rect_has_area(message_composer_scene_command_bar_text_rectangle(selected_scene)),
              "a selected command without options shows only the composer's option bar, as in the client");
        char first_picture[160]{};
        check(selected_scene.command.name_bytes == 7 &&
              utf8_equal(picture_url(selected_scene.command.avatar, first_picture),
                         utf8_literal("https://cdn.discordapp.com/embed/avatars/0.png")) &&
              selected_scene.display_style_count == 1 && selected_scene.display_styles[0].byte_offset == 0 &&
              selected_scene.display_styles[0].byte_count == 7 &&
              (selected_scene.display_styles[0].flags & text_style_bold) &&
              ui_rect_has_area(message_composer_scene_command_avatar(selected_scene)),
              "selected command projects its app's picture and bold editable name into the composer");
        const auto* bar_layout = ui_widget_manager_layout_node(selected_scene.widgets, selected_scene.command_bar);
        const auto* close_layout =
            message_composer_scene_layout_node(selected_scene, MessageComposerControl::command_close);
        const auto* edit_layout = message_composer_scene_layout_node(selected_scene, MessageComposerControl::edit);
        check(bar_layout && close_layout && edit_layout &&
                  ui_rect_height(bar_layout->rectangle) == kMessageComposerCommandBarHeight &&
                  bar_layout->rectangle.bottom <= edit_layout->rectangle.top &&
                  close_layout->rectangle.right <= bar_layout->rectangle.right &&
                  message_composer_scene_hit_test(selected_scene,
                                                  (close_layout->rectangle.left + close_layout->rectangle.right) * .5f,
                                                  (close_layout->rectangle.top + close_layout->rectangle.bottom) * .5f) ==
                      MessageComposerControl::command_close,
              "the option bar stacks above the editor with its trailing close button");
        const u32 before = calls, modals_before = modal_nonces;
        check(application_commands_key(key::enter) && !g_app.popup_scene.active && !composer.bytes &&
              calls == before && modal_nonces == modals_before + 1,
              "Enter queues the command, clears the draft like the official client and dismisses the picker");
        const MessageRecord* echo = only_echo();
        check(echo && echo->type == 20 && echo->author_id == 11 && echo->channel_id == 7 &&
              echo->delivery == MessageDeliveryState::pending &&
              utf8_equal(echo_text(*echo), utf8_literal("Sending command...")),
              "queued command shows the official local message under its bot until the interaction exists");
        check(application_commands_submit(3) && operation == 2 && payload_bytes && echo &&
              echo->id == submitted_nonce && expected_modal == submitted_nonce,
              "queued command sends the nonce of its local message and expects its modal");
        const u32 refreshes = frame_refreshes;
        check(application_commands_next_wake() == 1 && application_commands_tick(1) &&
              frame_refreshes == refreshes + 1 && application_commands_next_wake() != 1,
              "local command message repaints once through the deferred frame refresh");
        edit(utf8_literal("new draft"));
        respond(204);
        check(utf8_equal(message_composer_text(composer), utf8_literal("new draft")),
              "interaction acceptance never edits the composer");
        interaction(GatewayClientDispatchKind::interaction_create, 900, submitted_nonce);
        echo = only_echo();
        check(echo && echo->content_bytes == 0 && (echo->flags & (1u << 7)) &&
              echo->delivery == MessageDeliveryState::pending,
              "INTERACTION_CREATE turns the local message into the deferred thinking state");
        interaction(GatewayClientDispatchKind::interaction_success, 900, 0);
        echo = only_echo();
        check(echo && echo->content_bytes == 0 && !(echo->flags & (1u << 7)),
              "INTERACTION_SUCCESS matched by interaction id leaves the message without a status");
        application_commands_message_created(submitted_nonce);
        check(!only_echo(), "the response message with the command nonce replaces the local message");
        edit(utf8_literal("/word")); application_commands_key(key::enter); application_commands_key(key::enter);
        check(application_commands_submit(5), "failed command submits");
        respond(400, utf8_literal(R"({"code":50035,"errors":{"data":{"options":{"_errors":[{"code":"INTERACTION_APPLICATION_COMMAND_INVALID_VERSION","message":"This command is outdated, please try again in a few minutes"}]}}},"message":"Invalid Form Body"})"));
        echo = only_echo();
        check(!composer.bytes && !g_app.popup_scene.active && echo &&
              echo->delivery == MessageDeliveryState::failed &&
              utf8_equal(echo_text(*echo),
                         utf8_literal("This command is outdated, please try again in a few minutes")),
              "an API refusal fails the local message with the client's nested form error");
        edit(utf8_literal("/"));
        check(application_commands_submit(7) && operation == 0,
              "an outdated command version refetches the command index when discovery is wanted");
        respond(200, fixture_index);
        check(application_commands_submit(8) && operation == 1, "an outdated command version refetches the user index");
        respond(200, utf8_literal(R"({"applications":[],"application_commands":[]})"));
        message_timeline_remove_local(&g_app.message_view.timeline, only_echo() ? only_echo()->id : 0);
        edit(utf8_literal("/word")); application_commands_key(key::enter); application_commands_key(key::enter);
        submit_error = RestClientSubmitError::busy;
        check(application_commands_submit(6) && !g_app.popup_scene.active && !composer.bytes &&
                  application_commands_pending(106),
              "busy account lane keeps the queued command for a later turn");
        submit_error = RestClientSubmitError::invalid_input;
        echo = nullptr;
        check(application_commands_submit(106) && !application_commands_pending(107) && (echo = only_echo()) &&
                  utf8_equal(echo_text(*echo), utf8_literal("This interaction failed")),
              "immediate submit failure fails the local message");
        submit_error = RestClientSubmitError::none;
        const u64 failed_id = echo ? echo->id : 0;
        const u32 failed_payload_bytes = payload_bytes;
        check(application_commands_retry(failed_id) && !only_echo_with(failed_id) &&
                  application_commands_pending(300) && application_commands_submit(300) && operation == 2 &&
                  submitted_nonce != failed_id && payload_bytes == failed_payload_bytes &&
                  only_echo_with(submitted_nonce),
              "Retry send re-executes a failed command with a new nonce and local message like the client");
        respond(204);
        message_timeline_remove_local(&g_app.message_view.timeline, submitted_nonce);
        edit(utf8_literal("/word")); application_commands_key(key::enter); application_commands_key(key::enter);
        check(application_commands_submit(200), "failing command submits after the busy retry floor");
        respond(204);
        interaction(GatewayClientDispatchKind::interaction_failure, 0, submitted_nonce);
        echo = only_echo();
        check(echo && echo->delivery == MessageDeliveryState::failed &&
              utf8_equal(echo_text(*echo), utf8_literal("This interaction failed")),
              "a quick INTERACTION_FAILURE fails the local message");
        message_timeline_remove_local(&g_app.message_view.timeline, echo ? echo->id : 0);
        edit(utf8_literal("/word")); application_commands_key(key::enter); application_commands_key(key::enter);
        check(application_commands_submit(201) && only_echo(), "modal command submits");
        respond(204);
        interaction(GatewayClientDispatchKind::interaction_modal_create, 901, submitted_nonce);
        check(!only_echo(), "a modal answering the command removes its local message");

        ApplicationRootCommand roots[8]{};
        bool roots_loading = true;
        const u32 root_count = application_commands_for_application(7, 10, {roots, 8}, &roots_loading);
        bool admin_nested = false;
        for (u32 i = 0; i < root_count; ++i)
            admin_nested |= roots[i].command_id == 103 && roots[i].subcommands && !roots[i].description.bytes;
        check(root_count == 7 && !roots_loading && roots[0].command_id == 100 &&
                  utf8_equal(roots[0].name, utf8_literal("wordle")) &&
                  utf8_equal(roots[0].description, utf8_literal("Play wordle")) && !roots[0].subcommands &&
                  admin_nested && !application_commands_for_application(8, 10, {roots, 8}, &roots_loading),
              "profile commands list one app's root commands for the active channel only");
        check(application_commands_catalog_revision() != 0, "loaded indexes advance the catalog revision");
        check(application_commands_insert(10, 100, utf8_literal("wordle")) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("/wordle ")) &&
                  g_app.message_composer_scene.command.name_bytes == 7 && !confirm_request.title.bytes,
              "inserting a profile command into an empty draft selects it by id");
        edit(utf8_literal("written"));
        confirm_opens = true;
        check(application_commands_insert(10, 101, utf8_literal("echo")) &&
                  utf8_equal(confirm_request.title, utf8_literal("Clear message?")) &&
                  utf8_equal(confirm_request.accept, utf8_literal("Clear")) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("written")),
              "inserting over a written draft asks to clear it first");
        check(confirm_request.save && confirm_request.valid(confirm_request.owner) &&
                  confirm_request.save(confirm_request.owner, {}) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("/echo text:\xE2\x81\xA3")) &&
                  g_app.message_composer_scene.command.name_bytes == 5,
              "confirming the clear inserts and selects the command with its required option as a chip");
        reset_value(&confirm_request);
        confirm_opens = false;
        edit({});
        check(application_commands_insert(0, 103, utf8_literal("admin group run")) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("/admin group run count:\xE2\x81\xA3")) &&
                  g_app.message_composer_scene.command.name_bytes == 16,
              "a command mention selects its subcommand by id and full name");
        edit({});
        check(application_commands_insert(0, 103, utf8_literal("admin other")) &&
                  !g_app.message_composer_scene.command.name_bytes,
              "a mention whose name no longer matches the id selects nothing, like the client");
        edit(utf8_literal("/word")); application_commands_key(key::enter);
        popup_scene_close(&g_app.popup_scene); set_popup_owner(PopupOwnerKind::none);
        message_composer_select_all(&composer); refresh_message_composer_ui();
        check(!g_app.popup_scene.active, "dismissed picker stays closed during selection edits");
        check(g_app.message_composer_scene.command.name_bytes == 7 && composer.bytes,
              "Escape-style popup dismissal keeps the selected editable command identity");
        edit(utf8_literal("/wor"));
        check(g_app.popup_scene.active && !g_app.message_composer_scene.command.name_bytes &&
              popup_items_command_picker({g_app.popup_scene.items.data, g_app.popup_scene.item_count}),
              "backspacing through a selected command name restores the picker and clears its decoration");
        application_commands_key(key::enter);
        check(application_commands_close() && !composer.bytes && !g_app.popup_scene.active &&
                  !g_app.message_composer_scene.command.name_bytes &&
                  !message_composer_scene_command_bar_name(g_app.message_composer_scene).bytes,
              "the option bar's close button clears the selected command and restores the empty composer");

        edit(utf8_literal("/echo \t"));
        check(action_named(utf8_literal("/echo")) != 0,
              "trailing whitespace does not hide an exact command from discovery");
        {
            edit(utf8_literal("/pick"));
            const PopupItem* pick_row = nullptr;
            for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
                if (utf8_equal(g_app.popup_scene.items[i].label, utf8_literal("/pick")))
                    pick_row = &g_app.popup_scene.items[i];
            check(pick_row && utf8_equal(pick_row->detail, utf8_literal("Pick\x1FTest\x1Fmode")),
                  "a picker row carries its description, app column and required option summary");
            UiTextCommand row_text[96]{};
            UiTextCommandStream row_stream{};
            UiTextOutput row_output{&row_stream, nullptr};
            ui_text_command_stream_initialize(&row_stream, {row_text, 96});
            bool description = false, app = false, summary = false;
            if (popup_text_presentation_build_output(g_app.popup_scene, kDefaultTheme, &row_output))
                for (u32 i = 0; i < row_stream.count; ++i) {
                    description |= utf8_equal(row_text[i].text, utf8_literal("Pick"));
                    app |= utf8_equal(row_text[i].text, utf8_literal("Test"));
                    summary |= utf8_equal(row_text[i].text, utf8_literal("mode"));
                }
            check(description && app && summary, "picker rows draw the description, app name and options apart");
            edit(utf8_literal("/search"));
            pick_row = nullptr;
            for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
                if (utf8_equal(g_app.popup_scene.items[i].label, utf8_literal("/search")))
                    pick_row = &g_app.popup_scene.items[i];
            check(pick_row && utf8_equal(pick_row->detail, utf8_literal("Search\x1FTest\x1Fquery\x1E+1 optional")),
                  "optional options collapse into the client's +N optional");
        }
        edit(utf8_literal("/"));
        edit(utf8_literal("/echo text:keep \"quoted\" value"));
        const u32 echo_action = action_named(utf8_literal("/echo"));
        check(echo_action && application_commands_activate(echo_action) &&
              utf8_equal(message_composer_text(composer), utf8_literal("/echo text:keep \"quoted\" value\xE2\x81\xA3")),
              "selecting an exact typed command preserves its existing named arguments");
        {
            refresh_message_composer_ui();
            // The option is a chip: its blank and name project to the
            // chip's head and the value runs to its end marker.
            const auto& scene = g_app.message_composer_scene;
            bool head = false;
            for (u32 i = 0; i < scene.projection_span_count; ++i)
                head |= scene.projection_spans[i].kind == MessageComposerProjectionKind::command_option &&
                        scene.projection_spans[i].source_first == 5 && scene.projection_spans[i].source_last == 11 &&
                        (scene.projection_spans[i].target & message_composer_chip_active);
            MessageComposerChipRange chips[2]{};
            check(head && message_composer_scene_chip_ranges(scene, {chips, 2}) == 1 && !chips[0].open_end &&
                      chips[0].first < chips[0].key_last && chips[0].key_last < chips[0].value_last &&
                      (chips[0].flags & message_composer_chip_active),
                  "the selected command's typed option draws as the client's active option chip");
        }
        check(application_commands_key(key::enter) && application_commands_submit(1000) && operation == 2 &&
              payload_contains(utf8_literal("\"value\":\"keep \\\"quoted\\\" value\"")),
              "typed values are sent as typed, quotes included, like the client's pill text");
        respond(204);

        edit(utf8_literal("/pick"));
        application_commands_key(key::enter);
        // A chosen command is set up with its required options as
        // empty chips, the caret in the first: its choices list right away.
        check(utf8_equal(message_composer_text(composer), utf8_literal("/pick mode:\xE2\x81\xA3")) &&
              composer.caret == composer.bytes - 3 && g_app.popup_scene.item_count == 3 &&
              (g_app.popup_scene.items[0].flags & popup_item_command_section) &&
              utf8_equal(g_app.popup_scene.items[0].label, utf8_literal("OPTIONS")) &&
              !(g_app.popup_scene.items[1].flags & popup_item_command_row) &&
              utf8_equal(message_composer_scene_command_bar_name(g_app.message_composer_scene), utf8_literal("mode")) &&
              ui_rect_height(g_app.popup_scene.rectangle) == kCommandTitleHeight + 2 * kCommandGenericRowHeight +
                                                                    kCommandListInset,
              "a chosen command's required option becomes the active chip with its choices under the option bar");
        const u32 choice_action = action_named(utf8_literal("Two"));
        check(choice_action && application_commands_activate(choice_action) &&
              utf8_equal(message_composer_text(composer), utf8_literal("/pick mode:Two\xE2\x81\xA3")) &&
              composer.caret == composer.bytes,
              "static choice shows its name in the draft like the official client");
        check(application_commands_key(key::enter) && application_commands_submit(1001) &&
              payload_contains(utf8_literal("\"type\":4,\"value\":2")),
              "chosen numeric choice name executes as its JSON number value");
        respond(204);
        check(application_commands_activate(choice_action) && !composer.bytes,
              "a retired choice action cannot dereference or edit a completed command");

        edit(utf8_literal("/upload"));
        application_commands_key(key::enter);
        const u32 file_option = action_named(utf8_literal("Drag and drop or click to upload file"));
        const u32 opens = picker_opens;
        check(file_option && application_commands_activate(file_option) && picker_opens == opens + 1 &&
                  utf8_equal(message_composer_text(composer), utf8_literal("/upload file:\xE2\x81\xA3")),
              "an attachment chip asks for its file like the client's upload area");
        const u32 before_upload = calls;
        check(application_commands_key(key::enter) && composer.bytes && calls == before_upload &&
                  !application_commands_pending(1100),
              "a required attachment option without a staged upload never sends");
        staged_uploads[0] = {utf8_literal("a.png"), utf8_literal("up/a.png"), {}, 0, false};
        staged_uploads[1] = {utf8_literal("b.txt"), utf8_literal("up/b.txt"), {}, 1, false};
        staged_upload_count = 2;
        edit(utf8_literal("/upload file:b.txt"));
        check(application_commands_key(key::enter) && !composer.bytes && application_commands_submit(1101) &&
                  operation == 2 && payload_contains(utf8_literal("\"name\":\"file\",\"type\":11,\"value\":0")) &&
                  payload_contains(utf8_literal(
                      R"(,"attachments":[{"id":"0","filename":"b.txt","uploaded_filename":"up/b.txt"}]})")) &&
                  uploads_queued_nonce == submitted_nonce,
              "an attachment option sends its named staged upload as data.attachments index 0");
        const u32 completed = uploads_completed;
        respond(204);
        check(uploads_completed == completed + 1, "a command's uploads are spent with its response");
        interaction(GatewayClientDispatchKind::interaction_failure, 0, submitted_nonce);
        check(application_commands_retry(submitted_nonce) && !application_commands_pending(1102),
              "a failed command with files offers no resend, like the client");
        staged_upload_count = 0;
        message_timeline_remove_local(&g_app.message_view.timeline, submitted_nonce);

        edit(utf8_literal("/admin group run count:4"));
        const u32 nested_action = action_named(utf8_literal("/admin group run"));
        check(nested_action && application_commands_activate(nested_action) &&
              application_commands_key(key::enter) && application_commands_submit(1002) &&
              payload_contains(utf8_literal("\"type\":2,\"name\":\"group\",\"options\":[{\"type\":1,\"name\":\"run\"")),
              "flattened subcommand selection preserves arguments and serializes its nested wire scope");
        respond(204);

        edit(utf8_literal("/bump"));
        char second_picture[160]{};
        const u32 second_app_action = action_named(utf8_literal("/bump"), 1);
        check(second_app_action && application_commands_activate(second_app_action) &&
              utf8_equal(picture_url(g_app.message_composer_scene.command.avatar, second_picture),
                         utf8_literal("https://cdn.discordapp.com/app-icons/20/0f1e2d.png?size=160")) &&
              application_commands_key(key::enter) && application_commands_submit(1003) &&
              submitted_application == 20 && payload_contains(utf8_literal("\"id\":\"105\"")),
              "duplicate command labels retain the selected application and command identity");
        respond(204);

        check(application_commands_tick(1) && application_commands_next_wake() != 1,
              "sent commands' local messages repaint before the next picker");
        {
            // The end marker ends the chip: "name:" typed inside a
            // value stays text, Tab leaves the chip, a key typed after it makes
            // the next chip, and Backspace after a chip deletes its last character.
            edit(utf8_literal("/search"));
            application_commands_key(key::enter);
            const bool set_up = utf8_equal(message_composer_text(composer), utf8_literal("/search query:\xE2\x81\xA3")) &&
                                composer.caret == 14;
            composer_apply_result(message_composer_insert_utf8(&composer, utf8_literal("a count:")), true);
            MessageComposerChipRange chips[3]{};
            check(set_up &&
                      utf8_equal(message_composer_text(composer),
                                 utf8_literal("/search query:a count:\xE2\x81\xA3")) &&
                      composer.caret == 22 &&
                      message_composer_scene_chip_ranges(g_app.message_composer_scene, {chips, 3}) == 1,
                  "an option name typed inside a chip's value stays part of that value");
            check(application_commands_option_tab(false) && composer.caret == 25 &&
                      composer_apply_result(message_composer_insert_utf8(&composer, utf8_literal(" count:")), true) &&
                      utf8_equal(message_composer_text(composer),
                                 utf8_literal("/search query:a count:\xE2\x81\xA3 count:\xE2\x81\xA3")) &&
                      composer.caret == composer.bytes - 3 &&
                      message_composer_scene_chip_ranges(g_app.message_composer_scene, {chips, 3}) == 2,
                  "after the chip's end a typed option name starts the next chip with the caret in it");
            message_composer_set_selection(&composer, 25, 25);
            application_commands_prepare_delete(false);
            check(composer_apply_result(message_composer_backspace(&composer), true) &&
                      utf8_equal(message_composer_text(composer),
                                 utf8_literal("/search query:a count\xE2\x81\xA3 count:\xE2\x81\xA3")) &&
                      composer.caret == 21,
                  "Backspace right after a chip deletes its value's last character, like the client");
            // Back in the autocomplete option: five Loading rows
            // under the title while the suggestions are on their way.
            check(g_app.popup_scene.active && g_app.popup_scene.item_count == 6 &&
                      (g_app.popup_scene.items[0].flags & popup_item_command_section) &&
                      popup_item_placeholder(g_app.popup_scene.items[1]) &&
                      popup_item_placeholder(g_app.popup_scene.items[5]) &&
                      ui_rect_height(g_app.popup_scene.rectangle) ==
                          kCommandTitleHeight + 5 * kCommandGenericLoadingHeight + kCommandListInset,
                  "a loading autocomplete option lists the client's placeholder rows");
            edit({});
        }
        edit(utf8_literal("/search"));
        application_commands_key(key::enter);
        edit(utf8_literal("/search query:old count:2"));
        // Shift+Tab selects the previous chip's value; Tab the next one, then the end.
        // Typed options get end markers: "/search query:old⁣ count:2⁣".
        check(composer.caret == 28 && application_commands_option_tab(true) && composer.anchor == 14 &&
                  composer.caret == 17 && application_commands_option_tab(false) && composer.anchor == 27 &&
                  composer.caret == 28 && application_commands_option_tab(false) && composer.anchor == 31 &&
                  composer.caret == 31 && application_commands_option_tab(true) && composer.anchor == 27 &&
                  composer.caret == 28 && application_commands_option_tab(true) && composer.anchor == 14 &&
                  composer.caret == 17,
              "Tab and Shift+Tab move between the option chips like the client");
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
              utf8_equal(message_composer_text(composer),
                         utf8_literal("/search query:Picked\xE2\x81\xA3 count:2\xE2\x81\xA3")) &&
              composer.anchor == 30 && composer.caret == 31,
              "a suggestion's name replaces only its value, then the next chip is selected");
        check(application_commands_key(key::enter) && application_commands_submit(clock_monotonic_milliseconds()) &&
              operation == 2 && payload_contains(utf8_literal("\"value\":\"picked value\"")),
              "accepted dynamic choice is execution data rather than another autocomplete request");
        respond(204);

        edit(utf8_literal("/echo")); application_commands_key(key::enter);
        const u32 unchanged = calls;
        check(utf8_equal(message_composer_text(composer), utf8_literal("/echo text:\xE2\x81\xA3")) &&
                  application_commands_key(key::enter) && composer.bytes && calls == unchanged &&
                  g_app.message_composer_scene.command_bar_error &&
                  utf8_equal(message_composer_scene_command_bar_name(g_app.message_composer_scene),
                             utf8_literal("text")) &&
                  utf8_equal(message_composer_scene_command_bar_detail(g_app.message_composer_scene),
                             utf8_literal("This option is required. Specify a value.")),
              "an empty required option stops the send and the option bar shows the client's message");
        {
            // The refused send shakes the app: a move on every other frame for
            // 200 ms, then the content rests again.
            const u64 shake_at = clock_monotonic_milliseconds();
            const u32 moves = shake_offsets, rests = shake_rests;
            check(application_commands_next_wake() && application_commands_tick(shake_at) &&
                      shake_offsets == moves && application_commands_tick(shake_at + 16) &&
                      shake_offsets == moves + 1 && application_commands_tick(shake_at + 32) &&
                      shake_offsets == moves + 1 && application_commands_tick(shake_at + 400) &&
                      shake_rests == rests + 1 && application_commands_tick(shake_at + 416) &&
                      shake_rests == rests + 1,
                  "a refused send shakes the app like the client, then lets it rest");
        }
        edit(utf8_literal("/echo text:hi"));
        check(!g_app.message_composer_scene.command_bar_error &&
                  utf8_equal(message_composer_scene_command_bar_name(g_app.message_composer_scene),
                             utf8_literal("text")),
              "a failed option is checked again as its value changes");

        // Frequently Used: settings-proto/2 command frecency, read and saved.
        {
            static CommandFrecencyEntry seeded[2];
            u32 seeded_count = 0;
            const u64 today = clock_unix_milliseconds();
            for (u32 use = 0; use < 3; ++use)
                command_frecency_track({seeded, 2}, &seeded_count, utf8_literal("101"), today - use);
            seeded_count = command_frecency_compute({seeded, 2}, seeded_count, today);
            static u8 proto_scratch[4096], settings_body[8192];
            const u32 body_bytes = command_frecency_write_patch({seeded, seeded_count}, 3,
                                                                {proto_scratch, sizeof(proto_scratch)},
                                                                {settings_body, sizeof(settings_body)});
            const Utf8View settings{reinterpret_cast<const char*>(settings_body), body_bytes};
            g_app.account.status.running = true;
            g_app.account.status.frecency_settings_revision = 4;
            edit(utf8_literal("/"));
            check(body_bytes && application_commands_pending(5000) && application_commands_submit(5000) &&
                      operation == 4,
                  "an open picker fetches the account's command frecency from settings-proto/2");
            respond(200, settings);
            u32 first_row = 0;
            while (first_row < g_app.popup_scene.item_count &&
                   !(g_app.popup_scene.items[first_row].flags & popup_item_command_row))
                ++first_row;
            check(first_row < g_app.popup_scene.item_count &&
                      utf8_equal(g_app.popup_scene.items[first_row].label, utf8_literal("/wordle")) &&
                      utf8_equal(g_app.popup_scene.items[first_row + 1].label, utf8_literal("/echo")) &&
                      first_row && (g_app.popup_scene.items[first_row - 1].flags & popup_item_command_section),
                  "Frequently Used leads the picker by frecency score, replaying uses not saved yet");
            check(application_commands_tick(1), "pending local-message repaints drain first");
            const u64 save = application_commands_next_wake();
            check(save > 1, "frecency save: wake");
            check(!application_commands_pending(save - 1), "frecency save: not early");
            check(application_commands_submit(save) && operation == 5, "frecency save: op");
            check(command_frecency_patch_valid({payload, payload_bytes}), "frecency save: valid");
            check(payload_contains(utf8_literal("\"required_data_version\":0")),
                  "tracked uses are saved a few seconds later as application_command_frecency only");
            // The server answers a save with the stored settings: the saved map.
            static char saved[96 * 1024];
            copy_bytes(saved, payload, payload_bytes);
            respond(200, Utf8View{saved, payload_bytes});
            check(!application_commands_pending(save + 60000) && application_commands_next_wake() != save,
                  "a confirmed save leaves no pending frecency work");
            {
                const auto rail_item = [](Utf8View label) noexcept -> const PopupItem* {
                    for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
                        if ((g_app.popup_scene.items[i].flags & popup_item_command_category) &&
                            utf8_equal(g_app.popup_scene.items[i].label, label))
                            return &g_app.popup_scene.items[i];
                    return nullptr;
                };
                const auto section = [](Utf8View label) noexcept {
                    for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
                        if ((g_app.popup_scene.items[i].flags & popup_item_command_section) &&
                            utf8_equal(g_app.popup_scene.items[i].label, label))
                            return true;
                    return false;
                };
                const PopupItem* frequent_rail = rail_item(utf8_literal("Frequently Used"));
                check(frequent_rail && ui_menu_row_icon(frequent_rail->presentation) == UiIcon::recent &&
                          (frequent_rail->flags & popup_item_checked) && section(utf8_literal("Frequently Used")) &&
                          rail_item(utf8_literal("Test")) && !rail_item(utf8_literal("All")),
                      "the rail leads with the clock, highlights the section in view and has no All entry");
                {
                    // The icon order without a bot avatar: the app's
                    // icon (app-icons), else the bot's CDN default avatar,
                    // both fetched by URL.
                    const auto url_of = [](const PopupItem* app, char (&url)[160]) noexcept {
                        return app && (app->flags & popup_item_avatar) && (app->media_flags & kPopupItemMediaUrl)
                                   ? Utf8View{url, application_commands_icon_url(app->media_id, {url, sizeof(url)})}
                                   : Utf8View{};
                    };
                    char test_url[160]{}, other_url[160]{};
                    check(utf8_equal(url_of(rail_item(utf8_literal("Test")), test_url),
                                     utf8_literal("https://cdn.discordapp.com/embed/avatars/0.png")) &&
                              utf8_equal(url_of(rail_item(utf8_literal("Other")), other_url),
                                         utf8_literal("https://cdn.discordapp.com/app-icons/20/0f1e2d.png?size=160")),
                          "an app without a bot avatar shows its icon, else its bot's default avatar");
                }
                const PopupItem* app_rail = rail_item(utf8_literal("Test"));
                const u32 app_action = app_rail ? app_rail->action : 0;
                check(app_action && application_commands_activate(app_action) && !section(utf8_literal("Frequently Used")) &&
                          section(utf8_literal("Test")) && !section(utf8_literal("Other")) &&
                          action_named(utf8_literal("/wordle")) && rail_item(utf8_literal("Test")) &&
                          (rail_item(utf8_literal("Test"))->flags & popup_item_checked),
                      "a rail entry shows its section alone, Frequently Used commands included, and stays highlighted");
                check(app_action && application_commands_activate(app_action) &&
                          section(utf8_literal("Frequently Used")) && section(utf8_literal("Other")),
                      "clicking the filtered entry again lists every section");
                check(application_commands_activate(app_action) &&
                          application_commands_activate(action_named(utf8_literal("Frequently Used"))) &&
                          section(utf8_literal("Frequently Used")) && section(utf8_literal("Other")),
                      "the clock clears a filter and returns to the top, as in the client");
                // MaiCord's rail keys: Left enters the rail at the highlighted
                // section, Up/Down move, Enter clicks, Right returns to the list.
                const auto selected_item = [] noexcept -> const PopupItem* {
                    const u32 n = g_app.popup_scene.selected_index;
                    return n < g_app.popup_scene.item_count ? &g_app.popup_scene.items[n] : nullptr;
                };
                check(application_commands_key(key::left) && selected_item() &&
                          (selected_item()->flags & popup_item_command_category) &&
                          utf8_equal(selected_item()->label, utf8_literal("Frequently Used")),
                      "Left moves the keyboard into the rail at the highlighted section");
                check(application_commands_key(key::down) && selected_item() &&
                          utf8_equal(selected_item()->label, utf8_literal("Other")) && application_commands_key(key::up) &&
                          application_commands_key(key::up) && utf8_equal(selected_item()->label,
                                                                         utf8_literal("Frequently Used")),
                      "Up and Down move through the rail and stop at its ends");
                check(application_commands_key(key::down) && application_commands_key(key::down) &&
                          application_commands_key(key::enter) && selected_item() &&
                          utf8_equal(selected_item()->label, utf8_literal("Test")) &&
                          !section(utf8_literal("Frequently Used")) && section(utf8_literal("Test")) &&
                          g_app.composer_menu_held_key == key::enter,
                      "Enter in the rail filters to the focused section and keeps the rail focus");
                g_app.composer_menu_held_key = 0;
                const auto first_row = [] noexcept {
                    for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
                        if ((g_app.popup_scene.items[i].flags & popup_item_command_row) &&
                            (g_app.popup_scene.items[i].flags & popup_item_enabled))
                            return g_app.popup_scene.items[i].action;
                    return 0u;
                };
                check(application_commands_key(key::right) && first_row() &&
                          popup_scene_selected_action(g_app.popup_scene) == first_row(),
                      "Right returns to the list at the section's first row");
                check(application_commands_key(key::left) && application_commands_key(key::escape) &&
                          popup_scene_selected_action(g_app.popup_scene) == first_row() &&
                          !application_commands_key(key::escape),
                      "Escape leaves the rail first and only then belongs to the popup");
                application_commands_activate(app_action);
            }
            edit(utf8_literal("/wor"));
            check(action_named(utf8_literal("/wordle")) && !action_named(utf8_literal("/wordle"), 1) &&
                      (g_app.popup_scene.items[0].flags & popup_item_command_section) &&
                      utf8_equal(g_app.popup_scene.items[0].label, utf8_literal("COMMANDS MATCHING /WOR")),
                  "a query lists ranked results under the client's title without the rail or Frequently Used");
            edit(utf8_literal("/zzzz"));
            check(!g_app.popup_scene.active, "a query matching nothing hides the results, as in the client");
            // Built-ins count too (keyed by the negative id): a used /shrug joins Frequently Used.
            // Five uses outrank the fixture's earlier commands.
            for (u32 use = 0; use < 5; ++use) {
                edit(utf8_literal("/shrug"));
                bool shrug_handled = false;
                check(application_commands_key(key::enter) && application_commands_send(&shrug_handled),
                      "built-in use: send");
            }
            edit(utf8_literal("/"));
            check(action_named(utf8_literal("/shrug")) != 0,
                  "a used built-in command is listed under Frequently Used");
            g_app.account.status.running = false;
        }

        builtin_cases();
        plugin_cases();
        guild_picture_cases();

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
    if (ready) {
        composer_slowmode_cases();
        test_invite_retry(check);
    }
    application_commands_release(); popup_scene_release(&g_app.popup_scene);
    message_composer_scene_release(&g_app.message_composer_scene); message_composer_release(&composer);
    g_app.account_ui = nullptr; arena_release(&arena);
}

} // namespace
} // namespace maicord

extern "C" void maicord_command_ui_tests_entry() noexcept {
    maicord::run();
    maicord::app_detail::test_activity_retry(maicord::check);
    maicord::app_detail::test_custom_status_retry(maicord::check);
    maicord::app_detail::test_notification_retry(maicord::check);
    maicord::app_detail::test_community_retry(maicord::check);
    ExitProcess(maicord::failures ? 1 : 0);
}

