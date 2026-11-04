#include "commands_ui_internal.hpp"
#include "../../../../src/app/chat/composer/commands.hpp"
#include "../../../support/model_store_fixtures.hpp"
#include "../../../../src/app/chat/messages/feedback.hpp"
#include "maicord/app/chat/messages/send_response.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/model/guild/permission_bits.hpp"
#include "../../../../src/app/account/account_ui_runtime.hpp"
#include "maicord/protocol/user/private_conversation.hpp"

namespace maicord {
extern u32 shake_offsets;
}
namespace maicord {
namespace commands_ui_detail {

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
        check(application_commands_key(key::enter) && g_app.message_composer_scene.command.name_bytes == 6 &&
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
            check(application_commands_key(key::enter) && application_commands_key(key::enter),
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
    ChannelRecord channels[2]{};
    RoleRecord roles[1]{};
    PermissionOverwriteRecord overwrites[1]{};
    u16 guild_order[1]{}, channel_order[2]{};
    u8 names[256]{};
    u32 account_owners[8]{};
    constexpr u64 guild_id = 55, channel_id = 9, other_channel_id = 10, self_id = 7;
    constexpr PermissionSet ordinary = kPermissionViewChannel | kPermissionSendMessages;
    constexpr Utf8View draft = utf8_literal("slowmode draft stays");
    ChannelInput channel_input{};
    channel_input.id = channel_id;
    channel_input.guild_id = guild_id;
    channel_input.name = utf8_literal("slowmode");
    channel_input.rate_limit_per_user = 21'600;
    channel_input.rate_limit_known = true;
    ChannelInput other_input = channel_input;
    other_input.id = other_channel_id;
    other_input.name = utf8_literal("other");
    const bool initialized = chat_state_store_initialize(&ui.chat_state, {guilds, 1}, {guild_order, 1},
        {channels, 2}, {channel_order, 2}, {names, sizeof(names)}) &&
        chat_state_store_bind_permissions(&ui.chat_state, {roles, 1}, {overwrites, 1}) &&
        chat_state_store_upsert_guild(&ui.chat_state, {guild_id, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok() &&
        chat_state_store_upsert_channel(&ui.chat_state, channel_input).ok() &&
        chat_state_store_upsert_channel(&ui.chat_state, other_input).ok() &&
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
        check(application_message_interaction_next_wake(policy_now + 59'999) == policy_now + 60'100,
            "slowmode nonzero policy decrease caps the running prediction to the new duration");
        channels[0].slowmode_code = slowmode_encode(21'600);
        application_message_interaction_tick(policy_now + 1000);
        check(application_message_interaction_next_wake(policy_now + 59'999) == policy_now + 60'100,
            "slowmode policy increase preserves the earlier predicted deadline");
        ui.active_channel_id = 0;
        channels[0].slowmode_code = slowmode_encode(30);
        application_message_interaction_tick(policy_now + 2000);
        check(!application_message_interaction_next_wake(policy_now + 2000),
            "slowmode offscreen policy change leaves no composer countdown wake");
        ui.active_channel_id = channel_id;
        application_message_interaction_tick(policy_now + 3000);
        check(application_message_interaction_next_wake(policy_now + 31'999) == policy_now + 32'100,
            "slowmode returning to a channel preserves the deadline reduced while offscreen");
        channels[0].slowmode_code = slowmode_encode(20);
        application_message_interaction_tick(policy_now + 31'000);
        check(application_message_interaction_next_wake(policy_now + 31'999) == policy_now + 32'100,
            "slowmode decrease longer than remaining wait cannot extend the prediction");
        application_message_interaction_tick(policy_now + 32'100);
        check(!application_message_interaction_next_wake(policy_now + 32'100) &&
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
        application_composer_note_sent(channel_id);
        application_composer_note_sent(other_channel_id);
        check(application_composer_slowmode_active(channel_id) && application_composer_slowmode_active(other_channel_id),
            "slowmode cooldowns of two channels run side by side");
        application_composer_slowmode_cooldown(other_channel_id, 5000, 0);
        application_message_interaction_tick(clock_monotonic_milliseconds() + 6000);
        check(application_composer_slowmode_active(channel_id) && !application_composer_slowmode_active(other_channel_id),
            "slowmode a server cooldown from the typing response replaces one channel's deadline only");
        application_composer_slowmode_cooldown(other_channel_id, 0, 0);
        check(!application_composer_slowmode_active(other_channel_id), "slowmode a zero server cooldown arms nothing");
        application_composer_slowmode_cooldown(other_channel_id, 0, 7000);
        check(application_thread_create_cooldown_active(other_channel_id) && !application_composer_slowmode_active(other_channel_id),
            "slowmode the typing response's thread cooldown is separate from the send cooldown");
        application_thread_create_note_created(channel_id);
        application_composer_slowmode_clear(channel_id);
        check(application_thread_create_cooldown_active(channel_id) && !application_composer_slowmode_active(channel_id),
            "slowmode a discarded upload retires the send cooldown only; thread creation keeps its own");

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

} // namespace commands_ui_detail

namespace {

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

} // namespace

namespace commands_ui_detail {

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
                  application_commands_key(key::enter) &&
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
        check(pick(utf8_literal("/tableflip"), utf8_literal("/tableflip")) && application_commands_key(key::enter) &&
                  utf8_equal(message_composer_text(composer),
                             utf8_literal("(\xE2\x95\xAF\xC2\xB0\xE2\x96\xA1\xC2\xB0)\xE2\x95\xAF\xEF\xB8\xB5 "
                                          "\xE2\x94\xBB\xE2\x94\x81\xE2\x94\xBB")),
              "/tableflip without a message sends the table flip alone");
        check(pick(utf8_literal("/me"), utf8_literal("/me message:waves")) && application_commands_key(key::enter) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("_waves_")),
              "/me wraps the message in emphasis");
        check(pick(utf8_literal("/spoiler"), utf8_literal("/spoiler message:ending")) &&
                  application_commands_key(key::enter) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("||ending||")),
              "/spoiler marks the message as a spoiler");
        // The ordinary send path first rewrites the client's legacy text commands, then runs the selected
        // built-in: both must take the option's value, never "message:" or the chip's end marker.
        const auto sent_as = [&](Utf8View name, Utf8View typed, bool legacy, Utf8View expected) noexcept {
            bool prepared = true, ran = true;
            edit({}); // A command left selected by the previous case ends with its draft.
            if (!pick(name, typed) || !application_composer_prepare_send(&prepared) || prepared)
                return false;
            const auto drafted = message_composer_text(composer);
            const bool rewritten = drafted.bytes && drafted.data[0] != '/';
            return rewritten == legacy && application_commands_send(&ran) && !ran &&
                   utf8_equal(message_composer_text(composer), expected);
        };
        check(sent_as(utf8_literal("/me"), utf8_literal("/me message:test"), true, utf8_literal("_test_")),
              "/me sends the typed value in emphasis, without its option name or end marker");
        check(sent_as(utf8_literal("/me"), utf8_literal("/me message:snake_case *x*"), true,
                      utf8_literal("_snake_case *x*_")),
              "/me wraps the value as typed, escaping nothing, like the client");
        check(sent_as(utf8_literal("/shrug"), utf8_literal("/shrug message:hi"), true,
                      utf8_literal("hi \xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF")),
              "/shrug sends the value then the face unescaped");
        check(sent_as(utf8_literal("/shrug"), utf8_literal("/shrug"), true,
                      utf8_literal("\xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF")),
              "/shrug without a value sends the face alone");
        check(sent_as(utf8_literal("/tableflip"), utf8_literal("/tableflip message:rage"), true,
                      utf8_literal("rage (\xE2\x95\xAF\xC2\xB0\xE2\x96\xA1\xC2\xB0)\xE2\x95\xAF\xEF\xB8\xB5 "
                                   "\xE2\x94\xBB\xE2\x94\x81\xE2\x94\xBB")),
              "/tableflip sends the value then the table flip");
        check(sent_as(utf8_literal("/unflip"), utf8_literal("/unflip message:calm"), true,
                      utf8_literal("calm \xE2\x94\xAC\xE2\x94\x80\xE2\x94\xAC\xE3\x83\x8E( \xC2\xBA _ \xC2\xBA\xE3\x83\x8E)")),
              "/unflip sends the value then the restored table");
        check(sent_as(utf8_literal("/spoiler"), utf8_literal("/spoiler message:ending"), false,
                      utf8_literal("||ending||")),
              "/spoiler sends the value between spoiler markers on the ordinary send path");
        check(pick(utf8_literal("/gif"), utf8_literal("/gif query:cats")) && searches &&
                  searched_kind == UiMediaPickerKind::gif && utf8_equal({searched, searched_bytes}, utf8_literal("cats")) &&
                  application_commands_key(key::enter) &&
                  utf8_equal(message_composer_text(composer), utf8_literal("/gif query:cats\xE2\x81\xA3")),
              "/gif shows its inline results for the query; Enter without a chosen result keeps the draft");
        // Enter on a result-producing command's query sends a result rather than
        // shaking the app; only a search with nothing to send is refused.
        const auto shook = []() noexcept {
            const u64 at = clock_monotonic_milliseconds();
            const u32 moves = shake_offsets;
            const bool moved = application_commands_tick(at) && application_commands_tick(at + 16) &&
                               shake_offsets == moves + 1;
            application_commands_tick(at + 400); // the content returns to rest
            return moved;
        };
        const auto gif_draft_kept = [&]() noexcept {
            return utf8_equal(message_composer_text(composer), utf8_literal("/gif query:cat\xE2\x81\xA3"));
        };
        const auto bar_refuses = [&]() noexcept {
            const auto& scene = g_app.message_composer_scene;
            return scene.command_bar_error &&
                   utf8_equal(message_composer_scene_command_bar_detail(scene), fake_empty_message);
        };
        // A refused Enter leaves /gif selected: start each case from a clean draft.
        const auto pick_gif = [&]() noexcept {
            edit({});
            return pick(utf8_literal("/gif"), utf8_literal("/gif query:cat"));
        };
        shook(); // the check above refused its Enter: let that shake rest
        u32 sends = fake_sends, waits = fake_waits;
        fake_results = FakeResults::ready;
        fake_selected = 0;
        fake_sent_index = ~u32{0};
        check(pick_gif() && application_commands_key(key::enter) && fake_sends == sends + 1 &&
                  fake_sent_index == 0 && !composer.bytes && !shook(),
              "/gif cat + Enter sends the first result and does not shake the app");
        sends = fake_sends;
        fake_results = FakeResults::ready;
        fake_selected = 1;
        check(pick_gif() && application_commands_key(key::enter) && fake_sends == sends + 1 &&
                  fake_sent_index == 1 && !composer.bytes && !shook(),
              "/gif cat + Enter sends the highlighted result when the selection moved off the first");
        sends = fake_sends;
        fake_results = FakeResults::ready;
        fake_selected = 0;
        bool sent_as_command = false;
        check(pick_gif() && application_commands_send(&sent_as_command) && sent_as_command &&
                  fake_sends == sends + 1 && !composer.bytes && !g_app.message_composer_scene.command.name_bytes &&
                  !shook(),
              "the composer's own send path gives /gif's Enter to the results too");
        sends = fake_sends;
        fake_results = FakeResults::loading;
        check(pick_gif() && application_commands_key(key::enter) && fake_waits == waits + 1 && fake_sends == sends &&
                  gif_draft_kept() && !shook(),
              "/gif cat + Enter while the results load waits with the draft kept and no shake");
        check(fake_results_arrive(3) && fake_sends == sends + 1 && fake_sent_index == 0 && !composer.bytes &&
                  !shook(),
              "the waited-for first result sends as it arrives");
        sends = fake_sends;
        waits = fake_waits;
        fake_results = FakeResults::loading;
        check(pick_gif() && application_commands_send(&sent_as_command) && fake_waits == waits + 1 &&
                  gif_draft_kept() && !shook() && fake_results_arrive(1) && fake_sends == sends + 1 &&
                  !composer.bytes,
              "the composer's own send path waits for loading results the same way");
        waits = fake_waits;
        fake_results = FakeResults::loading;
        check(pick_gif() && application_commands_key(key::enter) && fake_waits == waits + 1 &&
                  fake_results_arrive(0) && gif_draft_kept() && bar_refuses() && shook(),
              "a wait that ends with no results shakes the app and says so in the option bar");
        sends = fake_sends;
        fake_results = FakeResults::none;
        check(pick_gif() && application_commands_key(key::enter) && fake_sends == sends && gif_draft_kept() &&
                  bar_refuses() && shook(),
              "/gif cat + Enter with no results keeps the draft, shakes the app and says why");
        edit({});
        const u32 before_nick = calls;
        check(pick(utf8_literal("/nick"), utf8_literal("/nick new_nick:Maj")) && application_commands_key(key::enter) &&
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
                  application_commands_key(key::enter) && application_commands_submit(clock_monotonic_milliseconds()) &&
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
        Utf8View thread_text{};
        if (application_commands_tick(clock_monotonic_milliseconds()))
            for (const auto& request : ui.message_composer_controller.requests)
                if (request.occupied && request.channel_id == 777) {
                    thread_message = request.nonce;
                    thread_text = {reinterpret_cast<const char*>(ui.message_composer_controller.request_text) +
                                       request.text_offset,
                                   request.text_bytes};
                }
        check(thread_message && ui.message_composer_controller.request_count == requests_before + 1 &&
                  utf8_equal(thread_text, utf8_literal("first")),
              "the first message is sent into the new thread once it arrives, as typed");
        message_composer_controller_cancel_unsent(&ui.message_composer_controller, thread_message);
        thread_sendable = false;
        const u32 before_kick = calls;
        check(pick(utf8_literal("/kick"), utf8_literal("/kick user:5")) && application_commands_key(key::enter) &&
                  !application_commands_pending(clock_monotonic_milliseconds()) && calls == before_kick &&
                  notice_says(utf8_literal("You do not have the appropriate permissions to kick that user.")),
              "/kick refuses a member it cannot manage with the client's notice and no request");
        check(pick(utf8_literal("/msg"), utf8_literal("/msg user:42 message:hey")) &&
                  application_commands_key(key::enter) && opened_dm_user == 42 && !composer.bytes,
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
                  application_commands_key(key::enter) && application_commands_key(key::enter) && left_group == 9 &&
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
// The bot's avatar in the channel's server comes first (its member avatar),
// as a still WebP by URL, in the picker and the composer.
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

} // namespace commands_ui_detail
} // namespace maicord
