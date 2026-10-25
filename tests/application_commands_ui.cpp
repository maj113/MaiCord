#include "../src/app/application_commands.hpp"
#include "../src/app/application_state.hpp"
#include "../src/app/application_plugins.hpp"
#include "../src/app/application_message_feedback.hpp"
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
            check(application_commands_tick(1) && calls == submitted_before &&
                      utf8_equal(message_composer_text(composer), utf8_literal("/guild ")),
                  "command invalidation never resubmits or edits a queued or sending draft");
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
        command_index_invalidation_cases();
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
        UiTextOutput stream_output{&stream, nullptr};
        ui_text_command_stream_initialize(&stream, {command_text, 96});
        check(popup_text_presentation_build_output(g_app.popup_scene, kDefaultTheme, &stream_output) && stream.count,
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
