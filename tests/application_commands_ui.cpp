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
} // namespace maicord::app_detail

namespace maicord {
namespace {
using namespace app_detail;
u32 failures, calls, operation;
char payload[65536];
u32 payload_bytes;
u64 submitted_channel, submitted_guild, submitted_application, submitted_nonce;
RestClientSubmitError submit_error;
bool measure_header_name(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    if (!request.no_wrap || request.styles.count != 1 ||
        request.styles[0].byte_count != request.text.bytes || request.styles[0].flags != text_style_bold)
        return false;
    *value = {{47, 20}, 16, 1};
    return true;
}
void check(bool ok, const char* label) noexcept {
    test_check(&failures, ok, label);
}
RestClientSubmitResult WINAPI submit(void*, u64 channel, u64 guild, u64 application, u64 nonce,
                                     Utf8View data, u64, u8 op) noexcept {
    ++calls;
    operation = op;
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
constexpr auto fixture_index = utf8_literal(R"({"applications":[{"id":"10","name":"Test","bot":{"id":"11"}},{"id":"20","name":"Other","bot":{"id":"21"}}],"application_commands":[{"id":"100","application_id":"10","version":"7","name":"wordle","description":"Play wordle","type":1},{"id":"101","application_id":"10","version":"7","name":"echo","description":"Echo","type":1,"options":[{"name":"text","type":3,"required":true}]},{"id":"102","application_id":"10","version":"7","name":"pick","description":"Pick","type":1,"options":[{"name":"mode","type":4,"required":true,"choices":[{"name":"One","value":1},{"name":"Two","value":2}]}]},{"id":"103","application_id":"10","version":"7","name":"admin","description":"Admin","type":1,"options":[{"name":"group","type":2,"options":[{"name":"run","description":"Run","type":1,"options":[{"name":"count","type":4,"required":true}]}]}]},{"id":"104","application_id":"10","version":"7","name":"bump","description":"First bump","type":1},{"id":"105","application_id":"20","version":"7","name":"bump","description":"Second bump","type":1},{"id":"106","application_id":"10","version":"7","name":"search","description":"Search","type":1,"options":[{"name":"query","type":3,"required":true,"autocomplete":true},{"name":"count","type":4}]},{"id":"107","application_id":"10","version":"7","name":"upload","description":"Upload","type":1,"options":[{"name":"file","type":11,"required":true},{"name":"extra","type":11}]}]})");
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
        UiTextOutput header_stream_output{&header_stream, nullptr};
        const auto original_header_name = g_app.popup_scene.items[0].label;
        g_app.popup_scene.items[0].label = utf8_literal("/caf\xC3\xA9");
        bool measured_name = false, measured_description = false;
        const bool header_shaped = ui_text_measure_cache_initialize(&header_measure, {header_measure_records, 4},
            measure_header_name, nullptr) && ui_text_command_stream_initialize(&header_stream, {header_text, 4}) &&
            popup_text_presentation_build_output(g_app.popup_scene, kDefaultTheme, &header_stream_output, &header_measure);
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
                  utf8_equal(message_composer_text(composer), utf8_literal("/echo ")) &&
                  g_app.message_composer_scene.command.name_bytes == 5,
              "confirming the clear inserts and selects the command");
        reset_value(&confirm_request);
        confirm_opens = false;
        edit({});
        check(application_commands_insert(0, 103, utf8_literal("admin group run")) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("/admin group run ")) &&
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
            check(pick_row && utf8_equal(pick_row->detail, utf8_literal("Search\x1FTest\x1Fquery  +1 optional")),
                  "optional options collapse into the client's +N optional");
        }
        edit(utf8_literal("/"));
        edit(utf8_literal("/echo text:\"keep quoted value\""));
        const u32 echo_action = action_named(utf8_literal("/echo"));
        check(echo_action && application_commands_activate(echo_action) &&
              utf8_equal(message_composer_text(composer), utf8_literal("/echo text:\"keep quoted value\"")),
              "selecting an exact typed command preserves its existing named arguments");
        {
            refresh_message_composer_ui();
            const auto& scene = g_app.message_composer_scene;
            bool key_pill = false, value_plain = true;
            for (u32 i = 0; i < scene.display_style_count; ++i) {
                const auto& span = scene.display_styles[i];
                if (text_style_is_command_option(span.flags))
                    key_pill |= span.byte_offset == 6 && span.byte_count == 5;
                if (text_style_is_command_option(span.flags) && span.byte_offset + span.byte_count > 11)
                    value_plain = false;
            }
            check(key_pill && value_plain, "the selected command's typed option key draws as an option pill");
        }
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
              utf8_equal(message_composer_text(composer), utf8_literal("/pick mode:Two ")),
              "static choice shows its name in the draft like the official client");
        check(application_commands_key(VK_RETURN) && application_commands_submit(1001) &&
              payload_contains(utf8_literal("\"type\":4,\"value\":2")),
              "chosen numeric choice name executes as its JSON number value");
        respond(204);
        check(old_option_action && application_commands_activate(old_option_action) && !composer.bytes,
              "a retired option action cannot dereference or edit a completed command");

        edit(utf8_literal("/upload"));
        application_commands_key(VK_RETURN);
        const u32 file_option = action_named(utf8_literal("file"));
        const u32 opens = picker_opens;
        check(file_option && application_commands_activate(file_option) && picker_opens == opens + 1 &&
                  utf8_equal(message_composer_text(composer), utf8_literal("/upload file:")),
              "an attachment option asks for its file like the client's upload pill");
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
        const u32 second_app_action = action_named(utf8_literal("/bump"), 1);
        check(second_app_action && application_commands_activate(second_app_action) &&
              g_app.message_composer_scene.command.bot_id == 21 &&
              application_commands_key(VK_RETURN) && application_commands_submit(1003) &&
              submitted_application == 20 && payload_contains(utf8_literal("\"id\":\"105\"")),
              "duplicate command labels retain the selected application and command identity");
        respond(204);

        check(application_commands_tick(1) && application_commands_next_wake() != 1,
              "sent commands' local messages repaint before the next picker");
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
        UiTextOutput stream_output{&stream, nullptr};
        ui_text_command_stream_initialize(&stream, {command_text, 96});
        check(popup_text_presentation_build_output(g_app.popup_scene, kDefaultTheme, &stream_output) && stream.count,
              "command error picker retains visible text commands");

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
                      action_named(utf8_literal("/wordle"), 1) && action_named(utf8_literal("/echo"), 1),
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
                const PopupItem* frequent_rail = nullptr;
                for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
                    if (utf8_equal(g_app.popup_scene.items[i].label, utf8_literal("Frequently Used")))
                        frequent_rail = &g_app.popup_scene.items[i];
                check(frequent_rail && (frequent_rail->flags & popup_item_command_category) &&
                          ui_menu_row_icon(frequent_rail->presentation) == UiIcon::recent,
                      "the rail offers the client's clock entry for Frequently Used");
                u32 rows = 0;
                if (frequent_rail && application_commands_activate(frequent_rail->action))
                    for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
                        rows += (g_app.popup_scene.items[i].flags & popup_item_command_row) ? 1u : 0u;
                check(rows >= 2 && rows <= 5 && action_named(utf8_literal("/wordle")) &&
                          !action_named(utf8_literal("/wordle"), 1),
                      "the clock entry shows only the frequently used commands");
                const u32 all_rail = action_named(utf8_literal("All"));
                check(all_rail && application_commands_activate(all_rail) &&
                          action_named(utf8_literal("/wordle"), 1),
                      "All restores the full list led by Frequently Used");
            }
            edit(utf8_literal("/wor"));
            check(action_named(utf8_literal("/wordle")) && !action_named(utf8_literal("/wordle"), 1),
                  "a query hides the Frequently Used section, as in the client");
            g_app.account.status.running = false;
        }

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
