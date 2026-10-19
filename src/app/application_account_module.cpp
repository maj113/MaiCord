#include "application_user_actions.hpp"
#include "maicord/core/hash.hpp"
#include "application_guild_community.hpp"
#include "application_activity.hpp"
#include "application_message_components.hpp"
#include "application_private_channel.hpp"
#include "application_stage.hpp"
#include "application_channel_memory.hpp"
#include "application_voice_channels.hpp"
#include "application_soundboard.hpp"
#include "application_action_captcha.hpp"
#include "application_emoji_preferences.hpp"
#include "application_guild_folders.hpp"
#include "application_user_settings.hpp"
#include "application_account_settings.hpp"
#include "application_downloads.hpp"
#include "application_message_feedback.hpp"
#include "maicord/core/win32_process.hpp"
#include "application_load_trace.hpp"
#include "application_perf_mark.hpp"
#include "application_commands.hpp"
#include "application_account_module.hpp"
#include "application_custom_status.hpp"
#include "application_notification_controls.hpp"
#include "application_inbox.hpp"
#include "application_account_logout.hpp"
#include "application_account_switch.hpp"
#include "application_scoped_read.hpp"
#include "application_scoped_read_menu.hpp"
#include "application_thread_browser.hpp"
#include "application_thread_creation.hpp"
#include "application_forum_post.hpp"
#include "application_message_forward.hpp"
#include "application_quick_switcher.hpp"
#include "application_channel_overview.hpp"
#include "application_attachments.hpp"
#include "application_forum_attachments.hpp"
#include "application_forum_preview.hpp"
#include "application_composer_preview.hpp"
#include "application_friends.hpp"
#include "application_plugins.hpp"
#include "application_notifications.hpp"
#include "application_video.hpp"
#include "application_tooltip.hpp"
#include "application_account_menu.hpp"
#include "application_search.hpp"
#include "application_message_navigation.hpp"
#include "application_media_picker.hpp"
#include "application_pinned_messages.hpp"
#include "application_message_history.hpp"
#include "application_thread_actions.hpp"
#include "application_poll.hpp"
#include "application_member_moderation.hpp"

#include "application_account_presentation.hpp"
#include "application_login_ui.hpp"
#include "application_login_auth.hpp"
#include "application_state.hpp"
#include "application_ui_runtime.hpp"
#include "application_voice_message.hpp"

#include "maicord/app/account_credential_forget.hpp"
#include "maicord/app/account_paths.hpp"
#include "maicord/app/message_draft_session.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_error.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {
void log_member_source_binding(const GatewayClientEvent& event) noexcept {
    if (!g_app.account_ui || event.kind != GatewayClientEventKind::dispatch ||
        event.dispatch_kind != GatewayClientDispatchKind::guild_member_list_update)
        return;
    const auto& state = g_app.account_ui->chat_state;
    const auto* selected = chat_state_store_find_channel(state, state.selected_channel_id);
    const bool same_guild = selected && selected->guild_id == event.member_list.guild_id;
    const u64 derived_list = same_guild ? member_list_channel_id(state, selected->id) : 0;
    TraceRecord trace("member_source_binding");
    trace.field("selected_guild_channel", selected && selected->guild_id ? u64{1} : u64{0});
    trace.field("same_guild", same_guild ? u64{1} : u64{0});
    trace.field("derived_list_available", derived_list ? u64{1} : u64{0});
    trace.field("incoming_list_present", event.member_list.list_id ? u64{1} : u64{0});
    trace.field("same_list",
                same_guild && derived_list &&
                        (!event.member_list.list_id || derived_list == event.member_list.list_id)
                    ? u64{1}
                    : u64{0});
    trace.field("member_records", event.member_list.member_record_count);
    trace.field("group_summaries", event.member_list.group_count);
    trace.field("logical_count", event.member_list.logical_count);
    trace.write("member_hydration");
}
} // namespace

namespace {
bool author_hydration_visible() noexcept {
    return g_app.account_ui && g_app.account.instance && !g_app.account.stop_failed &&
           !g_app.login.visible && !application_friends_active() &&
           !renderer_settings_hides_shell(g_app.renderer) && g_app.message_scene.active &&
           ui_rect_has_area(g_app.message_scene.viewport);
}
bool synchronize_member_subscription_impl(u64 now_ms) noexcept {
    MemberListSubscriptionRetry& pending = g_app.member_list_subscription;
    if (!g_app.account_ui) {
        member_list_subscription_complete(&pending);
        thread_member_list_subscription_complete(&g_app.thread_member_list_subscription);
        return true;
    }
    auto& ui = *g_app.account_ui;
    if (!message_event_pump_synchronize_thread_scope(&g_app.message_pump))
        return false;
    const auto& state = ui.chat_state;
    const auto* selected = chat_state_store_find_channel(state, state.selected_channel_id);
    const auto& scene = g_app.member_list_scene;
    const auto* pane =
        ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::member_sidebar));
    const bool visible = g_app.member_pane_visible && !g_app.login.visible &&
                         !application_friends_active() && !renderer_settings_hides_shell(g_app.renderer) &&
                         selected &&
                         selected->guild_id && pane && ui_rect_has_area(pane->rectangle);
    const bool ordinary_thread = selected && (selected->type == 11 || selected->type == 12);
    const bool active_thread =
        ordinary_thread &&
        thread_member_store_owns(ui.thread_member_store, selected->guild_id, selected->id);
    auto& thread_pending = g_app.thread_member_list_subscription;
    if (!thread_member_list_subscription_select(&thread_pending, active_thread ? selected->guild_id : 0,
                                                active_thread ? selected->id : 0, visible && active_thread,
                                                now_ms))
        return false;
    ThreadMemberListSubscriptionCommand thread_command{};
    if (thread_member_list_subscription_command(thread_pending, now_ms, &thread_command)) {
        GatewayGuildSubscription subscription{};
        subscription.guild_id = thread_command.guild_id;
        subscription.thread_id = thread_command.thread_id;
        subscription.thread_member_lists_present = true;
        subscription.omit_channel_ranges = true;
        subscription.omit_features = !thread_command.thread_id;
        subscription.typing = subscription.activities = subscription.threads = true;
        if (g_app.account.instance && g_app.account.api.guild_subscribe &&
            g_app.account.api.guild_subscribe(g_app.account.instance, &subscription)) {
            thread_member_list_subscription_sent(&thread_pending, thread_command, now_ms);
            log_u64(LogLevel::info, "gateway", "thread_member_subscription_thread_id",
                    thread_command.thread_id);
        } else
            thread_member_list_subscription_fail(&thread_pending, now_ms);
    }
    if (ordinary_thread) {
        member_list_subscription_complete(&pending);
        guild_member_store_clear_visible_range(&ui.guild_member_store);
        // Thread snapshots have no positional paging contract. Only local
        // materialized users are pinned; scrolling must never request ranges.
        const bool pin_visible =
            visible && active_thread && scene.active && scene.source == &ui.member_list &&
            ui.member_list.thread_mode && ui.member_list.guild_id == selected->guild_id &&
            ui.member_list.list_id == selected->id && scene.source_revision == ui.member_list.revision;
        if (thread_pending.visible_pins != pin_visible ||
            thread_pending.member_pin_revision != ui.guild_member_store.revision ||
            thread_pending.user_pin_revision != ui.user_store.revision ||
            thread_pending.projection_pin_revision != ui.member_list.revision ||
            (pin_visible && (thread_pending.profile_first != scene.first ||
                             thread_pending.profile_count != scene.count))) {
            guild_member_store_clear_pins(&ui.guild_member_store);
            user_store_clear_pins(&ui.user_store);
            if (pin_visible)
                for (u32 i = 0; i < scene.count; ++i)
                    if (member_list_scene_row_resident(scene, i) &&
                        scene.rows[i].kind == MemberListRowKind::member) {
                        user_store_pin(&ui.user_store, scene.rows[i].user_id);
                        guild_member_store_pin_user(&ui.guild_member_store, selected->guild_id,
                                                            scene.rows[i].user_id);
                    }
            thread_pending.visible_pins = pin_visible;
            thread_pending.member_pin_revision = ui.guild_member_store.revision;
            thread_pending.user_pin_revision = ui.user_store.revision;
            thread_pending.projection_pin_revision = ui.member_list.revision;
            thread_pending.profile_first = pin_visible ? scene.first : 0;
            thread_pending.profile_count = pin_visible ? scene.count : 0;
        }
        return true;
    }
    if (thread_pending.visible_pins) {
        guild_member_store_clear_user_pins(&ui.guild_member_store);
        thread_pending.visible_pins = false;
    }
    const u64 prior_list_id = g_app.member_list_identity.channel_id == state.selected_channel_id
                                  ? g_app.member_list_identity.list_id
                                  : 0;
    const u64 list_id =
        selected ? member_list_channel_cached_id(&g_app.member_list_identity, state, selected->id) : 0;
    if (prior_list_id && prior_list_id != list_id)
        member_list_subscription_refresh(&pending, now_ms);
    const bool same_source = selected && ui.member_list.guild_id == selected->guild_id &&
                             ui.member_list.list_id == list_id && ui.member_list.authoritative;
    const bool admitted_source = same_source && scene.active && scene.source == &ui.member_list &&
                                 scene.source_revision == ui.member_list.revision;
    const u32 visible_rows =
        visible ? static_cast<u32>(ui_rect_height(pane->rectangle) / kMemberListRowHeight) + 2 : 0;
    const u32 first_visible = same_source ? static_cast<u32>(scene.scroll_dips / kMemberListRowHeight) : 0;
    const u32 logical_count = same_source ? member_list_view_count(ui.member_list) : 0;
    MemberListPinnedRange desired_visible{};
    if (visible && admitted_source && scene.count)
        desired_visible = {selected->guild_id, list_id, scene.first, scene.first + scene.count - 1};
    const auto& current_visible = ui.guild_member_store.visible;
    const bool visible_interest_changed = current_visible.guild_id != desired_visible.guild_id ||
                                          current_visible.list_id != desired_visible.list_id ||
                                          current_visible.first != desired_visible.first ||
                                          current_visible.last != desired_visible.last;
    // Compute before the planner can reset the retry owner on hide/selection.
    const bool refresh_user_pins = member_list_subscription_pins_stale(
        pending, visible_interest_changed, ui.guild_member_store.revision, ui.user_store.revision);
    const bool planned = member_list_subscription_viewport(&pending, selected ? selected->guild_id : 0,
                                                           selected ? selected->id : 0, logical_count,
                                                           first_visible, visible_rows, visible, now_ms);
    if (visible_interest_changed) {
        guild_member_store_clear_visible_range(&ui.guild_member_store);
        if (desired_visible.guild_id &&
            !guild_member_store_set_visible_range(&ui.guild_member_store, desired_visible.guild_id,
                                                          desired_visible.list_id, desired_visible.first,
                                                          desired_visible.last))
            log_write(LogLevel::warning, "gateway", "member_visible_interest_exceeds_capacity");
    }
    if (refresh_user_pins)
        guild_member_store_refresh_visible_users(ui.guild_member_store, &ui.user_store);
    pending.member_pin_revision = ui.guild_member_store.revision;
    pending.user_pin_revision = ui.user_store.revision;
    if (!planned) {
        log_write(LogLevel::warning, "gateway", "member_viewport_exceeds_resident_capacity");
        return true;
    }
    // Only the positional ranges are replanned here. Members' visible pins
    // belong to the author interest below, which reapplies them when it changes.
    guild_member_store_clear_range_pins(&ui.guild_member_store);
    for (u32 i = 0; i < pending.range_count; ++i)
        guild_member_store_pin_range(&ui.guild_member_store, pending.guild_id, list_id,
                                             pending.ranges[i].first, pending.ranges[i].last);
    if (visible && admitted_source &&
        (pending.profile_first != scene.first || pending.profile_count != scene.count)) {
        pending.profile_first = scene.first;
        pending.profile_count = scene.count;
        bool missing_profile = false;
        for (u32 i = 0; i < scene.count; ++i)
            if (member_list_scene_row_resident(scene, i) &&
                scene.rows[i].kind == MemberListRowKind::member &&
                !user_store_find(ui.user_store, scene.rows[i].user_id))
                missing_profile = true;
        if (missing_profile && pending.sent_range_count && !pending.retry_at_ms)
            member_list_subscription_refresh(&pending, now_ms);
    }
    if (!member_list_subscription_due(pending, now_ms))
        return true;
    u64 channel_id = pending.channel_id;
    if (selected && selected->type == 10 && selected->parent_id)
        channel_id = selected->parent_id;
    // As in the official client: a remembered channel with unchanged ranges writes
    // nothing (its list kept its own rows); otherwise the frame carries all of
    // the guild's remembered channels. Restored if the frame is not admitted.
    const MemberListChannelMemory channels_before = g_app.member_list_channels;
    const auto* held = guild_member_store_list(ui.guild_member_store, pending.guild_id, list_id);
    GatewayGuildSubscription subscription{};
    if (!member_list_channel_memory_subscribe(&g_app.member_list_channels, pending.guild_id, channel_id,
                                              pending.ranges, pending.range_count, !held || !held->authoritative,
                                              &subscription)) {
        member_list_subscription_sent(&pending);
        return true;
    }
    subscription.thread_member_lists_present = true;
    if (!g_app.account.instance || !g_app.account.api.guild_subscribe ||
        !g_app.account.api.guild_subscribe(g_app.account.instance, &subscription)) {
        g_app.member_list_channels = channels_before;
        member_list_subscription_fail(&pending, now_ms);
        log_u64(LogLevel::warning, "gateway", "guild_member_subscription_retry_at_ms", pending.retry_at_ms);
        return true;
    }
    log_u64(LogLevel::info, "gateway", "guild_member_subscription_channel_id", subscription.channel_id);
    for (u32 i = 0; i < subscription.range_count; ++i) {
        TraceRecord trace("guild_member_subscription_range_queued");
        trace.field("channel_id", subscription.channel_id);
        trace.field("first", static_cast<u64>(subscription.ranges[i].first));
        trace.field("last", static_cast<u64>(subscription.ranges[i].last));
        trace.write("gateway");
    }
    log_u64(LogLevel::info, "gateway", "guild_member_subscription_list_id",
            member_list_channel_cached_id(&g_app.member_list_identity, state, pending.channel_id));
    member_list_subscription_sent(&pending);
    return true;
}

} // namespace

u64 application_author_hydration_next_wake() noexcept {
    if (!author_hydration_visible())
        return 0;
    const auto& ui = *g_app.account_ui;
    const auto& owner = ui.author_hydration;
    return ui.active_channel_id == owner.channel_id &&
                   ui.chat_state.selected_channel_id == owner.channel_id &&
                   ui.chat_state.selected_guild_id == owner.guild_id
               ? owner.next_at_ms
               : 0;
}

// Pin epochs seen when the author interest was last applied: any clear of
// member or user pins since (member list events, the thread planner, a
// channel switch) makes the interest reapply its pins.
static u32 g_author_member_pin_epoch;
static u16 g_author_user_pin_epoch;
// The people whose pins were last applied (authors, voice users, list rows).
static u64 g_author_people;

void synchronize_visible_author_hydration(u64 now_ms, bool pins_reset) noexcept {
    if (!g_app.account_ui)
        return;
    auto& ui = *g_app.account_ui;
    pins_reset = pins_reset || g_author_member_pin_epoch != ui.guild_member_store.user_pin_epoch ||
                 g_author_user_pin_epoch != ui.user_store.pin_epoch;
    auto& owner = ui.author_hydration;
    const auto* selected =
        chat_state_store_find_channel(ui.chat_state, ui.chat_state.selected_channel_id);
    const bool eligible = author_hydration_visible() && selected && selected->guild_id &&
                          ui.active_channel_id == selected->id;
    const auto key = visible_author_interest_key(g_app.message_scene, selected ? selected->guild_id : 0,
                                                 selected ? selected->id : 0, eligible);
    // The selected server's voice participants show in the sidebar whatever
    // the content column holds.
    const u64 voice_guild = selected && selected->guild_id && g_app.account.instance && !g_app.login.visible
                                ? selected->guild_id
                                : 0;
    if (!pins_reset && owner.voice_revision == ui.voice_states.revision &&
        visible_author_hydration_interest_current(owner, key, ui.guild_member_store.revision,
                                                  ui.user_store.revision, now_ms))
        return;
    const auto retain_interest = [&]() noexcept {
        owner.interest = key;
        owner.member_pin_revision = ui.guild_member_store.revision;
        owner.user_pin_revision = ui.user_store.revision;
        owner.voice_revision = ui.voice_states.revision;
        owner.interest_current = true;
        g_author_member_pin_epoch = ui.guild_member_store.user_pin_epoch;
        g_author_user_pin_epoch = ui.user_store.pin_epoch;
    };
    u64 ids[kVisibleAuthorMaximum]{};
    const u32 author_count =
        eligible ? visible_author_ids(g_app.message_scene, selected->id, {ids, kVisibleAuthorMaximum}) : 0;
    const u32 voice_end = visible_voice_user_ids(ui.voice_states, voice_guild, {ids, kVisibleAuthorMaximum}, author_count);
    // Visible member list rows keep their user while that member's record or
    // profile may have been evicted meanwhile (the list kept its own rows):
    // ask for those members by ID, as the official client does for users it shows.
    u32 count = voice_end;
    const auto& shown = ui.guild_member_store.visible;
    if (selected && shown.guild_id && shown.guild_id == selected->guild_id) {
        u64 rows[128]{};
        const u32 row_count = guild_member_store_list_users(ui.guild_member_store, shown.guild_id, shown.list_id,
                                                            shown.first, shown.last, {rows, 128});
        for (u32 i = 0; i < row_count && count < kVisibleAuthorMaximum; ++i) {
            if (guild_member_store_find(ui.guild_member_store, shown.guild_id, rows[i]) &&
                user_store_find(ui.user_store, rows[i]))
                continue;
            bool duplicate = false;
            for (u32 j = 0; j < count && !duplicate; ++j)
                duplicate = ids[j] == rows[i];
            if (!duplicate)
                ids[count++] = rows[i];
        }
    }
    // Scrolling changes the interest key every frame, but the people shown
    // change only as rows cross the viewport. With the same people, scope,
    // revisions and pins, the pins applied last time still stand.
    const u64 people =
        hash64_bytes(hash64_mix(kHash64Seed, (u64{author_count} << 32) | voice_end), ids,
                     count * static_cast<u32>(sizeof(ids[0])));
    if (!pins_reset && people == g_author_people && owner.interest_current &&
        owner.interest.eligible == key.eligible && owner.interest.guild == key.guild &&
        owner.interest.channel == key.channel && owner.voice_revision == ui.voice_states.revision &&
        owner.member_pin_revision == ui.guild_member_store.revision &&
        owner.user_pin_revision == ui.user_store.revision && (!owner.next_at_ms || owner.next_at_ms > now_ms)) {
        retain_interest();
        return;
    }
    g_author_people = people;
    u64 missing[kVisibleAuthorMaximum]{};
    u32 missing_count = 0;
    // Rebuild only finite resident interest, retaining positional range pins.
    // Otherwise scrolling through authors could permanently pin the whole cache.
    guild_member_store_clear_user_pins(&ui.guild_member_store);
    guild_member_store_refresh_visible_users(ui.guild_member_store, &ui.user_store);
    const auto& members = g_app.member_list_scene;
    if (g_app.member_pane_visible && members.active && ui.member_list.thread_mode && selected &&
        ui.member_list.guild_id == selected->guild_id && ui.member_list.list_id == selected->id)
        for (u32 i = 0; i < members.count; ++i)
            if (member_list_scene_row_resident(members, i) &&
                members.rows[i].kind == MemberListRowKind::member) {
                guild_member_store_pin_user(&ui.guild_member_store, selected->guild_id,
                                                    members.rows[i].user_id);
                user_store_pin(&ui.user_store, members.rows[i].user_id);
            }
    for (u32 i = 0; i < count; ++i) {
        const auto* member =
            guild_member_store_find(ui.guild_member_store, selected->guild_id, ids[i]);
        if (member)
            guild_member_store_pin_user(&ui.guild_member_store, selected->guild_id, ids[i]);
        user_store_pin(&ui.user_store, ids[i]);
        // Authors need their roles (name colour), voice rows their profile;
        // list rows were gathered because their record is missing.
        if (i < author_count ? !member || !member->roles_known
            : i < voice_end  ? visible_voice_user_missing(ui.user_store, ui.guild_member_store,
                                                          selected->guild_id, ids[i])
                             : true)
            missing[missing_count++] = ids[i];
    }
    const bool scope_changed =
        eligible && (owner.guild_id != selected->guild_id || owner.channel_id != selected->id);
    if ((!missing_count || scope_changed) && owner.pending_interest) {
        if (g_app.account.api.guild_members_request)
            g_app.account.api.guild_members_request(g_app.account.instance, nullptr);
        owner.pending_interest = false;
    }
    if (!missing_count) {
        owner.next_at_ms = 0;
        owner.retry_at_ms = 0;
        owner.retry_delay_ms = 0;
        retain_interest();
        return;
    }
    GatewayGuildMemberRequest request{};
    if (!visible_author_hydration_plan(&owner, selected->guild_id, selected->id, {missing, missing_count},
                                       now_ms, &request)) {
        retain_interest();
        return;
    }
    const bool admitted = g_app.account.api.guild_members_request &&
                          g_app.account.api.guild_members_request(g_app.account.instance, &request);
    visible_author_hydration_complete(&owner, request, now_ms, admitted);
    retain_interest();
}

bool synchronize_member_subscription(u64 now_ms) noexcept {
    const bool result = synchronize_member_subscription_impl(now_ms);
    // The positional/thread planner above may clear pins; the pin epochs make
    // the author interest reapply them. Author interest is independent of
    // sidebar visibility and is reapplied only from clipped rows.
    synchronize_visible_author_hydration(now_ms);
    return result;
}

GatewayClientPopResult application_pop_account_gateway(void* source, GatewayClientEvent* event,
                                                       Slice<u8> payload_output) noexcept {
    if (!application_notifications_can_pop(&g_app))
        return {0, GatewayClientPopError::empty};
    const GatewayClientPopResult popped = g_app.account.api.gateway_pop(source, event, payload_output);
    if (!popped.ok() || !event) {
        return popped;
    }
    log_member_source_binding(*event);
    application_channel_overview_gateway(
        *event, {reinterpret_cast<const char*>(payload_output.data), popped.payload_bytes});
    if (event->kind == GatewayClientEventKind::ready)
        application_plugins_account_scope_invalidated();
    // Plugin events copy from the cached state before the pump applies the event.
    // An oversized guild record lives outside this buffer; its event goes without a name.
    application_plugins_gateway(*event, popped.payload_bytes <= payload_output.count
                                            ? ConstSlice<u8>{payload_output.data, popped.payload_bytes}
                                            : ConstSlice<u8>{});
    application_friends_gateway(*event, {payload_output.data, popped.payload_bytes});
    application_guild_folders_gateway(*event, {payload_output.data, popped.payload_bytes});
    application_user_settings_gateway(*event, {payload_output.data, popped.payload_bytes});
    application_private_channel_gateway(*event);
    application_stage_gateway(*event, {payload_output.data, popped.payload_bytes});
    application_voice_channels_gateway(*event, popped.payload_bytes <= payload_output.count
                                                   ? ConstSlice<u8>{payload_output.data, popped.payload_bytes}
                                                   : ConstSlice<u8>{});
    application_soundboard_gateway(*event, {payload_output.data, popped.payload_bytes});
    application_guild_community_gateway(*event, {payload_output.data, popped.payload_bytes});
    application_notifications_gateway(&g_app, *event, {payload_output.data, popped.payload_bytes});
    application_composer_gateway_event(event->kind);
    if (event->kind == GatewayClientEventKind::dispatch &&
        event->dispatch_kind == GatewayClientDispatchKind::guild_command_index_update)
        application_commands_invalidate_guild(event->state.guild_id);
    // Observe the mailbox before the selected timeline filters it. Pins can
    // reference messages outside the resident window, including deletions.
    if (event->kind == GatewayClientEventKind::dispatch && popped.payload_bytes <= payload_output.count) {
        const auto kind = event->dispatch_kind;
        const bool pinned_create = kind == GatewayClientDispatchKind::message_create &&
                                   (event->message.fields & gateway_message_field_pinned) &&
                                   (event->message.mention_flags & gateway_client_message_pinned);
        const bool pinned_update = kind == GatewayClientDispatchKind::message_update &&
                                   (event->message.fields & gateway_message_field_pinned);
        if (kind == GatewayClientDispatchKind::channel_pins_update || pinned_create || pinned_update ||
            kind == GatewayClientDispatchKind::message_delete ||
            kind == GatewayClientDispatchKind::message_bulk_delete)
            application_pinned_messages_gateway(event->message.channel_id);
    }
    if (event->kind == GatewayClientEventKind::dispatch &&
        event->dispatch_kind == GatewayClientDispatchKind::command_autocomplete &&
        popped.payload_bytes <= payload_output.count)
        application_commands_gateway(
            {reinterpret_cast<const char*>(payload_output.data), popped.payload_bytes});
    if (event->kind == GatewayClientEventKind::dispatch && popped.payload_bytes <= payload_output.count) {
        const auto kind = event->dispatch_kind;
        if (kind == GatewayClientDispatchKind::interaction_create ||
            kind == GatewayClientDispatchKind::interaction_success ||
            kind == GatewayClientDispatchKind::interaction_failure ||
            kind == GatewayClientDispatchKind::interaction_modal_create)
            application_message_components_gateway(
                kind, {reinterpret_cast<const char*>(payload_output.data), popped.payload_bytes});
        else if (kind == GatewayClientDispatchKind::message_update ||
                 kind == GatewayClientDispatchKind::message_create)
            application_message_components_message(event->message.id);
        else if (kind == GatewayClientDispatchKind::user_note_update)
            application_user_actions_note_gateway(
                {reinterpret_cast<const char*>(payload_output.data), popped.payload_bytes});
    }
    if (event->kind == GatewayClientEventKind::ready) {
        if (g_app.account_ui)
            g_app.account_ui->author_hydration = {};
        thread_member_list_subscription_complete(&g_app.thread_member_list_subscription);
        if (g_app.account_ui && g_app.account_ui->member_list.authoritative &&
            g_app.account_ui->member_list.count)
            g_app.member_list_scene.restore_channel_id =
                g_app.account_ui->chat_state.selected_channel_id
                    ? g_app.account_ui->chat_state.selected_channel_id
                    : (g_app.account_ui->chat_state.restore_channel_id
                           ? g_app.account_ui->chat_state.restore_channel_id
                           : (g_app.account_ui->active_channel_id
                                  ? g_app.account_ui->active_channel_id
                                  : g_app.member_list_scene.restore_channel_id));
        g_app.member_list_scene.focused_user_id = 0;
        ui_widget_manager_focus(&g_app.member_list_scene.widgets, ui_widget_invalid_handle());
        // Discard ownership of old-session history results while still draining
        // their REST response normally. A same-channel restoration must not let
        // an old catchup completion clear the new READY's resync requirement.
        g_app.message_catchup = {};
        g_app.message_older = {};
        g_app.attachment_refresh = {};
        g_app.reply_refresh = {};
    }
    if (event->kind == GatewayClientEventKind::ready || event->kind == GatewayClientEventKind::resumed) {
        application_trace_load_stage(event->kind == GatewayClientEventKind::ready
                                         ? "gateway_ready_received"
                                         : "gateway_resumed_received");
        g_app.member_list_identity = {};
        // The official client rewrites its subscriptions on READY and RESUMED; a
        // fresh session keeps only the guild on screen.
        member_list_channel_memory_connection(
            &g_app.member_list_channels, event->kind == GatewayClientEventKind::ready,
            g_app.account_ui ? g_app.account_ui->chat_state.selected_guild_id : 0);
        member_list_subscription_refresh(&g_app.member_list_subscription, clock_monotonic_milliseconds());
        thread_member_list_subscription_refresh(&g_app.thread_member_list_subscription,
                                                clock_monotonic_milliseconds());
    }
    if (event->kind == GatewayClientEventKind::dispatch &&
        event->dispatch_kind == GatewayClientDispatchKind::guild_emojis_update) {
        log_u64(LogLevel::info, "gateway", "guild_emojis_update_guild_id", event->state.guild_id);
        log_u64(LogLevel::info, "gateway", "guild_emojis_update_count", event->state.emoji_count);
        log_u64(LogLevel::info, "gateway", "guild_emojis_update_role_ids", event->state.emoji_role_count);
    }
    application_log_gateway_event(*event);
    return popped;
}

void application_log_gateway_event(const GatewayClientEvent& event) noexcept {
    switch (event.kind) {
    case GatewayClientEventKind::websocket_opened:
    case GatewayClientEventKind::hello:
    case GatewayClientEventKind::identifying:
    case GatewayClientEventKind::resuming:
    case GatewayClientEventKind::ready:
    case GatewayClientEventKind::resumed:
    case GatewayClientEventKind::reconnecting:
    case GatewayClientEventKind::closed:
    case GatewayClientEventKind::failed:
    case GatewayClientEventKind::stopped:
        log_u64(LogLevel::info, "gateway", "event_kind", static_cast<u64>(event.kind));
        if (event.close_code != 0) {
            log_u64(LogLevel::warning, "gateway", "close_code", event.close_code);
        }
        if (event.error != GatewayClientError::none) {
            log_u64(LogLevel::error, "gateway", "event_error", static_cast<u64>(event.error));
            log_u64(LogLevel::error, "gateway", "event_native_error", event.native_error);
            log_u64(LogLevel::error, "gateway", "failure_stage", static_cast<u64>(event.failure_stage));
            log_u64(LogLevel::error, "gateway", "session_error", static_cast<u64>(event.session_error));
            log_u64(LogLevel::error, "gateway", "handshake_error", static_cast<u64>(event.handshake_error));
            log_u64(LogLevel::error, "gateway", "websocket_error", static_cast<u64>(event.websocket_error));
            log_u64(LogLevel::error, "gateway", "gateway_decode_error",
                    static_cast<u64>(event.gateway_decode_error));
            log_u64(LogLevel::error, "gateway", "event_decode_error",
                    static_cast<u64>(event.event_decode_error));
            log_u64(LogLevel::error, "gateway", "compression_error",
                    static_cast<u64>(event.compression_error));
            log_u64(LogLevel::error, "gateway", "json_scan_error", static_cast<u64>(event.json_scan_error));
            log_u64(LogLevel::error, "gateway", "json_read_error", static_cast<u64>(event.json_read_error));
            log_u64(LogLevel::error, "gateway", "route_error_offset", event.route_error_offset);
            log_u64(LogLevel::error, "gateway", "route_detail", event.route_detail);
        }
        break;
    default:
        if (event.kind == GatewayClientEventKind::dispatch &&
            event.dispatch_kind == GatewayClientDispatchKind::guild_member_list_update) {
            log_u64(LogLevel::info, "gateway", "member_list_records", event.member_list.record_count);
            log_u64(LogLevel::info, "gateway", "member_list_members",
                    event.member_list.member_record_count);
            log_u64(LogLevel::info, "gateway", "member_list_id", event.member_list.list_id);
        }
        if (event.kind == GatewayClientEventKind::dispatch &&
            event.event_decode_error != GatewayEventDecodeError::none) {
            log_u64(LogLevel::warning, "gateway", "dispatch_decode_skipped",
                    static_cast<u64>(event.event_decode_error));
            log_u64(LogLevel::warning, "gateway", "dispatch_scan_error",
                    static_cast<u64>(event.json_scan_error));
            log_u64(LogLevel::warning, "gateway", "dispatch_read_error",
                    static_cast<u64>(event.json_read_error));
            log_u64(LogLevel::warning, "gateway", "dispatch_error_offset", event.route_error_offset);
            log_u64(LogLevel::warning, "gateway", "dispatch_kind", static_cast<u64>(event.dispatch_kind));
            log_u64(LogLevel::warning, "gateway", "dispatch_payload_bytes", event.dispatch_payload_bytes);
            log_u64(LogLevel::warning, "gateway", "dispatch_sequence", event.sequence);
            log_u64(LogLevel::warning, "gateway", "dispatch_event_name_bytes", event.event_name_bytes);
            if (event.dispatch_kind == GatewayClientDispatchKind::unknown && event.event_name_bytes &&
                event.event_name_bytes < kGatewayClientEventNameCapacity) {
                // Protocol event names only ([A-Z0-9_]); anything else is dropped, never echoed.
                char line[24 + kGatewayClientEventNameCapacity] = "dispatch_event_name=";
                u32 used = sizeof("dispatch_event_name=") - 1;
                for (u32 i = 0; i < event.event_name_bytes; ++i) {
                    const char c = event.event_name[i];
                    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
                        line[used++] = c;
                }
                line[used] = 0;
                log_write(LogLevel::warning, "gateway", line);
            }
            log_u64(LogLevel::warning, "gateway", "dispatch_route_detail", event.route_detail);
            if (event.failure_stage == GatewayClientFailureStage::ready_guild_route)
                log_u64(LogLevel::warning, "gateway", "ready_guilds_unavailable", event.native_error);
        }
        break;
    }
}

void application_log_account_observations(const AccountModuleStatus& status, u32 observations) noexcept {
    if ((observations & account_host_observation_phase) != 0) {
        log_u64(LogLevel::info, "account", "coordinator_phase", status.coordinator_phase);
        log_u64(LogLevel::info, "account", "coordinator_error", status.coordinator_error);
        log_u64(LogLevel::info, "account", "rest_error", status.rest_error);
        log_u64(LogLevel::info, "account", "gateway_error", status.gateway_error);
        log_u64(LogLevel::info, "account", "native_error", status.native_error);
        log_u64(LogLevel::info, "account", "required_action", static_cast<u64>(status.required_action));
    }
    if ((observations & account_host_observation_ready) != 0) {
        log_u64(LogLevel::info, "account", "gateway_ready", status.gateway_ready);
        log_u64(LogLevel::info, "account", "gateway_sources", status.gateway_sources);
    }
    if ((observations & account_host_observation_session_saves) != 0) {
        log_u64(LogLevel::info, "account", "session_saves", status.session_saves);
    }
    if ((observations & account_host_observation_session_save_failures) != 0) {
        log_u64(LogLevel::warning, "account", "session_save_failures", status.session_save_failures);
        log_u64(LogLevel::warning, "account", "session_native_error", status.session_native_error);
    }
}

namespace {
void release_account_owners() noexcept {
    application_gif_favorites_release();
    application_emoji_preferences_release();
    application_action_captcha_release();
    application_user_settings_release();
    application_account_settings_release();
}
} // namespace

void application_prepare_account_stop() noexcept {
    UiWidgetManager* const logout_focus =
        application_account_logout_blocked() && g_app.text_form ? g_app.context_focus : nullptr;
    // Retire queued intents and cancel the exact owned flight while its actor
    // and PRIMARY lane still exist. A failed stop retains that tombstone.
    application_thread_actions_release();
    application_scoped_read_menu_clear();
    application_scoped_read_reset();
    reset_message_reaction();
    application_message_history_reset_reaction_turn();
    application_downloads_prepare_account_stop();
    application_self_status_clear();
    application_custom_status_clear();
    application_activity_clear();
    application_notification_controls_clear();
    application_inbox_release();
    application_tooltip_clear();
    application_account_menu_clear();
    application_message_navigation_release();
    application_search_release();
    application_media_picker_release();
    application_video_close();
    application_notifications_account_stop(&g_app);
    application_plugins_account_scope_invalidated();
    application_voice_message_release();
    application_cancel_attachments();
    application_forum_attachments_release();
    application_forum_preview_release();
    application_composer_preview_release();
    ui_button_keyboard_cancel(&g_app.button_keyboard);
    g_app.context_focus = logout_focus;
    renderer_bind_focus_owner(&g_app.renderer, logout_focus);
    ui_scroll_host_cancel(&g_app.scroll_host);
    g_app.member_list_identity = {};
    g_app.member_list_scene.restore_channel_id = 0;
    member_list_subscription_complete(&g_app.member_list_subscription);
    thread_member_list_subscription_complete(&g_app.thread_member_list_subscription);
    if (g_app.account_ui) {
        guild_member_store_clear_pins(&g_app.account_ui->guild_member_store);
        guild_member_store_clear_visible_range(&g_app.account_ui->guild_member_store);
        user_store_clear_pins(&g_app.account_ui->user_store);
    }
    application_pinned_messages_release();
    application_thread_browser_release();
    application_poll_release();
    application_member_moderation_release();
    application_thread_creation_release();
    application_forum_post_release();
    application_message_forward_release();
    application_quick_switcher_release();
    application_channel_overview_release();
    application_channel_memory_stop();
    if (g_app.account_ui) {
        AccountUiRuntime& ui = *g_app.account_ui;
        message_draft_session_persist(ui.message_draft_session, ui.message_drafts,
                                      clock_monotonic_milliseconds(), true);
    }
}

void application_account_stop_observed(const AccountHostStopResult& stopped, bool had_account) noexcept {
    application_log_account_observations(stopped.account_status, stopped.observations);
    if (had_account) {
        log_u64(LogLevel::info, "account", "session_save_skips", stopped.account_status.session_save_skips);
        if (stopped.account_status.error == AccountModuleError::session_store) {
            log_write(LogLevel::warning, "account", "final_session_save_failed");
        }
    }
    if (!stopped.ok()) {
        log_u64(LogLevel::error, "account", "host_stop_error", static_cast<u64>(stopped.error));
        log_u64(LogLevel::error, "account", "host_stop_native_error", stopped.native_error);
        return;
    }
    // The joined stop cleared the account instance. The host can now release
    // retained reaction backing without losing a live completion ticket.
    application_thread_actions_release();
    application_poll_release();
    reset_message_reaction();
    release_account_owners();
}

bool application_stop_account() noexcept {
    // The asynchronous owner must join before even reading the borrowed host.
    if (application_account_logout_blocked() && !application_account_logout_shutdown())
        return false;
    if (!application_account_switch_shutdown())
        return false;
    application_prepare_account_stop();
    AccountHost& host = g_app.account;
    if (!host.module) {
        release_account_owners();
        return true;
    }
    const bool had_account = host.instance != nullptr;
    const AccountHostStopResult stopped = account_host_stop(
        &host, kAccountStopTimeoutMs, clock_monotonic_milliseconds(), clock_unix_milliseconds());
    application_account_stop_observed(stopped, had_account);
    return stopped.ok();
}

bool application_hide_account_scenes() noexcept {
    popup_scene_close(&g_app.popup_scene);
    application_plugins_menu_dismissed();
    g_app.popup_owner_kind = PopupOwnerKind::none;
    g_app.message_action_bar.active = false;
    g_app.member_list_scene.active = false;
    g_app.message_scene.active = false;
    g_app.message_composer_scene.active = false;
    message_view_runtime_release(&g_app.message_view);
    return message_view_runtime_initialize(
               &g_app.message_view, ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height), 1, true)
        .ok();
}

bool application_account_publish_logged_out(bool unconfirmed) noexcept {
    if (application_account_logout_host_owned() || g_app.account.instance || g_app.account.login_instance)
        return false;
    // With other saved accounts, logging out continues with the most recent one.
    if (application_account_switch_after_logout())
        return true;
    if (g_app.account_ui)
        application_release_account_ui(g_app);
    const bool shell_reset = application_hide_account_scenes();
    g_app.login.visible = true;
    g_app.login.status =
        unconfirmed ? RendererLoginStatus::signed_out_unconfirmed : RendererLoginStatus::signed_out;
    secure_edit_clear(&g_app.login.password);
    secure_edit_clear(&g_app.login.mfa);
    g_app.login.remote_active = false;
    g_app.login.qr_ready = false;
    g_app.login.remote_status = LoginRemoteStatus::idle;
    g_app.login.remote_restart_at_ms = 0;
    g_app.login.remote_user_bytes = 0;
    clear_bytes(g_app.login.remote_user, sizeof(g_app.login.remote_user));
    g_app.login.qr_code = {};
    g_app.login.reset_mode = false;
    g_app.login.reset_retry = false;
    g_app.login.webauthn_post_epoch = 0;
    g_app.login.browser_launch_epoch = 0;
    g_app.login.focus = LoginControl::login;
    if (!shell_reset || !application_refresh_login_view())
        return false;
    if (!application_login_begin_remote_auth())
        log_write(LogLevel::warning, "auth", "remote_auth_restart_failed");
    application_request_frame();
    return true;
}

bool application_account_return_to_login(bool rejected) noexcept {
    if (application_account_logout_blocked())
        return false;
    // Quiesce workers before releasing their views or removing resume data.
    if (!application_stop_account())
        return false;
    application_release_account_ui(g_app);
    if (!application_hide_account_scenes())
        return false;
    if (rejected) {
        AccountPaths paths{};
        if (account_paths_resolve(&paths).ok()) {
            const wchar_t* invalidated[] = {paths.profile, paths.session};
            for (const auto* path : invalidated)
                if (!DeleteFileW(path) && !win32_error_is_missing_file(thread_last_error()))
                    log_write(LogLevel::warning, "auth", "invalidated_session_removal_failed");
        }
    }
    secure_edit_clear(&g_app.login.password);
    secure_edit_clear(&g_app.login.mfa);
    g_app.login.visible = true;
    g_app.login.status =
        rejected ? RendererLoginStatus::session_expired : RendererLoginStatus::connection_failed;
    g_app.login.focus = LoginControl::login;
    if (!application_refresh_login_view())
        return false;
    // QR login is a new unauthenticated flow; it never retries the rejected token.
    if (!application_login_begin_remote_auth())
        log_write(LogLevel::warning, "auth", "remote_auth_restart_failed");
    application_request_frame();
    return true;
}

bool application_account_module_load() noexcept {
    application_account_switch_initialize();
    if (!application_account_logout_prepare_startup())
        return false;
    const i64 trace_start = clock_now();
    const AccountHostResult loaded = account_host_load(&g_app.account);
    if (!loaded.ok()) {
        log_u64(LogLevel::error, "account", "host_load_error", static_cast<u64>(loaded.error));
        log_u64(LogLevel::error, "account", "host_load_native_error", loaded.native_error);
    }
    if (loaded.ok() && !g_app.account.instance && !g_app.account.login_instance)
        g_app.account.api.set_rest_trace([](const char* record) noexcept {
            // Voice records arrive as "voice:<I|W|E> <event> key=value ...".
            if (record && record[0] == 'v' && record[1] == 'o' && record[2] == 'i' && record[3] == 'c' &&
                record[4] == 'e' && record[5] == ':' && record[6] && record[7] == ' ') {
                const LogLevel level = record[6] == 'E'   ? LogLevel::error
                                       : record[6] == 'W' ? LogLevel::warning
                                                          : LogLevel::info;
                log_write(level, "voice", record + 8);
                return;
            }
            log_write(LogLevel::info, "rest_trace", record);
        });
    TraceRecord module_trace("module_load");
    module_trace.field("elapsed_us", clock_microseconds(g_app.clock, trace_start, clock_now()));
    module_trace.field("ok", static_cast<u64>(loaded.ok()));
    module_trace.write("load_trace");
    return loaded.ok();
}

void application_account_module_release() noexcept {
    if (application_account_logout_host_owned())
        return;
    if (!g_app.account.instance && !g_app.account.login_instance) {
        const AccountHostResult released = account_host_release(&g_app.account);
        if (!released.ok()) {
            log_u64(LogLevel::error, "account", "host_release_error", static_cast<u64>(released.error));
            log_u64(LogLevel::error, "account", "host_release_native_error", released.native_error);
        }
    }
}

namespace {
// The worker starts before the window, renderer and UI exist so DNS, TCP, TLS
// and the Gateway handshake overlap their initialization. Until attach
// publishes the instance, nothing pops its bounded mailbox; the Gateway worker
// waits for space (producer backpressure), so dispatches arrive in order.
struct AccountEarlyStart {
    AccountPaths paths;
    AccountModuleStartResult started;
    void* instance;
    bool failed;
};
AccountEarlyStart g_early_start{};

AccountModuleStartResult start_account_worker(const AccountPaths& paths) noexcept {
    const i64 start_counter = clock_now();
    application_trace_load_stage("account_start");
    const AccountModuleStartResult started = account_host_start(
        &g_app.account, paths.profile, paths.session, clock_monotonic_milliseconds(), clock_unix_milliseconds());
    TraceRecord start_trace("account_start_return");
    start_trace.field("elapsed_us", clock_microseconds(g_app.clock, start_counter, clock_now()));
    start_trace.field("ok", static_cast<u64>(started.ok()));
    start_trace.field("session_loaded", static_cast<u64>(started.session_loaded));
    start_trace.write("load_trace");
    if (!started.ok()) {
        log_u64(LogLevel::error, "account", "module_start_error", static_cast<u64>(started.error));
        log_u64(LogLevel::error, "account", "profile_error", started.profile_error);
        log_u64(LogLevel::error, "account", "module_start_native_error", started.native_error);
    }
    return started;
}

void publish_started_account(const AccountPaths& paths, const AccountModuleStartResult& started) noexcept {
    AccountHost& host = g_app.account;
    g_app.login.visible = true;
    g_app.login.status = RendererLoginStatus::restoring_session;
    g_app.login.focus = LoginControl::none;
    if (!application_synchronize_login_view()) {
        log_write(LogLevel::error, "ui", "login_view_hide_failed");
    }
    const u32 observations = account_host_observe(&host, started.status);
    application_log_account_observations(started.status, observations);
    AccountUiRuntime& ui = *g_app.account_ui;
    if (!message_draft_session_configure(ui.message_draft_session, ui.message_drafts, paths.drafts,
                                         started.status.profile_id, clock_monotonic_milliseconds())) {
        log_write(LogLevel::warning, "draft", "persistence_configuration_failed");
    }
    application_channel_memory_start(started.status.profile_id);
    log_u64(LogLevel::info, "account", "profile_id", started.status.profile_id);
    log_u64(LogLevel::info, "account", "module_runtime_bytes", host.api.runtime_bytes());
    log_u64(LogLevel::info, "account", "ui_runtime_bytes", sizeof(AccountUiRuntime));
    log_u64(LogLevel::info, "account", "session_loaded", started.session_loaded ? 1 : 0);
    if (!started.session_loaded && !win32_error_is_missing_file(started.status.session_native_error)) {
        log_u64(LogLevel::warning, "account", "session_load_error", started.session_error);
        log_u64(LogLevel::warning, "account", "session_load_native_error",
                started.status.session_native_error);
    }
}
} // namespace

bool application_account_restart() noexcept {
    AccountHost& host = g_app.account;
    if (!g_app.account_ui || g_app.login.visible || host.instance || host.login_instance || host.stop_failed ||
        application_account_logout_blocked())
        return false;
    AccountPaths paths{};
    if (!account_paths_resolve(&paths).ok() || (!host.module && !application_account_module_load()))
        return false;
    application_plugins_account_scope_invalidated();
    const AccountModuleStartResult started = start_account_worker(paths);
    if (!started.ok())
        return false;
    // The cached shell stays up and usable; READY (or RESUMED) replaces its state.
    application_log_account_observations(started.status, account_host_observe(&host, started.status));
    AccountUiRuntime& ui = *g_app.account_ui;
    if (!message_draft_session_configure(ui.message_draft_session, ui.message_drafts, paths.drafts,
                                         started.status.profile_id, clock_monotonic_milliseconds()))
        log_write(LogLevel::warning, "draft", "persistence_configuration_failed");
    application_channel_memory_start(started.status.profile_id);
    log_write(LogLevel::info, "account", "session_restarted");
    return true;
}

bool application_account_prestart() noexcept {
    // Selects the saved account's slot before any account path resolves.
    application_account_switch_initialize();
    AccountEarlyStart& early = g_early_start;
    if (early.instance || early.failed || g_app.account.instance || g_app.account.login_instance ||
        application_account_logout_locked() || !account_paths_resolve(&early.paths).ok())
        return false;
    // Pending logout recovery and every diagnostic path need the UI; attach
    // repeats these probes with logging when this early start declines.
    const AccountCredentialForgetResult forget = account_credential_forget_probe(early.paths);
    const DWORD attributes = GetFileAttributesW(early.paths.profile);
    if (!forget.ok() || forget.pending || attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;
    early.failed = true;
    if (!application_account_module_load())
        return false;
    early.started = start_account_worker(early.paths);
    if (!early.started.ok()) {
        application_account_module_release();
        return false;
    }
    early.instance = g_app.account.instance;
    g_app.account.instance = nullptr;
    early.failed = false;
    return true;
}

void application_account_prestart_cancel() noexcept {
    AccountEarlyStart& early = g_early_start;
    if (early.instance) {
        g_app.account.instance = early.instance;
        const AccountHostStopResult stopped = account_host_stop(
            &g_app.account, kAccountStopTimeoutMs, clock_monotonic_milliseconds(), clock_unix_milliseconds());
        application_account_stop_observed(stopped, true);
        application_account_module_release();
    }
    early = {};
}

bool application_account_prestart_pending() noexcept {
    return g_early_start.instance != nullptr;
}

bool application_account_attach_if_present() noexcept {
    application_account_switch_initialize();
    AccountEarlyStart& early = g_early_start;
    if (early.failed) {
        early = {};
        return true;
    }
    if (early.instance) {
        const bool ui_ready = application_initialize_account_ui(g_app);
        MAICORD_PERF_MARK("account_ui_initialized");
        if (!ui_ready || !rebuild_account_text_cache()) {
            application_release_account_ui(g_app);
            log_u64(LogLevel::error, "account", "ui_runtime_allocation_bytes", sizeof(AccountUiRuntime));
            application_account_prestart_cancel();
            // UI startup left the sign-in form for this attach to replace.
            if (!application_synchronize_login_view())
                log_write(LogLevel::error, "ui", "login_view_restore_failed");
            return true;
        }
        MAICORD_PERF_MARK("account_text_cache");
        g_app.account.instance = early.instance;
        early.instance = nullptr;
        application_plugins_account_scope_invalidated();
        publish_started_account(early.paths, early.started);
        early = {};
        return true;
    }
    AccountPaths paths{};
    const AccountPathsResult resolved = account_paths_resolve(&paths);
    if (!resolved.ok()) {
        log_u64(LogLevel::warning, "account", "path_error", static_cast<u64>(resolved.error));
        log_u64(LogLevel::warning, "account", "path_native_error", resolved.native_error);
        application_account_logout_prepare_startup();
        return true;
    }
    if (!application_account_logout_prepare_startup(&paths))
        return true;
    const DWORD profile_attributes = GetFileAttributesW(paths.profile);
    if (profile_attributes == INVALID_FILE_ATTRIBUTES) {
        const u32 error = thread_last_error();
        if (win32_error_is_missing_file(error)) {
            log_write(LogLevel::info, "account", "normal_user_profile_absent");
            return true;
        }
        log_u64(LogLevel::warning, "account", "profile_probe_native_error", error);
        return true;
    }
    if ((profile_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        log_write(LogLevel::error, "account", "profile_path_is_directory");
        return true;
    }

    if (!application_account_module_load()) {
        return true;
    }
    if (!application_initialize_account_ui(g_app) || !rebuild_account_text_cache()) {
        application_release_account_ui(g_app);
        log_u64(LogLevel::error, "account", "ui_runtime_allocation_bytes", sizeof(AccountUiRuntime));
        application_account_module_release();
        return true;
    }
    application_plugins_account_scope_invalidated();
    const AccountModuleStartResult started = start_account_worker(paths);
    if (!started.ok()) {
        AccountUiRuntime& ui = *g_app.account_ui;
        message_draft_session_persist(ui.message_draft_session, ui.message_drafts,
                                      clock_monotonic_milliseconds(), true);
        application_release_account_ui(g_app);
        application_account_module_release();
        return true;
    }
    publish_started_account(paths, started);
    return true;
}

} // namespace maicord::app_detail
