#include "../src/app/application_commands.hpp"
#include "../src/app/application_state.hpp"
#include "../src/app/application_plugins.hpp"
#include "../src/app/application_message_feedback.hpp"
#include "../src/app/application_text_form.hpp"
#include "../src/app/application_attachments.hpp"
#include "maicord/protocol/attachment_upload.hpp"
#include "maicord/protocol/application_commands.hpp"
#include "maicord/app/message_composer_controller.hpp"
#include "maicord/app/message_send_response.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/writer.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/model/permission_bits.hpp"
#include "../src/app/account_ui_runtime.hpp"
#include "../src/app/application_builtin_commands.hpp"
#include "../src/app/application_media_picker.hpp"
#include "../src/app/application_private_channel.hpp"
#include "../src/app/application_private_conversation.hpp"
#include "../src/app/application_thread_actions.hpp"
#include "maicord/protocol/private_conversation.hpp"
#include "maicord/protocol/guild_admin.hpp"
#include "maicord/ui/media_picker.hpp"

namespace maicord {
// The app shake moves the composition visual; the fixture counts the moves.
u32 shake_offsets, shake_rests;
HRESULT d3d11_device_owner_set_offset(D3D11DeviceOwner*, float x, float y) noexcept {
    ++(x == 0 && y == 0 ? shake_rests : shake_offsets);
    return 0;
}
} // namespace maicord

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
bool reveal_command_row(float, float, bool) noexcept { return true; }
// No plugins are loaded in this fixture.
u32 application_plugins_commands(Utf8View, Slice<NativePluginSdkCommandView>) noexcept { return 0; }
bool application_plugins_command_pending() noexcept { return false; }
void application_plugins_menu_dismissed() noexcept {}
bool queue_message_send() noexcept { return true; }
// Slowmode tests leave reaction, edit, history and connection UI effects idle.
bool queue_message_reaction_emoji(u64, u64, Utf8View, bool) noexcept { return false; }
u64 application_message_edit_latest_own() noexcept { return 0; }
bool queue_message_edit_content(u64, Utf8View) noexcept { return false; }
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
u32 searched_bytes, searches, inline_closes;
bool application_media_picker_inline(UiMediaPickerKind kind, Utf8View query, UiRect) noexcept {
    searched_kind = kind;
    searched_bytes = utf8_copy_truncated(searched, query);
    ++searches;
    return true;
}
void application_media_picker_inline_close() noexcept {
    ++inline_closes;
}
bool application_media_picker_inline_key(u32, bool* handled) noexcept {
    *handled = false;
    return true;
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
namespace {
using namespace app_detail;
u32 failures, calls, operation;
char payload[65536];
u32 payload_bytes;
u64 submitted_channel, submitted_guild, submitted_application, submitted_nonce, submitted_request;
RestClientSubmitError submit_error;
void check(bool ok, const char* label) noexcept {
    test_check(&failures, ok, label);
}
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
constexpr auto fixture_index = utf8_literal(R"({"applications":[{"id":"10","name":"Test","bot":{"id":"11"}},{"id":"20","name":"Other","icon":"0f1e2d","bot":{"id":"21"}}],"application_commands":[{"id":"100","application_id":"10","version":"7","name":"wordle","description":"Play wordle","type":1},{"id":"101","application_id":"10","version":"7","name":"echo","description":"Echo","type":1,"options":[{"name":"text","type":3,"required":true}]},{"id":"102","application_id":"10","version":"7","name":"pick","description":"Pick","type":1,"options":[{"name":"mode","type":4,"required":true,"choices":[{"name":"One","value":1},{"name":"Two","value":2}]}]},{"id":"103","application_id":"10","version":"7","name":"admin","description":"Admin","type":1,"options":[{"name":"group","type":2,"options":[{"name":"run","description":"Run","type":1,"options":[{"name":"count","type":4,"required":true}]}]}]},{"id":"104","application_id":"10","version":"7","name":"bump","description":"First bump","type":1},{"id":"105","application_id":"20","version":"7","name":"bump","description":"Second bump","type":1},{"id":"106","application_id":"10","version":"7","name":"search","description":"Search","type":1,"options":[{"name":"query","type":3,"required":true,"autocomplete":true},{"name":"count","type":4}]},{"id":"107","application_id":"10","version":"7","name":"upload","description":"Upload","type":1,"options":[{"name":"file","type":11,"required":true},{"name":"extra","type":11}]}]})");
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
void command_index_invalidation_cases() noexcept {
    auto& ui = *g_app.account_ui;
    auto& composer = g_app.message_composer;
    const auto saved_state = ui.chat_state;
    const u64 saved_active_channel = ui.active_channel_id;
    const u64 saved_composer_channel = composer.channel_id;
    GuildRecord guilds[1]{};
    ChannelRecord channels[1]{};
    u16 guild_order[1]{}, channel_order[1]{};
    u8 names[128]{};
    ChannelInput channel{};
    channel.id = 9;
    channel.guild_id = 55;
    channel.name = utf8_literal("commands");
    application_commands_release();
    const u32 unbound_calls = calls;
    application_commands_invalidate_guild(55);
    check(!application_commands_next_wake() && calls == unbound_calls,
          "command invalidation leaves an unbound catalog idle");
    const bool initialized =
        chat_state_store_initialize(&ui.chat_state, {guilds, 1}, {guild_order, 1}, {channels, 1},
                                    {channel_order, 1}, {names, sizeof(names)}) &&
        chat_state_store_upsert_guild(&ui.chat_state,
                                      {55, utf8_literal("Guild"), GuildMutationKind::create, 1})
            .ok() &&
        chat_state_store_upsert_channel(&ui.chat_state, channel).ok() &&
        message_composer_controller_select_channel(&ui.message_composer_controller, 9).ok();
    check(initialized, "command invalidation guild fixture initializes");
    if (initialized) {
        ui.active_channel_id = 9;
        constexpr auto server = utf8_literal(
            R"({"applications":[{"id":"10","name":"Guild app"}],"application_commands":[{"id":"100","application_id":"10","version":"2","name":"guild","description":"Guild command","type":1},{"id":"102","application_id":"10","version":"2","name":"Inspect user","type":2}]})");
        constexpr auto stale = utf8_literal(
            R"({"applications":[{"id":"10","name":"Guild app"}],"application_commands":[{"id":"101","application_id":"10","version":"1","name":"stale","description":"Old command","type":1}]})");
        constexpr auto user = utf8_literal(
            R"({"applications":[{"id":"20","name":"User app"}],"application_commands":[{"id":"200","application_id":"20","version":"8","name":"personal","description":"User command","type":1}]})");
        const u32 initial_calls = calls;
        edit(utf8_literal("/"));
        check(application_commands_submit(1) && operation == 0, "command invalidation loads guild index");
        respond(200, server);
        edit(utf8_literal("/guild"));
        check(application_commands_key(VK_RETURN) && g_app.message_composer_scene.command.name_bytes == 6 &&
                  application_commands_submit(2) && operation == 1,
              "command invalidation retains selected guild command while the independent user index loads");
        const u32 before_event = calls;
        application_commands_invalidate_guild(0);
        application_commands_invalidate_guild(66);
        check(
            application_commands_tick(1) && calls == before_event &&
                g_app.message_composer_scene.command.name_bytes == 6 &&
                utf8_equal(message_composer_text(composer), utf8_literal("/guild ")),
            "command invalidation ignores zero and unrelated guilds without disturbing selection or draft");
        application_commands_invalidate_guild(55);
        check(application_commands_next_wake() == 1 && calls == before_event &&
                  application_commands_tick(1) && !g_app.message_composer_scene.command.name_bytes &&
                  g_app.popup_scene.active &&
                  utf8_equal(message_composer_text(composer), utf8_literal("/guild ")),
              "command invalidation defers projection work and reopens the untouched draft without stale "
              "selection");
        respond(200, user);
        edit(utf8_literal("/"));
        check(action_named(utf8_literal("/personal")) && !action_named(utf8_literal("/guild")),
              "command invalidation preserves the independently arriving user index while removing stale "
              "guild entries");
        constexpr u16 old_statuses[]{200, 202, 429};
        for (u32 i = 0; i < 3; ++i) {
            check(application_commands_submit(10 + i * 4) && operation == 0,
                  "command invalidation admits one guild refresh");
            application_commands_invalidate_guild(55);
            application_commands_invalidate_guild(55);
            check(application_commands_tick(1),
                  "command invalidation coalesces repeated in-flight updates");
            RestClientResponse old{};
            old.status_code = old_statuses[i];
            old.rate_limited = old.status_code == 429;
            old.retry_at_ms = old.rate_limited ? 900000000000ull : 0;
            g_app.details_request_owner = MessageHistoryRequestOwner::none;
            check(application_commands_response(old, stale) && application_commands_pending(12 + i * 4) &&
                      !action_named(utf8_literal("/stale")) && action_named(utf8_literal("/personal")) &&
                      application_commands_submit(12 + i * 4) && operation == 0,
                  "command invalidation fences old success pending and rate responses without losing or "
                  "delaying the refetch");
            respond(200, server);
            check(action_named(utf8_literal("/guild")) && action_named(utf8_literal("/personal")) &&
                      !application_commands_pending(13 + i * 4),
                  "command invalidation installs only the current guild response and never refetches the "
                  "user index");
            if (i != 2) {
                application_commands_invalidate_guild(55);
                check(application_commands_tick(1),
                      "command invalidation prepares the next response fence");
            }
        }
        check(calls == initial_calls + 8,
              "command invalidation refetch counts exclude duplicate events and user-index reloads");
        for (u32 sending = 0; sending < 2; ++sending) {
            edit(utf8_literal("/guild"));
            check(application_commands_key(VK_RETURN) && application_commands_key(VK_RETURN),
                  "command invalidation queues an immutable interaction payload");
            if (sending)
                check(application_commands_submit(30) && operation == 2,
                      "command invalidation sends the interaction before update");
            const u32 submitted_before = calls;
            application_commands_invalidate_guild(55);
            check(application_commands_tick(1) && calls == submitted_before && !composer.bytes,
                  "command invalidation never resubmits a queued or sending command");
            if (!sending)
                check(application_commands_submit(30) && operation == 2,
                      "command invalidation preserves the already serialized queued interaction");
            check(submitted_channel == 9 && submitted_guild == 55 && submitted_application == 10 &&
                      submitted_nonce && payload_contains(utf8_literal("\"id\":\"100\"")) &&
                      payload_contains(utf8_literal("\"version\":\"2\"")),
                  "command invalidation keeps the admitted command identity and version after freeing its "
                  "index");
            edit(utf8_literal("newer draft stays"));
            respond(204);
            check(utf8_equal(message_composer_text(composer), utf8_literal("newer draft stays")) &&
                      !application_commands_pending(31) &&
                      message_composer_controller_select_channel(&ui.message_composer_controller, 8).ok() &&
                      message_composer_controller_select_channel(&ui.message_composer_controller, 9).ok() &&
                      utf8_equal(message_composer_text(composer), utf8_literal("newer draft stays")),
                  "command invalidation preserves a newer retained draft through interaction completion "
                  "and channel switches");
            edit(utf8_literal("/"));
            check(application_commands_submit(32) && operation == 0,
                  "command invalidation refetches the guild only when command discovery is wanted again");
            respond(200, server);
        }
        edit(utf8_literal("retained context draft"));
        ApplicationContextCommand found[1]{};
        bool loading = false;
        const u32 context_count = application_commands_context(2, {found, 1}, &loading);
        PopupItem item{};
        item.label = found[0].name;
        item.flags = popup_item_enabled;
        item.action = 1;
        set_popup_owner(PopupOwnerKind::member);
        check(context_count == 1 && !loading &&
                  popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, 1, {&item, 1}, {0, 0, 1, 1},
                                   command_viewport, 200)
                      .ok(),
              "command invalidation opens an Apps row borrowing the guild index label");
        const u32 context_before = context_changes, requests_before = calls;
        application_commands_invalidate_guild(55);
        check(g_app.popup_scene.active && context_changes == context_before && calls == requests_before &&
                  utf8_equal(g_app.popup_scene.items[0].label, utf8_literal("Inspect user")) &&
                  application_commands_tick(1) && !g_app.popup_scene.active &&
                  context_changes == context_before + 1 && calls == requests_before,
              "command invalidation retains borrowed Apps text until the deferred no-op refresh safely "
              "closes it");
        constexpr u16 waiting_statuses[]{202, 429};
        for (const u16 status : waiting_statuses) {
            edit(utf8_literal("/"));
            check(application_commands_submit(1000) && operation == 0,
                  "command invalidation submits the index before an existing retry wait");
            RestClientResponse response{};
            response.status_code = status;
            response.rate_limited = status == 429;
            response.retry_at_ms = response.rate_limited ? 900000000000ull : 0;
            g_app.details_request_owner = MessageHistoryRequestOwner::none;
            check(application_commands_response(response, {}),
                  "command invalidation installs the existing index retry wait");
            application_commands_invalidate_guild(55);
            check(application_commands_tick(1) && application_commands_pending(1002) == (status == 202) &&
                      (status != 429 || application_commands_next_wake() == response.retry_at_ms) &&
                      application_commands_submit(status == 429 ? response.retry_at_ms : 1002) &&
                      operation == 0,
                  "command invalidation replaces obsolete local 202 waiting while retaining the server 429 "
                  "floor");
            respond(200, server);
            if (status == 202) {
                application_commands_invalidate_guild(55);
                check(application_commands_tick(1), "command invalidation prepares the server-floor case");
            }
        }
        ui.active_channel_id = 8;
        check(message_composer_controller_select_channel(&ui.message_composer_controller, 8).ok(),
              "command invalidation binds the direct-message channel");
        edit(utf8_literal("/"));
        check(application_commands_submit(2000) && operation == 0 && submitted_channel == 8 &&
                  !submitted_guild,
              "command invalidation loads the direct-message channel index separately from the user index");
        respond(200, server);
        edit(utf8_literal("direct-message draft"));
        const u32 dm_calls = calls;
        application_commands_invalidate_guild(55);
        application_commands_invalidate_guild(66);
        check(application_commands_tick(1) && calls == dm_calls && !application_commands_pending(2001) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("direct-message draft")),
              "command invalidation coalesces guild hints for a bound DM catalog without fetching behind a "
              "closed picker");
        edit(utf8_literal("/"));
        check(action_named(utf8_literal("/personal")) && !action_named(utf8_literal("/guild")) &&
                  application_commands_submit(2002) && operation == 0 && submitted_channel == 8 &&
                  !submitted_guild && calls == dm_calls + 1,
              "command invalidation refreshes the DM channel once on demand while preserving the "
              "independent user index");
        respond(200, server);
    }
    application_commands_release();
    popup_scene_close(&g_app.popup_scene);
    set_popup_owner(PopupOwnerKind::none);
    ui.chat_state = saved_state;
    ui.active_channel_id = saved_active_channel;
    message_composer_controller_select_channel(&ui.message_composer_controller, saved_composer_channel);
    edit({});
}
void composer_slowmode_cases() noexcept {
    auto& ui = *g_app.account_ui;
    const auto saved_state = ui.chat_state;
    const auto saved_instance = g_app.account.instance;
    const u64 saved_active_channel = ui.active_channel_id;
    const u64 saved_composer_channel = g_app.message_composer.channel_id;
    GuildRecord guilds[1]{};
    ChannelRecord channels[1]{};
    RoleRecord roles[1]{};
    PermissionOverwriteRecord overwrites[1]{};
    u16 guild_order[1]{}, channel_order[1]{};
    u8 names[256]{};
    u32 account_owners[8]{};
    constexpr u64 guild_id = 55, channel_id = 9, self_id = 7;
    constexpr PermissionSet ordinary = kPermissionViewChannel | kPermissionSendMessages;
    constexpr Utf8View draft = utf8_literal("slowmode draft stays");
    ChannelInput channel_input{};
    channel_input.id = channel_id;
    channel_input.guild_id = guild_id;
    channel_input.name = utf8_literal("slowmode");
    channel_input.rate_limit_per_user = 21'600;
    channel_input.rate_limit_known = true;
    const bool initialized = chat_state_store_initialize(&ui.chat_state, {guilds, 1}, {guild_order, 1},
        {channels, 1}, {channel_order, 1}, {names, sizeof(names)}) &&
        chat_state_store_bind_permissions(&ui.chat_state, {roles, 1}, {overwrites, 1}) &&
        chat_state_store_upsert_guild(&ui.chat_state, {guild_id, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok() &&
        chat_state_store_upsert_channel(&ui.chat_state, channel_input).ok() &&
        message_composer_controller_select_channel(&ui.message_composer_controller, channel_id).ok() &&
        message_composer_replace_range(&g_app.message_composer, 0, g_app.message_composer.bytes, draft) !=
            MessageComposerResult::invalid_input;
    check(initialized, "slowmode production feedback fixture initializes");
    if (initialized) {
        ui.active_channel_id = channel_id;
        const auto permissions = [&](PermissionSet bits, bool owner = false) noexcept {
            const bool updated = chat_state_store_upsert_role(&ui.chat_state,
                {guild_id, guild_id, ordinary | bits, {}}) &&
                chat_state_store_set_self_roles(&ui.chat_state, guild_id, owner ? self_id : 1, self_id, {});
            chat_state_store_recompute_guild_permissions(&ui.chat_state, guild_id);
            return updated;
        };
        struct Case { PermissionSet bits; bool resolved, owner, blocked; const char* label; };
        constexpr Case cases[] = {
            {kPermissionBypassSlowmode, true, false, false, "slowmode dedicated bypass alone does not arm or block the draft"},
            {kPermissionManageMessages, true, false, true, "slowmode Manage Messages alone no longer bypasses the countdown"},
            {kPermissionManageChannels, true, false, true, "slowmode Manage Channels alone no longer bypasses the countdown"},
            {kPermissionManageThreads, true, false, true, "slowmode Manage Threads alone no longer bypasses the countdown"},
            {kPermissionBypassSlowmode, false, false, true, "slowmode unresolved permission bits cannot exempt the draft"},
            {kPermissionAdministrator, true, false, false, "slowmode administrator effective-all remains exempt"},
            {0, true, true, false, "slowmode owner effective-all remains exempt"},
        };
        for (u32 i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            const auto& test = cases[i];
            g_app.account.instance = &account_owners[i];
            check(permissions(test.bits, test.owner), "slowmode effective permissions project");
            channels[0].permissions_resolved = test.resolved;
            application_composer_note_sent(channel_id);
            bool handled = false;
            const bool active = application_composer_slowmode_active(channel_id);
            check(application_composer_prepare_send(&handled) && active == test.blocked && handled == test.blocked &&
                utf8_equal(message_composer_text(g_app.message_composer), draft) &&
                (application_composer_status_notice().bytes != 0) == test.blocked, test.label);
        }
        g_app.account.instance = &account_owners[7];
        check(permissions(kPermissionBypassSlowmode) &&
            chat_state_store_append_channel_overwrite(&ui.chat_state, channel_id,
                {guild_id, 0, kPermissionBypassSlowmode, 0}), "slowmode channel deny fixture initializes");
        chat_state_store_recompute_guild_permissions(&ui.chat_state, guild_id);
        application_composer_note_sent(channel_id);
        bool handled = false;
        check(!(channels[0].effective_permissions & kPermissionBypassSlowmode) &&
            application_composer_slowmode_active(channel_id) && application_composer_prepare_send(&handled) && handled &&
            utf8_equal(message_composer_text(g_app.message_composer), draft),
            "slowmode channel overwrite denying bypass retains countdown and draft");
        chat_state_store_clear_channel_overwrites(&ui.chat_state, channel_id);
        check(permissions(kPermissionBypassSlowmode) && !application_composer_status_notice().bytes &&
            !application_composer_slowmode_active(channel_id), "slowmode live grant immediately retires the predicted countdown");
        handled = true;
        check(application_composer_prepare_send(&handled) && !handled && permissions(0) &&
            !application_composer_slowmode_active(channel_id) && utf8_equal(message_composer_text(g_app.message_composer), draft),
            "slowmode revoking a grant does not resurrect the retired prediction or consume the draft");
        application_composer_note_sent(channel_id);
        check(application_composer_slowmode_active(channel_id), "slowmode ordinary send rearms the prediction");
        const u64 policy_now = clock_monotonic_milliseconds();
        channels[0].slowmode_code = slowmode_encode(60);
        application_message_interaction_tick(policy_now);
        check(application_message_interaction_next_wake(policy_now + 59'999) == policy_now + 60'000,
            "slowmode nonzero policy decrease caps the running prediction to the new duration");
        channels[0].slowmode_code = slowmode_encode(21'600);
        application_message_interaction_tick(policy_now + 1000);
        check(application_message_interaction_next_wake(policy_now + 59'999) == policy_now + 60'000,
            "slowmode policy increase preserves the earlier predicted deadline");
        ui.active_channel_id = 0;
        channels[0].slowmode_code = slowmode_encode(30);
        application_message_interaction_tick(policy_now + 2000);
        check(!application_message_interaction_next_wake(policy_now + 2000),
            "slowmode offscreen policy change leaves no composer countdown wake");
        ui.active_channel_id = channel_id;
        application_message_interaction_tick(policy_now + 3000);
        check(application_message_interaction_next_wake(policy_now + 31'999) == policy_now + 32'000,
            "slowmode returning to a channel preserves the deadline reduced while offscreen");
        channels[0].slowmode_code = slowmode_encode(20);
        application_message_interaction_tick(policy_now + 31'000);
        check(application_message_interaction_next_wake(policy_now + 31'999) == policy_now + 32'000,
            "slowmode decrease longer than remaining wait cannot extend the prediction");
        application_message_interaction_tick(policy_now + 32'000);
        check(!application_message_interaction_next_wake(policy_now + 32'000) &&
            !application_composer_status_notice().bytes && !application_composer_slowmode_active(channel_id) &&
            utf8_equal(message_composer_text(g_app.message_composer), draft),
            "slowmode reduced deadline expires exactly and preserves the unsent draft");
        channels[0].slowmode_code = slowmode_encode(21'600);
        application_composer_note_sent(channel_id);
        channels[0].slowmode_code = 0;
        check(!application_composer_status_notice().bytes && !application_composer_slowmode_active(channel_id),
            "slowmode disabled channel immediately removes its predicted notice and block");
        channels[0].slowmode_code = slowmode_encode(21'600);
        handled = true;
        check(application_composer_prepare_send(&handled) && !handled &&
            !application_composer_slowmode_active(channel_id) && utf8_equal(message_composer_text(g_app.message_composer), draft),
            "slowmode re-enabling the channel does not resurrect an old prediction");

        auto& controller = ui.message_composer_controller;
        const auto queued = message_composer_controller_queue_send(&controller, true, self_id, 1000, 100, 0);
        check(queued.ok() && message_composer_controller_mark_submitted(&controller, queued.nonce).ok(),
            "slowmode authoritative retry fixture queues and submits a real request");
        RestClientResponse response{};
        response.status_code = 429;
        response.rate_limited = true;
        response.retry_at_ms = 500;
        const auto receipt = message_send_accept_response(&controller, response, {}, {}, channel_id, self_id, 110);
        application_composer_note_sent(channel_id);
        const auto replaced = message_composer_replace_range(&g_app.message_composer, 0, g_app.message_composer.bytes, draft);
        handled = true;
        MessageComposerSubmit submission{};
        check(receipt.http.ok() && replaced != MessageComposerResult::invalid_input && permissions(kPermissionBypassSlowmode) &&
            application_composer_prepare_send(&handled) && !handled && !application_composer_slowmode_active(channel_id) &&
            utf8_equal(message_composer_text(g_app.message_composer), draft) &&
            message_composer_controller_prepare_submit(&controller, 499, &submission).error ==
                MessageComposerControllerError::not_found &&
            message_composer_controller_prepare_submit(&controller, 500, &submission).ok() && submission.nonce == queued.nonce,
            "slowmode bypass clears only the prediction while a real 429 keeps its exact 499/500 retry floor");
        message_composer_controller_cancel_unsent(&controller, queued.nonce);
        application_message_interaction_tick(~u64{0});
        check(!application_composer_status_notice().bytes, "slowmode retired countdown leaves an empty status strip");
    }
    ui.chat_state = saved_state;
    ui.active_channel_id = saved_active_channel;
    message_composer_controller_select_channel(&ui.message_composer_controller, saved_composer_channel);
    g_app.account.instance = saved_instance;
    application_composer_slowmode_active(0);
}
// The newest local notice (the client's bot message) on the visible timeline.
bool notice_says(Utf8View expected) noexcept {
    const auto& store = *g_app.message_view.timeline.store;
    for (u32 i = 0; i < store.capacity; ++i) {
        const auto& record = store.records[i];
        if (record.occupied && record.type == kMessageTypeLocalNotice &&
            utf8_equal({reinterpret_cast<const char*>(store.text + record.content_offset), record.content_bytes},
                       expected))
            return true;
    }
    return false;
}
void builtin_cases() noexcept {
    auto& ui = *g_app.account_ui;
    auto& composer = g_app.message_composer;
    const auto saved_state = ui.chat_state;
    const u64 saved_active_channel = ui.active_channel_id;
    const u64 saved_composer_channel = composer.channel_id;
    GuildRecord guilds[1]{};
    ChannelRecord channels[1]{};
    u16 guild_order[1]{}, channel_order[1]{};
    u8 names[128]{};
    ChannelInput channel{};
    channel.id = 9;
    channel.guild_id = 55;
    channel.name = utf8_literal("general");
    const bool initialized =
        chat_state_store_initialize(&ui.chat_state, {guilds, 1}, {guild_order, 1}, {channels, 1},
                                    {channel_order, 1}, {names, sizeof(names)}) &&
        chat_state_store_upsert_guild(&ui.chat_state, {55, utf8_literal("Guild"), GuildMutationKind::create, 1})
            .ok() &&
        chat_state_store_upsert_channel(&ui.chat_state, channel).ok() &&
        message_composer_controller_select_channel(&ui.message_composer_controller, 9).ok();
    check(initialized, "built-in command fixture initializes");
    if (initialized) {
        ui.active_channel_id = 9;
        ui.chat_state.self_user_id = 1;
        channels[0].permissions_resolved = true;
        channels[0].effective_permissions = kPermissionViewChannel | kPermissionSendMessages |
                                            kPermissionChangeNickname | kPermissionCreatePublicThreads |
                                            kPermissionReadMessageHistory | kPermissionKickMembers;
        const auto pick = [&](Utf8View name, Utf8View typed) noexcept {
            edit(name);
            const u32 action = action_named(name);
            if (!action || !application_commands_activate(action))
                return false;
            edit(typed);
            return true;
        };
        // The discovery list shows 420 DIPs at a time; each name's query shows
        // whether this channel lists it.
        const auto listed = [](Utf8View query, Utf8View name) noexcept {
            edit(query);
            return action_named(name) != 0;
        };
        edit(utf8_literal("/"));
        check(action_named(utf8_literal("Built-in")) && listed(utf8_literal("/shrug"), utf8_literal("/shrug")) &&
                  listed(utf8_literal("/gif"), utf8_literal("/gif")) &&
                  listed(utf8_literal("/nick"), utf8_literal("/nick")) &&
                  listed(utf8_literal("/thread"), utf8_literal("/thread")) &&
                  listed(utf8_literal("/kick"), utf8_literal("/kick")) &&
                  !listed(utf8_literal("/ban"), utf8_literal("/ban")) &&
                  !listed(utf8_literal("/timeout"), utf8_literal("/timeout")) &&
                  !listed(utf8_literal("/leave"), utf8_literal("/leave")),
              "the picker lists the client's built-in commands this channel allows, in their own section");
        check(pick(utf8_literal("/shrug"), utf8_literal("/shrug message:hello")) &&
                  application_commands_key(VK_RETURN) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("hello \xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF")),
              "/shrug appends the shrug and leaves the message for the ordinary send");
        bool handled = true;
        check(application_commands_send(&handled) && !handled,
              "the rewritten message sends as typed rather than as a command");
        edit(utf8_literal("/shrug "));
        check(g_app.message_composer_scene.command.name_bytes == 6 &&
                  utf8_equal(message_composer_text(composer), utf8_literal("/shrug ")),
              "typing a command's whole name and a space selects it, like the client");
        edit(utf8_literal("/shrug hi"));
        // The option's value ends at its end marker (U+2063), the caret before it.
        check(utf8_equal(message_composer_text(composer), utf8_literal("/shrug message:hi\xE2\x81\xA3")) &&
                  composer.caret == composer.bytes - 3,
              "text after a command whose only option is optional becomes that option's value");
        check(pick(utf8_literal("/tableflip"), utf8_literal("/tableflip")) && application_commands_key(VK_RETURN) &&
                  utf8_equal(message_composer_text(composer),
                             utf8_literal("(\xE2\x95\xAF\xC2\xB0\xE2\x96\xA1\xC2\xB0)\xE2\x95\xAF\xEF\xB8\xB5 "
                                          "\xE2\x94\xBB\xE2\x94\x81\xE2\x94\xBB")),
              "/tableflip without a message sends the table flip alone");
        check(pick(utf8_literal("/me"), utf8_literal("/me message:waves")) && application_commands_key(VK_RETURN) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("_waves_")),
              "/me wraps the message in emphasis");
        check(pick(utf8_literal("/spoiler"), utf8_literal("/spoiler message:ending")) &&
                  application_commands_key(VK_RETURN) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("||ending||")),
              "/spoiler marks the message as a spoiler");
        check(pick(utf8_literal("/gif"), utf8_literal("/gif query:cats")) && searches &&
                  searched_kind == UiMediaPickerKind::gif && utf8_equal({searched, searched_bytes}, utf8_literal("cats")) &&
                  application_commands_key(VK_RETURN) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("/gif query:cats\xE2\x81\xA3")),
              "/gif shows its inline results for the query; Enter without a chosen result keeps the draft");
        const u32 before_nick = calls;
        check(pick(utf8_literal("/nick"), utf8_literal("/nick new_nick:Maj")) && application_commands_key(VK_RETURN) &&
                  !composer.bytes && application_commands_pending(clock_monotonic_milliseconds()) &&
                  application_commands_submit(clock_monotonic_milliseconds()) && calls == before_nick + 1 &&
                  operation == 7 && submitted_guild == 55 &&
                  utf8_equal({payload, payload_bytes}, utf8_literal(R"({"nick":"Maj"})")),
              "/nick sends the client's nickname change on the command lane");
        RestClientResponse response{};
        response.request_id = submitted_request;
        response.status_code = 200;
        g_app.details_request_owner = MessageHistoryRequestOwner::none;
        check(application_commands_response(response, utf8_literal(R"({"nick":"Maj"})")) &&
                  notice_says(utf8_literal("Your nickname on this server has been changed to **Maj**.")),
              "a changed nickname is answered with the client's local notice");
        // The channel's own default archive (a day) replaces the three-day default.
        channels[0].default_archive = 2;
        check(pick(utf8_literal("/thread"), utf8_literal("/thread name:Plans message:first")) &&
                  application_commands_key(VK_RETURN) && application_commands_submit(clock_monotonic_milliseconds()) &&
                  operation == 8 && submitted_channel == 9 &&
                  utf8_equal({payload, payload_bytes},
                             utf8_literal(
                                 R"({"name":"Plans","type":11,"auto_archive_duration":1440,"location":"Slash Command"})")),
              "/thread creates a public thread with the client's body and the channel's default archive");
        channels[0].default_archive = 0;
        response.request_id = submitted_request;
        response.status_code = 201;
        const u32 requests_before = ui.message_composer_controller.request_count;
        thread_sendable = false;
        check(application_commands_response(response, utf8_literal(R"({"id":"777","type":11})")) &&
                  ui.message_composer_controller.request_count == requests_before,
              "the thread's first message waits for the thread channel");
        thread_sendable = true;
        u64 thread_message = 0;
        if (application_commands_tick(clock_monotonic_milliseconds()))
            for (const auto& request : ui.message_composer_controller.requests)
                if (request.occupied && request.channel_id == 777)
                    thread_message = request.nonce;
        check(thread_message && ui.message_composer_controller.request_count == requests_before + 1,
              "the first message is sent into the new thread once it arrives");
        message_composer_controller_cancel_unsent(&ui.message_composer_controller, thread_message);
        thread_sendable = false;
        const u32 before_kick = calls;
        check(pick(utf8_literal("/kick"), utf8_literal("/kick user:5")) && application_commands_key(VK_RETURN) &&
                  !application_commands_pending(clock_monotonic_milliseconds()) && calls == before_kick &&
                  notice_says(utf8_literal("You do not have the appropriate permissions to kick that user.")),
              "/kick refuses a member it cannot manage with the client's notice and no request");
        check(pick(utf8_literal("/msg"), utf8_literal("/msg user:42 message:hey")) &&
                  application_commands_key(VK_RETURN) && opened_dm_user == 42 && !composer.bytes,
              "/msg opens the DM with the user");
        ui.private_conversation.target_id = 42;
        ui.private_conversation.operation = PrivateConversationOperation::open;
        ui.private_conversation.phase = PrivateConversationPhase::failed;
        check(application_commands_tick(clock_monotonic_milliseconds()) &&
                  notice_says(utf8_literal("Oops! We encountered an error when trying to message that user.")),
              "a DM that could not open reports the client's error notice");
        reset_value(&ui.private_conversation);
        channels[0].type = 3;
        // Enter first takes the highlighted True choice, then runs the command.
        check(pick(utf8_literal("/leave"), utf8_literal("/leave silent:true")) &&
                  application_commands_key(VK_RETURN) && application_commands_key(VK_RETURN) && left_group == 9 &&
                  left_silent,
              "/leave asks to leave the group, silently when asked");
    }
    message_composer_clear(&composer);
    popup_scene_close(&g_app.popup_scene);
    set_popup_owner(PopupOwnerKind::none);
    ui.chat_state = saved_state;
    ui.active_channel_id = saved_active_channel;
    message_composer_controller_select_channel(&ui.message_composer_controller, saved_composer_channel);
    // Rebind the picker to the fixture's own channel (and its server) again.
    edit(utf8_literal("/"));
    message_composer_clear(&composer);
    popup_scene_close(&g_app.popup_scene);
    set_popup_owner(PopupOwnerKind::none);
}
// The bot's avatar in the channel's server comes first (the client's guild
// member avatar), as a still WebP by URL, in the picker and the composer.
void guild_picture_cases() noexcept {
    auto& ui = *g_app.account_ui;
    auto& composer = g_app.message_composer;
    const auto saved_state = ui.chat_state;
    const u64 saved_active_channel = ui.active_channel_id;
    const u64 saved_composer_channel = composer.channel_id;
    GuildRecord guilds[1]{};
    ChannelRecord channels[1]{};
    u16 guild_order[1]{}, channel_order[1]{};
    u8 names[128]{};
    ChannelInput channel{};
    channel.id = 9;
    channel.guild_id = 55;
    channel.name = utf8_literal("general");
    UserAppearance member{};
    member.user_id = 21;
    member.guild_id = 55;
    member.fields = appearance_avatar;
    copy_bytes(member.avatar.data, "fedcba9876543210fedcba9876543210", 32);
    member.avatar.bytes = 32;
    const bool initialized =
        chat_state_store_initialize(&ui.chat_state, {guilds, 1}, {guild_order, 1}, {channels, 1},
                                    {channel_order, 1}, {names, sizeof(names)}) &&
        chat_state_store_upsert_guild(&ui.chat_state, {55, utf8_literal("Guild"), GuildMutationKind::create, 1})
            .ok() &&
        chat_state_store_upsert_channel(&ui.chat_state, channel).ok() &&
        message_composer_controller_select_channel(&ui.message_composer_controller, 9).ok() &&
        user_appearance_upsert(&ui.user_store.appearance, member);
    check(initialized, "server picture fixture initializes");
    if (initialized) {
        ui.active_channel_id = 9;
        edit(utf8_literal("/"));
        const bool loaded = application_commands_submit(clock_monotonic_milliseconds()) && operation == 0;
        respond(200, fixture_index);
        edit(utf8_literal("/bump"));
        // The second app's /bump (its row's detail starts with its description).
        const PopupItem* other = nullptr;
        for (u32 i = 0; i < g_app.popup_scene.item_count; ++i) {
            const auto& item = g_app.popup_scene.items[i];
            if (utf8_equal(item.label, utf8_literal("/bump")) && item.detail.bytes > 11 &&
                bytes_equal(item.detail.data, "Second bump", 11))
                other = &item;
        }
        char row_url[160]{}, bar_url[160]{};
        const Utf8View row = other && (other->media_flags & kPopupItemMediaUrl)
                                 ? Utf8View{row_url, application_commands_icon_url(other->media_id,
                                                                                   {row_url, sizeof(row_url)})}
                                 : Utf8View{};
        constexpr auto expected = utf8_literal("https://cdn.discordapp.com/guilds/55/users/21/avatars/"
                                               "fedcba9876543210fedcba9876543210.webp?size=64");
        check(loaded && utf8_equal(row, expected) &&
                  other->media_variant == static_cast<u32>(MediaAttachmentFormat::webp),
              "a bot's avatar in this server comes before its own avatar and the app's icon");
        check(other && application_commands_activate(other->action) &&
                  utf8_equal(picture_url(g_app.message_composer_scene.command.avatar, bar_url), expected),
              "the composer's command bar shows the same picture as the picker");
    }
    message_composer_clear(&composer);
    popup_scene_close(&g_app.popup_scene);
    set_popup_owner(PopupOwnerKind::none);
    ui.chat_state = saved_state;
    ui.active_channel_id = saved_active_channel;
    message_composer_controller_select_channel(&ui.message_composer_controller, saved_composer_channel);
    edit(utf8_literal("/"));
    message_composer_clear(&composer);
    popup_scene_close(&g_app.popup_scene);
    set_popup_owner(PopupOwnerKind::none);
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
            // Keyboard navigation follows the client's two lists: the discovery
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
            check(!application_commands_key(VK_RETURN) && utf8_equal(message_composer_text(composer), utf8_literal("/")),
                  "Enter without a discovery selection keeps its composer meaning");
            check(application_commands_key(VK_TAB) && selected() == row_action(false) &&
                      utf8_equal(message_composer_text(composer), utf8_literal("/")),
                  "Tab without a selection selects the top discovery row without choosing it");
            check(application_commands_key(VK_UP) && selected() == row_action(false),
                  "Up stops at the top of the discovery list");
            check(application_commands_key(VK_DOWN) && selected() != row_action(false) && selected(),
                  "Down moves the discovery selection");
            application_commands_hover();
            check(application_commands_key(VK_DOWN) && selected() == row_action(false),
                  "after hovering, Down starts again at the top of the discovery list");
            edit(utf8_literal("/o"));
            check(selected() == row_action(false), "typed query results focus their first row");
            check(application_commands_key(VK_UP) && selected() == row_action(true),
                  "Up from the first query result wraps to the last");
            check(application_commands_key(VK_DOWN) && selected() == row_action(false),
                  "Down from the last query result wraps to the first");
            application_commands_hover();
            check(application_commands_key(VK_UP) && selected() == row_action(true),
                  "after hovering, Up starts from the end of the query results");
            application_commands_hover();
            check(!application_commands_key(VK_TAB) && utf8_equal(message_composer_text(composer), utf8_literal("/o")),
                  "Tab after hovering keeps its composer meaning instead of choosing the hovered row");
        }
        edit(utf8_literal("/word"));
        check(application_commands_key(VK_RETURN) && utf8_equal(message_composer_text(composer),
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
        check(application_commands_key(VK_RETURN) && !g_app.popup_scene.active && !composer.bytes &&
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
        edit(utf8_literal("/word")); application_commands_key(VK_RETURN); application_commands_key(VK_RETURN);
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
        edit(utf8_literal("/word")); application_commands_key(VK_RETURN); application_commands_key(VK_RETURN);
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
        edit(utf8_literal("/word")); application_commands_key(VK_RETURN); application_commands_key(VK_RETURN);
        check(application_commands_submit(200), "failing command submits after the busy retry floor");
        respond(204);
        interaction(GatewayClientDispatchKind::interaction_failure, 0, submitted_nonce);
        echo = only_echo();
        check(echo && echo->delivery == MessageDeliveryState::failed &&
              utf8_equal(echo_text(*echo), utf8_literal("This interaction failed")),
              "a quick INTERACTION_FAILURE fails the local message");
        message_timeline_remove_local(&g_app.message_view.timeline, echo ? echo->id : 0);
        edit(utf8_literal("/word")); application_commands_key(VK_RETURN); application_commands_key(VK_RETURN);
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
        edit(utf8_literal("/word")); application_commands_key(VK_RETURN);
        popup_scene_close(&g_app.popup_scene); set_popup_owner(PopupOwnerKind::none);
        message_composer_select_all(&composer); refresh_message_composer_ui();
        check(!g_app.popup_scene.active, "dismissed picker stays closed during selection edits");
        check(g_app.message_composer_scene.command.name_bytes == 7 && composer.bytes,
              "Escape-style popup dismissal keeps the selected editable command identity");
        edit(utf8_literal("/wor"));
        check(g_app.popup_scene.active && !g_app.message_composer_scene.command.name_bytes &&
              popup_items_command_picker({g_app.popup_scene.items.data, g_app.popup_scene.item_count}),
              "backspacing through a selected command name restores the picker and clears its decoration");
        application_commands_key(VK_RETURN);
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
            // The option is the client's chip: its blank and name project to the
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
        check(application_commands_key(VK_RETURN) && application_commands_submit(1000) && operation == 2 &&
              payload_contains(utf8_literal("\"value\":\"keep \\\"quoted\\\" value\"")),
              "typed values are sent as typed, quotes included, like the client's pill text");
        respond(204);

        edit(utf8_literal("/pick"));
        application_commands_key(VK_RETURN);
        // The client sets a chosen command up with its required options as
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
        check(application_commands_key(VK_RETURN) && application_commands_submit(1001) &&
              payload_contains(utf8_literal("\"type\":4,\"value\":2")),
              "chosen numeric choice name executes as its JSON number value");
        respond(204);
        check(application_commands_activate(choice_action) && !composer.bytes,
              "a retired choice action cannot dereference or edit a completed command");

        edit(utf8_literal("/upload"));
        application_commands_key(VK_RETURN);
        const u32 file_option = action_named(utf8_literal("Drag and drop or click to upload file"));
        const u32 opens = picker_opens;
        check(file_option && application_commands_activate(file_option) && picker_opens == opens + 1 &&
                  utf8_equal(message_composer_text(composer), utf8_literal("/upload file:\xE2\x81\xA3")),
              "an attachment chip asks for its file like the client's upload area");
        const u32 before_upload = calls;
        check(application_commands_key(VK_RETURN) && composer.bytes && calls == before_upload &&
                  !application_commands_pending(1100),
              "a required attachment option without a staged upload never sends");
        staged_uploads[0] = {utf8_literal("a.png"), utf8_literal("up/a.png"), {}, 0, false};
        staged_uploads[1] = {utf8_literal("b.txt"), utf8_literal("up/b.txt"), {}, 1, false};
        staged_upload_count = 2;
        edit(utf8_literal("/upload file:b.txt"));
        check(application_commands_key(VK_RETURN) && !composer.bytes && application_commands_submit(1101) &&
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
              application_commands_key(VK_RETURN) && application_commands_submit(1002) &&
              payload_contains(utf8_literal("\"type\":2,\"name\":\"group\",\"options\":[{\"type\":1,\"name\":\"run\"")),
              "flattened subcommand selection preserves arguments and serializes its nested wire scope");
        respond(204);

        edit(utf8_literal("/bump"));
        char second_picture[160]{};
        const u32 second_app_action = action_named(utf8_literal("/bump"), 1);
        check(second_app_action && application_commands_activate(second_app_action) &&
              utf8_equal(picture_url(g_app.message_composer_scene.command.avatar, second_picture),
                         utf8_literal("https://cdn.discordapp.com/app-icons/20/0f1e2d.png?size=160")) &&
              application_commands_key(VK_RETURN) && application_commands_submit(1003) &&
              submitted_application == 20 && payload_contains(utf8_literal("\"id\":\"105\"")),
              "duplicate command labels retain the selected application and command identity");
        respond(204);

        check(application_commands_tick(1) && application_commands_next_wake() != 1,
              "sent commands' local messages repaint before the next picker");
        {
            // The end marker is the client's pill end: "name:" typed inside a
            // value stays text, Tab leaves the chip, a key typed after it makes
            // the next chip, and Backspace after a chip deletes its last character.
            edit(utf8_literal("/search"));
            application_commands_key(VK_RETURN);
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
            // Back in the autocomplete option: the client's five Loading rows
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
        application_commands_key(VK_RETURN);
        edit(utf8_literal("/search query:old count:2"));
        // Shift+Tab selects the previous chip's value (the client's
        // selectPreviousCommandOption); Tab the next one, then the end.
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
        check(application_commands_key(VK_RETURN) && application_commands_submit(clock_monotonic_milliseconds()) &&
              operation == 2 && payload_contains(utf8_literal("\"value\":\"picked value\"")),
              "accepted dynamic choice is execution data rather than another autocomplete request");
        respond(204);

        edit(utf8_literal("/echo")); application_commands_key(VK_RETURN);
        const u32 unchanged = calls;
        check(utf8_equal(message_composer_text(composer), utf8_literal("/echo text:\xE2\x81\xA3")) &&
                  application_commands_key(VK_RETURN) && composer.bytes && calls == unchanged &&
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

        // Frequently Used: settings-proto/2 command frecency, read and saved like the client.
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
                    // The client's icon order without a bot avatar: the app's
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
                check(application_commands_key(VK_LEFT) && selected_item() &&
                          (selected_item()->flags & popup_item_command_category) &&
                          utf8_equal(selected_item()->label, utf8_literal("Frequently Used")),
                      "Left moves the keyboard into the rail at the highlighted section");
                check(application_commands_key(VK_DOWN) && selected_item() &&
                          utf8_equal(selected_item()->label, utf8_literal("Other")) && application_commands_key(VK_UP) &&
                          application_commands_key(VK_UP) && utf8_equal(selected_item()->label,
                                                                         utf8_literal("Frequently Used")),
                      "Up and Down move through the rail and stop at its ends");
                check(application_commands_key(VK_DOWN) && application_commands_key(VK_DOWN) &&
                          application_commands_key(VK_RETURN) && selected_item() &&
                          utf8_equal(selected_item()->label, utf8_literal("Test")) &&
                          !section(utf8_literal("Frequently Used")) && section(utf8_literal("Test")) &&
                          g_app.composer_menu_held_key == VK_RETURN,
                      "Enter in the rail filters to the focused section and keeps the rail focus");
                g_app.composer_menu_held_key = 0;
                const auto first_row = [] noexcept {
                    for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
                        if ((g_app.popup_scene.items[i].flags & popup_item_command_row) &&
                            (g_app.popup_scene.items[i].flags & popup_item_enabled))
                            return g_app.popup_scene.items[i].action;
                    return 0u;
                };
                check(application_commands_key(VK_RIGHT) && first_row() &&
                          popup_scene_selected_action(g_app.popup_scene) == first_row(),
                      "Right returns to the list at the section's first row");
                check(application_commands_key(VK_LEFT) && application_commands_key(VK_ESCAPE) &&
                          popup_scene_selected_action(g_app.popup_scene) == first_row() &&
                          !application_commands_key(VK_ESCAPE),
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
            // The client counts built-ins too (APPLICATION_COMMAND_USED before
            // execute, keyed by the negative id): a used /shrug joins Frequently Used.
            // Five uses outrank the fixture's earlier commands.
            for (u32 use = 0; use < 5; ++use) {
                edit(utf8_literal("/shrug"));
                bool shrug_handled = false;
                check(application_commands_key(VK_RETURN) && application_commands_send(&shrug_handled),
                      "built-in use: send");
            }
            edit(utf8_literal("/"));
            check(action_named(utf8_literal("/shrug")) != 0,
                  "a used built-in command is listed under Frequently Used");
            g_app.account.status.running = false;
        }

        builtin_cases();
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
