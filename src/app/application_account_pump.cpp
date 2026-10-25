#include "application_load_trace.hpp"
#include "application_channel_snapshots.hpp"
#include "maicord/app/account_host.hpp"
#include "maicord/app/message_draft_session.hpp"
#include "maicord/app/message_event_pump.hpp"
#include "maicord/app/presence_coalescer.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/trace_record.hpp"
#include "maicord/core/byte_range.hpp"

#include "application_account_module.hpp"
#include "application_account_logout.hpp"
#include "application_account_reconnect.hpp"
#include "application_voice.hpp"
#include "application_account_presentation.hpp"
#include "application_thread_actions.hpp"
#include "application_thread_browser.hpp"
#include "application_thread_creation.hpp"
#include "application_forum_post.hpp"
#include "application_forum_form.hpp"
#include "application_thread_header.hpp"
#include "application_friends.hpp"
#include "application_channel_memory.hpp"
#include "application_channel_session.hpp"
#include "application_startup_shell.hpp"
#include "application_media.hpp"
#include "application_message_history.hpp"
#include "application_message_reaction.hpp"
#include "application_message_send.hpp"
#include "application_private_conversation.hpp"
#include "application_private_channel.hpp"
#include "application_state.hpp"
#include "application_login_ui.hpp"
#include "application_plugins.hpp"
#include "application_read_state.hpp"
#include "application_scoped_read.hpp"
#include "application_scoped_read_menu.hpp"
#include "application_notifications.hpp"
#include "application_reply_draft.hpp"
#include "application_message_navigation.hpp"
#include "application_poll.hpp"
#include "application_frame_presentation.hpp"
#include "maicord/app/user_account_coordinator.hpp"

#include "maicord/core/memory.hpp"

namespace maicord::app_detail {
namespace {
PresenceCoalescer g_presence{};

// Gateway events the pump skipped for one account instance. A skipped event
// never stops the session; one that may leave account-wide state stale asks
// the module for a fresh session state (bounded by its recovery budget) and
// shows a non-blocking notice until a clean READY replaces the state.
enum class AccountSyncPhase : u8 { healthy, recovering, stale };
struct AccountSyncHealth {
    const void* instance;
    u32 skipped, logged;
    AccountSyncPhase phase;
    bool recovery_requested;
};
AccountSyncHealth g_sync{};
constexpr u32 kAccountSyncLoggedFailures = 64;

void log_skipped_events(u32 source_index, const MessageEventPumpDrainResult& drained) noexcept {
    g_sync.skipped += drained.apply_failures;
    if (g_sync.logged >= kAccountSyncLoggedFailures)
        return;
    ++g_sync.logged;
    // Kinds, the failed check and sizes only: never payload contents.
    const auto& failure = drained.failure;
    TraceRecord trace("event_skipped");
    trace.field("source", source_index);
    trace.field("event_kind", static_cast<u64>(failure.kind));
    trace.field("dispatch_kind", static_cast<u64>(failure.dispatch_kind));
    trace.field("reason", message_event_pump_reject_reason_name(failure.reason));
    trace.field("detail", failure.detail);
    trace.field("payload_bytes", failure.payload_bytes);
    trace.field("sequence", failure.sequence);
    trace.field("in_drain", drained.apply_failures);
    trace.field("session_total", g_sync.skipped);
    trace.field("state_incomplete", drained.state_incomplete ? u64{1} : u64{0});
    log_write(LogLevel::error, "account", trace.text);
}

// True when the notice changed.
bool observe_sync_health(AccountHost& host, const MessageEventPumpDrainResult& drained) noexcept {
    const AccountSyncPhase before = g_sync.phase;
    // A clean READY replaced the whole account state.
    if (drained.ready_applied && !drained.state_incomplete) {
        g_sync.phase = AccountSyncPhase::healthy;
        g_sync.recovery_requested = false;
    }
    if (drained.state_incomplete && g_sync.phase == AccountSyncPhase::healthy)
        g_sync.phase = AccountSyncPhase::stale;
    if (g_sync.phase != AccountSyncPhase::healthy && !g_sync.recovery_requested && host.api.gateway_refresh) {
        const auto recovery = host.api.gateway_refresh(host.instance);
        if (recovery != GatewayStateRecoveryResult::unavailable) {
            g_sync.recovery_requested = true;
            g_sync.phase = recovery == GatewayStateRecoveryResult::exhausted ? AccountSyncPhase::stale
                                                                             : AccountSyncPhase::recovering;
            log_write(LogLevel::warning, "account",
                      recovery == GatewayStateRecoveryResult::exhausted ? "state_recovery_exhausted"
                                                                        : "state_recovery_requested");
        }
    }
    return before != g_sync.phase;
}

// Member rows and message authors currently materialized on screen. Guild
// presence for anyone else updates the model without invalidating text.
bool presence_visible(void*, u64 guild_id, u64 user_id) noexcept {
    const AccountUiRuntime* ui = g_app.account_ui;
    if (!ui)
        return false;
    if (user_id == ui->chat_state.self_user_id ||
        (g_app.renderer.profile_scene && g_app.renderer.profile_scene->active))
        return true;
    // Private conversation rows show the overall status, which any server's
    // presence updates; message authors stay scoped to the selected server.
    const bool any_scope = !ui->chat_state.selected_guild_id;
    if (guild_id != ui->chat_state.selected_guild_id && !any_scope)
        return false;
    const MemberListScene& members = g_app.member_list_scene;
    if (members.active)
        for (u32 index = 0; index < members.count && index < members.rows.count; ++index)
            if (members.rows[index].user_id == user_id)
                return true;
    if (guild_id != ui->chat_state.selected_guild_id)
        return false;
    const MessageScene& messages = g_app.message_scene;
    if (messages.active && messages.timeline && messages.timeline->store)
        for (u32 index = 0; index < messages.count && index < messages.rows.count; ++index) {
            const MessageRecord* record = message_store_resolve(*messages.timeline->store, messages.rows[index].message);
            if (record && record->author_id == user_id)
                return true;
        }
    return false;
}

bool presence_pending() noexcept {
    const AccountUiRuntime* ui = g_app.account_ui;
    return ui && (g_app.message_pump.presence_visible_dirty ||
                  ui->member_list.source_presence_revision != ui->guild_member_store.presence_revision);
}

// Applies coalesced presence: in-place member rows, then one text refresh only
// when an on-screen row or author changed. True requests that refresh.
bool flush_presence(u64 now_ms) noexcept {
    if (!g_app.account_ui || !presence_coalescer_take(&g_presence, now_ms))
        return false;
    AccountUiRuntime& ui = *g_app.account_ui;
    bool visible = g_app.message_pump.presence_visible_dirty;
    if (visible) {
        g_app.message_pump.presence_visible_dirty = false;
        ++ui.user_store.revision;
    }
    const MemberListScene& scene = g_app.member_list_scene;
    const MemberListPresenceResult rows = member_list_view_refresh_presence(
        &ui.member_list, ui.guild_member_store, scene.active ? scene.first : 0, scene.active ? scene.count : 0);
    return visible || rows.visible;
}

struct BrowserGatewayScope {
    u64 guild, parent;
};
struct BrowserGatewayDrain {
    void* source;
    const ChatStateStore* state;
    BrowserGatewayScope pending_scope, scopes[8];
    u64 pending_thread;
    u64 navigation_deleted;
    const AccountUiRuntime* navigation_ui;
    void* navigation_instance;
    u64 navigation_scope, navigation_profile, navigation_self;
    u32 channels, permissions, guilds, count;
    u64 poll_sequence;
    u64 poll_ready_self;
    u8 source_index;
    bool poll_has_sequence, poll_checked, poll_current, poll_batched;
    bool pending, force, all, forum_metadata, navigation_message_receipt;
};
BrowserGatewayDrain* navigation_gateway_drain;
bool navigation_gateway_scope_current(const BrowserGatewayDrain& drain) noexcept {
    return g_app.account_ui == drain.navigation_ui && g_app.account.instance == drain.navigation_instance &&
           g_app.plugin_account_scope_serial == drain.navigation_scope &&
           g_app.account.status.profile_id == drain.navigation_profile &&
           (!g_app.account_ui || g_app.account_ui->chat_state.self_user_id == drain.navigation_self);
}
bool accept_poll_receipt() noexcept {
    auto* drain = navigation_gateway_drain;
    if (!drain || !g_app.account_ui || !navigation_gateway_scope_current(*drain) ||
        drain->source_index >= 8)
        return false;
    if (drain->poll_checked)
        return drain->poll_current;
    drain->poll_checked = true;
    auto& ui = *g_app.account_ui;
    const u8 bit = static_cast<u8>(1u << drain->source_index);
    if (drain->poll_has_sequence) {
        // Votes expanded from one MESSAGE_POLL_VOTE_ADD_MANY share its sequence.
        const u64 last = ui.poll_source_sequences[drain->source_index];
        if ((ui.poll_source_sequence_mask & bit) &&
            (drain->poll_sequence < last || (drain->poll_sequence == last && !drain->poll_batched)))
            return false;
        ui.poll_source_sequences[drain->source_index] = drain->poll_sequence;
        ui.poll_source_sequence_mask |= bit;
    }
    drain->poll_current = true;
    if (++ui.poll_gateway_serial == 0)
        ++ui.poll_gateway_serial;
    return true;
}
void collect_browser_gateway_scope(BrowserGatewayDrain& drain, BrowserGatewayScope scope) noexcept {
    if (drain.all)
        return;
    if (!scope.guild && !scope.parent) {
        drain.all = true;
        drain.count = 0;
        return;
    }
    for (u32 i = 0; i < drain.count; ++i)
        if (drain.scopes[i].guild == scope.guild && drain.scopes[i].parent == scope.parent)
            return;
    if (drain.count == 8) {
        drain.all = true;
        drain.count = 0;
        return;
    }
    drain.scopes[drain.count++] = scope;
}
void finish_browser_gateway_receipt(BrowserGatewayDrain& drain) noexcept {
    // A validated initial READY may establish the actor when none is set.
    // Adopt it only after the state store has committed that same actor.
    if (drain.poll_ready_self && !drain.navigation_self && g_app.account_ui == drain.navigation_ui &&
        g_app.account.instance == drain.navigation_instance &&
        g_app.plugin_account_scope_serial == drain.navigation_scope &&
        g_app.account.status.profile_id == drain.navigation_profile && g_app.account_ui &&
        g_app.account_ui->chat_state.self_user_id == drain.poll_ready_self)
        drain.navigation_self = drain.poll_ready_self;
    drain.poll_ready_self = 0;
    if (!drain.pending)
        return;
    const auto* state = g_app.account_ui ? &g_app.account_ui->chat_state : nullptr;
    // These snapshots bracket the previous event's actual model application,
    // not an unrelated message later in the same pump. Core conservatively
    // advances channel stamps for accepted updates, including repeated values.
    const bool changed = drain.force || state != drain.state ||
                         (state && (state->channel_mutations != drain.channels ||
                                    state->permission_mutations != drain.permissions ||
                                    state->guild_mutations != drain.guilds));
    if (changed)
        collect_browser_gateway_scope(drain, drain.pending_scope);
    // Core does not keep forum tags or archive defaults. Even a channel
    // update with unchanged Core fields can replace this metadata.
    if (changed || drain.forum_metadata)
        application_forum_post_gateway(drain.pending_scope.guild, drain.pending_scope.parent,
                                       drain.pending_thread);
    if (drain.navigation_deleted && navigation_gateway_scope_current(drain))
        application_message_navigation_gateway_deleted(drain.pending_scope.guild, drain.navigation_deleted);
    drain.pending = false;
}
GatewayClientPopResult pop_browser_gateway(void* opaque, GatewayClientEvent* event,
                                           Slice<u8> payload_output) noexcept {
    auto& drain = *static_cast<BrowserGatewayDrain*>(opaque);
    finish_browser_gateway_receipt(drain);
    drain.state = g_app.account_ui ? &g_app.account_ui->chat_state : nullptr;
    drain.channels = drain.state ? drain.state->channel_mutations : 0;
    drain.permissions = drain.state ? drain.state->permission_mutations : 0;
    drain.guilds = drain.state ? drain.state->guild_mutations : 0;
    const auto popped = application_pop_account_gateway(drain.source, event, payload_output);
    drain.navigation_message_receipt = false;
    if (!popped.ok() || !event)
        return popped;
    // A startup shell saved for another account must not reject READY's.
    if (event->kind == GatewayClientEventKind::ready && application_startup_shell_gateway_ready(event->state.id))
        drain.navigation_self = 0;
    drain.poll_sequence = event->sequence;
    drain.poll_has_sequence = event->has_sequence;
    drain.poll_batched = event->kind == GatewayClientEventKind::dispatch &&
                         event->dispatch_kind == GatewayClientDispatchKind::message_poll_vote_add &&
                         event->poll_vote.batched;
    drain.poll_checked = drain.poll_current = false;
    reset_value(&drain.pending_scope);
    drain.pending_thread = 0;
    drain.navigation_deleted = 0;
    drain.force = false;
    drain.forum_metadata = false;
    GatewayClientEvent decoded{};
    ConstSlice<u8> decoded_payload{};
    const bool decoded_receipt =
        event->error == GatewayClientError::none &&
        event->event_decode_error == GatewayEventDecodeError::none &&
        event->gateway_decode_error == GatewayDecodeError::none &&
        popped.payload_bytes <= payload_output.count &&
        gateway_client_notification_trim(*event, {payload_output.data, popped.payload_bytes}, &decoded,
                                         &decoded_payload);
    drain.navigation_message_receipt =
        decoded_receipt && navigation_gateway_scope_current(drain) &&
        decoded.kind == GatewayClientEventKind::dispatch &&
        (decoded.dispatch_kind == GatewayClientDispatchKind::message_delete ||
         decoded.dispatch_kind == GatewayClientDispatchKind::message_bulk_delete);
    // Cancel the shared navigation intent before a fresh READY can reset its
    // source. Reconnect/RESUMED keep the current session's request guards
    // and do not enter this branch.
    if (decoded_receipt && navigation_gateway_scope_current(drain) &&
        event->kind == GatewayClientEventKind::ready && event->state.id &&
        (!drain.state || !drain.state->self_user_id || drain.state->self_user_id == event->state.id)) {
        u32 guild_count{};
        const auto scratch = g_app.message_pump.guild_id_scratch;
        bool ready_valid =
            gateway_client_event_as_ready_guilds(decoded, decoded_payload, scratch, &guild_count);
        for (u32 i = 0; ready_valid && i < guild_count; ++i)
            for (u32 j = 0; j < i; ++j)
                ready_valid = ready_valid && scratch[i] != scratch[j];
        if (ready_valid) {
            if (!drain.navigation_self)
                drain.poll_ready_self = event->state.id;
            application_message_navigation_gateway_reset();
            g_app.account_ui->poll_source_sequence_mask &= static_cast<u8>(~(1u << drain.source_index));
            if (++g_app.account_ui->poll_gateway_serial == 0)
                ++g_app.account_ui->poll_gateway_serial;
            application_poll_gateway_reset();
            // A new session cannot replay what cached channels missed.
            application_channel_snapshots_reset();
        }
    }
    application_account_connection_observe(event->kind);
    if (event->kind == GatewayClientEventKind::ready ||
        event->kind == GatewayClientEventKind::reconnecting ||
        event->kind == GatewayClientEventKind::closed || event->kind == GatewayClientEventKind::failed ||
        event->kind == GatewayClientEventKind::stopped) {
        drain.pending = drain.force = true;
        return popped;
    }
    if (event->kind != GatewayClientEventKind::dispatch)
        return popped;
    switch (event->dispatch_kind) {
    case GatewayClientDispatchKind::private_state_incomplete:
    case GatewayClientDispatchKind::ready_state_complete:
    case GatewayClientDispatchKind::supplemental_state_complete:
        drain.pending = drain.force = true;
        break;
    case GatewayClientDispatchKind::channel_create:
    case GatewayClientDispatchKind::channel_update:
    case GatewayClientDispatchKind::channel_delete: {
        const auto* previous =
            drain.state ? chat_state_store_find_channel(*drain.state, event->state.id) : nullptr;
        const bool thread = (event->state.fields & gateway_client_state_field_type)
                                ? channel_is_thread(event->state.type)
                                : previous && channel_is_thread(previous->type);
        const u64 guild = event->state.guild_id ? event->state.guild_id : previous ? previous->guild_id : 0;
        const bool category = (event->state.fields & gateway_client_state_field_type)
                                  ? event->state.type == 4
                                  : previous && previous->type == 4;
        const bool unknown_delete = event->dispatch_kind == GatewayClientDispatchKind::channel_delete &&
                                    !(event->state.fields & gateway_client_state_field_type) && !previous;
        const u64 parent = category || unknown_delete ? 0
                           : thread                   ? (event->state.parent_id ? event->state.parent_id
                                                         : previous             ? previous->parent_id
                                                                                : 0)
                                                      : event->state.id;
        drain.pending_scope = {guild, parent};
        if (decoded_receipt && event->dispatch_kind == GatewayClientDispatchKind::channel_delete &&
            event->state.id && (event->state.fields & gateway_client_state_field_id) &&
            navigation_gateway_scope_current(drain))
            drain.navigation_deleted = event->state.id;
        drain.pending_thread = thread || unknown_delete ? event->state.id : 0;
        drain.forum_metadata =
            event->dispatch_kind == GatewayClientDispatchKind::channel_update &&
            (((event->state.fields & gateway_client_state_field_type) && event->state.type == 15) ||
             (previous && previous->type == 15));
        drain.pending = true;
        // A deletion of an uncached thread is newer than an in-flight REST page
        // and must stop that page from bringing the thread back.
        drain.force = event->dispatch_kind == GatewayClientDispatchKind::channel_delete;
        break;
    }
    case GatewayClientDispatchKind::thread_self_update: {
        const auto* thread =
            drain.state ? chat_state_store_find_channel(*drain.state, event->thread_self.thread_id)
                        : nullptr;
        drain.pending_scope = {event->thread_self.guild_id, thread ? thread->parent_id : 0};
        drain.pending_thread = event->thread_self.thread_id;
        drain.pending = drain.force = true;
        break;
    }
    case GatewayClientDispatchKind::thread_list_sync:
        drain.pending_scope = {event->thread_sync.guild_id, 0};
        drain.pending = drain.force = true;
        break;
    case GatewayClientDispatchKind::thread_members_update:
    case GatewayClientDispatchKind::thread_member_list_update: {
        const auto* thread =
            drain.state ? chat_state_store_find_channel(*drain.state, event->thread_members.thread_id)
                        : nullptr;
        drain.pending_scope = {event->thread_members.guild_id, thread ? thread->parent_id : 0};
        drain.pending_thread = event->thread_members.thread_id;
        drain.pending = true;
        break;
    }
    case GatewayClientDispatchKind::guild_create:
    case GatewayClientDispatchKind::guild_update:
    case GatewayClientDispatchKind::guild_delete:
        drain.pending_scope = {event->state.id, 0};
        drain.pending = true;
        drain.force = event->dispatch_kind == GatewayClientDispatchKind::guild_delete;
        break;
    case GatewayClientDispatchKind::guild_role_create:
    case GatewayClientDispatchKind::guild_role_update:
    case GatewayClientDispatchKind::guild_role_delete:
    case GatewayClientDispatchKind::guild_member_update:
        drain.pending_scope = {event->state.guild_id, 0};
        drain.pending = true;
        break;
    default:
        break;
    }
    return popped;
}

void observe_message_reaction(void* context, const GatewayMessageReactionEvent& event, MessageHandle source,
                              u32 revision, bool model_applied) noexcept {
    if (context == &g_app)
        application_message_reaction_observe(event, source, revision, model_applied);
}

void observe_reaction_source(void* context, const GatewayMessageEvent& event, MessageHandle source,
                             u32 revision, bool model_applied) noexcept {
    if (context == &g_app) {
        application_message_reaction_message_observe(event, source, revision, model_applied);
        application_forum_post_message_observe(event);
        const bool poll_resident =
            model_applied ||
            (g_app.account_ui && message_poll_store_find(g_app.account_ui->message_poll_store, event.id));
        if (poll_resident && (event.fields & gateway_message_field_poll) && accept_poll_receipt()) {
            const auto applied =
                message_poll_store_apply(&g_app.account_ui->message_poll_store, event.id, event.channel_id,
                                         event.poll, g_app.account_ui->poll_gateway_serial);
            application_poll_message_observe(event, applied.ok());
        }
    }
}

void observe_reaction_delete(void* context, u64 channel_id, u64 message_id) noexcept {
    if (context == &g_app) {
        application_message_reaction_message_deleted(channel_id, message_id);
        application_reply_draft_deleted(channel_id, message_id);
        application_forum_post_message_deleted(channel_id, message_id);
        if (accept_poll_receipt()) {
            message_poll_store_delete(&g_app.account_ui->message_poll_store, message_id);
            application_poll_message_deleted(channel_id, message_id);
        }
        if (navigation_gateway_drain && navigation_gateway_drain->navigation_message_receipt &&
            navigation_gateway_scope_current(*navigation_gateway_drain))
            application_message_navigation_message_deleted(channel_id, message_id);
    }
}
bool observe_poll_vote(void* context, const GatewayMessagePollVoteEvent& event) noexcept {
    if (context != &g_app || !accept_poll_receipt())
        return false;
    auto& ui = *g_app.account_ui;
    const auto applied = message_poll_store_vote(&ui.message_poll_store, event,
                                                 ui.chat_state.self_user_id, ui.poll_gateway_serial);
    application_poll_observe(event, applied.ok());
    return applied.ok() && applied.changed;
}

void refresh_member_role_authority(void* context, u64 guild_id) noexcept {
    if (context != &g_app || !g_app.account_ui || !guild_id)
        return;
    const auto& state = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, state.selected_channel_id);
    if (!channel || channel->guild_id != guild_id)
        return;
    auto& author = g_app.account_ui->author_hydration;
    if (author.guild_id == guild_id) {
        for (auto& entry : author.cooldowns)
            reset_value(&entry);
        author.retry_at_ms = 0;
        author.retry_delay_ms = 0;
        author.interest_current = false;
    }
    const auto now = clock_monotonic_milliseconds();
    if (g_app.member_list_subscription.guild_id == guild_id)
        member_list_subscription_refresh(&g_app.member_list_subscription, now);
    if (g_app.thread_member_list_subscription.guild_id == guild_id)
        thread_member_list_subscription_refresh(&g_app.thread_member_list_subscription, now);
}

void drain_terminal_gateway_diagnostics() noexcept {
    constexpr u32 maximum_sources = 8;
    AccountHost& host = g_app.account;
    const u32 sources = host.status.gateway_sources;
    if (!sources)
        return;
    if (!host.instance || !host.api.gateway_source || !host.api.gateway_pop || sources > maximum_sources) {
        log_write(LogLevel::error, "gateway", "terminal_diagnostic_source_invalid");
        return;
    }
    const Slice<u8> scratch = g_app.message_pump.payload_scratch;
    if (!scratch.data || !scratch.count || scratch.count > kGatewayClientDispatchPayloadCapacity ||
        !byte_range_valid({scratch.data, scratch.count})) {
        log_write(LogLevel::error, "gateway", "terminal_diagnostic_scratch_invalid");
        return;
    }
    for (u32 source_index = 0; source_index < sources; ++source_index) {
        void* source = host.api.gateway_source(host.instance, source_index);
        if (!source)
            continue;
        for (u32 count = 0; count < kGatewayClientEventCapacity; ++count) {
            GatewayClientEvent event{};
            const auto popped = host.api.gateway_pop(source, &event, scratch);
            if (popped.error == GatewayClientPopError::empty)
                break;
            if (!popped.ok() || popped.payload_bytes > scratch.count) {
                secure_clear_bytes(scratch.data, scratch.count);
                log_u64(LogLevel::error, "gateway", "terminal_diagnostic_pop_error",
                        static_cast<u64>(popped.ok() ? GatewayClientPopError::corrupt : popped.error));
                return;
            }
            if (event.kind == GatewayClientEventKind::failed ||
                event.kind == GatewayClientEventKind::closed)
                application_log_gateway_event(event);
            if (popped.payload_bytes)
                secure_clear_bytes(scratch.data, popped.payload_bytes);
        }
    }
}

// The drain stage that stopped the account, for its log line.
const char* g_drain_failure = "event_drain_failed";
bool drain_failed(const char* stage) noexcept {
    g_drain_failure = stage;
    return false;
}
bool drain_account_events() noexcept {
    AccountHost& host = g_app.account;
    if (!host.instance) {
        return true;
    }
    message_event_pump_set_view_active(&g_app.message_pump, !application_thread_browser_read_blocked() &&
                                                                application_read_state_eligible());
    message_event_pump_set_read_ack_cancel(&g_app.message_pump, application_read_state_cancel, nullptr);
    message_event_pump_set_read_ack_observer(&g_app.message_pump, application_read_state_ack_observed,
                                             nullptr);
    message_event_pump_set_read_token_reset(&g_app.message_pump, application_read_state_reset_token);
    message_event_pump_set_create_observer(&g_app.message_pump, application_notifications_message, &g_app);
    message_event_pump_set_read_badge(&g_app.message_pump, application_notifications_read_badge, &g_app);
    message_event_pump_set_member_roles_refresh(&g_app.message_pump, refresh_member_role_authority, &g_app);
    const bool observe_reactions = application_message_reaction_observation_needed();
    const bool observe_forum = application_forum_post_observation_needed();
    const bool observe_polls = g_app.account_ui != nullptr;
    message_event_pump_set_poll_observer(&g_app.message_pump, observe_polls ? observe_poll_vote : nullptr,
                                         observe_polls ? &g_app : nullptr);
    const bool observe_navigation = application_message_navigation_observation_needed();
    const bool observe_reply_deletes =
        g_app.account_ui && (g_app.account_ui->message_composer_controller.reply_message_id ||
                             g_app.account_ui->message_drafts.count ||
                             g_app.account_ui->message_composer_controller.request_count ||
                             g_app.message_edit.saved_reply_message_id);
    const bool observe_deletes =
        observe_reactions || observe_reply_deletes || observe_forum || observe_navigation || observe_polls;
    message_event_pump_set_reaction_observers(
        &g_app.message_pump, observe_reactions ? observe_message_reaction : nullptr,
        observe_reactions || observe_forum || observe_polls ? observe_reaction_source : nullptr,
        observe_deletes ? observe_reaction_delete : nullptr, observe_deletes ? &g_app : nullptr);
    g_app.message_pump.source_observe_poll_only = !observe_reactions && !observe_forum;
    message_event_pump_set_inactive_observer(&g_app.message_pump, application_channel_snapshots_observe, nullptr);
    message_event_pump_set_presence_visibility(&g_app.message_pump, presence_visible, nullptr);
    const u32 poll_revision = g_app.account_ui ? g_app.account_ui->message_poll_store.revision : 0;
    if (g_sync.instance != host.instance)
        g_sync = {host.instance};
    bool repaint = false;
    bool ready_applied = false;
    for (u32 source_index = 0; source_index < host.status.gateway_sources; ++source_index) {
        void* source = host.api.gateway_source(host.instance, source_index);
        if (!source) {
            continue;
        }
        const bool private_was_incomplete = g_app.message_pump.private_state_incomplete;
        const u32 hydration_skipped = g_app.message_pump.hydration_skipped;
        BrowserGatewayDrain browser_drain{};
        browser_drain.source_index = static_cast<u8>(source_index);
        browser_drain.source = source;
        browser_drain.navigation_ui = g_app.account_ui;
        browser_drain.navigation_instance = host.instance;
        browser_drain.navigation_scope = g_app.plugin_account_scope_serial;
        browser_drain.navigation_profile = host.status.profile_id;
        browser_drain.navigation_self = g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0;
        auto* previous_navigation_drain = navigation_gateway_drain;
        navigation_gateway_drain = &browser_drain;
        const MessageEventPumpDrainResult drained = message_event_pump_drain(
            &g_app.message_pump, &browser_drain, pop_browser_gateway, kGatewayClientEventCapacity);
        navigation_gateway_drain = previous_navigation_drain;
        finish_browser_gateway_receipt(browser_drain);
        if (drained.private_state_incomplete && !private_was_incomplete) {
            log_write(LogLevel::error, "account", "private_state_incomplete");
        }
        repaint = repaint || drained.repaint;
        ready_applied = ready_applied || drained.ready_applied;
        if (drained.apply_failures)
            log_skipped_events(source_index, drained);
        if (g_app.message_pump.hydration_skipped != hydration_skipped)
            log_u64(LogLevel::warning, "account", "message_author_cache_skipped",
                    g_app.message_pump.hydration_skipped - hydration_skipped);
        repaint = observe_sync_health(host, drained) || repaint;
        if (!drained.ok()) {
            // Transport failures only (a corrupt or oversized mailbox record):
            // the stream itself is unusable, so the session stops.
            log_u64(LogLevel::error, "account", "event_drain_error", static_cast<u64>(drained.error));
            return drain_failed("event_drain_failed");
        }
        if (browser_drain.all) {
            application_thread_browser_gateway();
            application_thread_creation_gateway();
        } else
            for (u32 i = 0; i < browser_drain.count; ++i) {
                application_thread_browser_gateway(browser_drain.scopes[i].guild,
                                                   browser_drain.scopes[i].parent);
                application_thread_creation_gateway(browser_drain.scopes[i].guild,
                                                    browser_drain.scopes[i].parent);
            }
    }
    if (g_app.account_ui) {
        repaint = chat_state_store_expire_restore(&g_app.account_ui->chat_state,
                                                     clock_monotonic_milliseconds()) ||
                  repaint;
    }
    if (g_app.message_pump.private_state_incomplete && !g_app.message_pump.private_recovery_requested) {
        const auto recovery = host.api.gateway_refresh(host.instance);
        if (recovery != GatewayStateRecoveryResult::unavailable) {
            g_app.message_pump.private_recovery_requested = true;
            log_write(LogLevel::warning, "account",
                      recovery == GatewayStateRecoveryResult::exhausted
                          ? "private_state_recovery_exhausted"
                          : "private_state_recovery_requested");
        }
    }
    if (host.api.voice_snapshot && g_app.account_ui) {
        GatewayClientVoiceSnapshot voice{};
        auto& ui = *g_app.account_ui;
        if (host.api.voice_snapshot(host.instance, &voice) && !(voice == ui.voice_session)) {
            ui.voice_session = voice;
            const u64 call = gateway_voice_call_channel(voice);
            // The account panel shows guild voice channels only.
            const u64 channel = voice.guild_id ? call : 0;
            // Call video decodes on the renderer's device; give it once per
            // module instance when a call (guild or DM) starts.
            static void* video_device_instance = nullptr;
            if (call && video_device_instance != host.instance && host.api.voice_video_device &&
                g_app.renderer.graphics.device &&
                host.api.voice_video_device(host.instance, g_app.renderer.graphics.device))
                video_device_instance = host.instance;
            repaint = voice_state_store_set_speaking(&ui.voice_states, call,
                                                             {voice.speaking_users, voice.speaking_count}) ||
                      repaint;
            if (ui.navigation_view.connected_voice_channel_id != channel) {
                ui.navigation_view.connected_voice_channel_id = channel;
                ui.navigation_view.connected_voice_stale = true;
                saturating_increment(ui.navigation_view.revision);
                repaint = true;
            }
        }
    }
    application_voice_pump();
    if (!consume_message_send_completions())
        return drain_failed("send_completion_sync_failed");
    if (!synchronize_navigation_view())
        return drain_failed("navigation_sync_failed");
    if (!synchronize_selected_channel())
        return drain_failed("selected_channel_sync_failed");
    // A startup shell ends with READY's selection settled; collapsed
    // categories it brings back rebuild the channel list.
    if (ready_applied) {
        application_startup_shell_ready();
        if (!synchronize_navigation_view())
            return drain_failed("navigation_sync_failed");
    }
    const u32 thread_revision = application_thread_actions_publication_revision();
    application_thread_actions_synchronize();
    // Feature surfaces are presentation: a failure degrades the surface, never
    // the session.
    if (!application_thread_browser_synchronize())
        application_record_presentation_failure("thread_browser_sync_failed");
    if (!application_thread_creation_synchronize())
        application_record_presentation_failure("thread_creation_sync_failed");
    if (!application_forum_post_synchronize())
        application_record_presentation_failure("forum_post_sync_failed");
    if (!application_poll_synchronize())
        application_record_presentation_failure("poll_sync_failed");
    repaint =
        repaint || (g_app.account_ui && poll_revision != g_app.account_ui->message_poll_store.revision);
    if (thread_revision != application_thread_actions_publication_revision()) {
        if (!application_refresh_thread_actions_menu() || !synchronize_shell_widget_scenes())
            application_record_presentation_failure("thread_actions_refresh_failed");
        repaint = true;
    }
    application_notifications_selection(&g_app);
    if (presence_pending())
        presence_coalescer_note(&g_presence, clock_monotonic_milliseconds());
    if (repaint && g_app.window) {
        application_request_frame();
    }
    application_notifications_wake(&g_app);
    return true;
}

} // namespace

bool application_pump_visible_static_media(bool* repaint) noexcept {
    if (application_thread_browser_read_blocked()) {
        // The browser covers only the chat column: keep uncovered chrome
        // animations playing instead of stalling the shared timeline.
        const auto preview = application_forum_form_active() ? application_pump_forum_preview_media(&g_app)
                                                             : application_pump_covered_media(&g_app);
        if (repaint)
            *repaint = preview.repaint;
        return preview.ok;
    }
    const ApplicationMediaPumpResult pumped = application_pump_visible_media(&g_app);
    if (!pumped.ok || (pumped.text_changed && !refresh_account_text(true)) ||
        (pumped.history_requested && !pump_message_history())) {
        return false;
    }
    if (repaint) {
        *repaint = pumped.repaint;
    }
    return true;
}

u64 application_presence_next_wake() noexcept {
    return g_app.account_ui ? g_presence.flush_at_ms : 0;
}

Utf8View application_account_sync_notice() noexcept {
    if (!g_app.account_ui || !g_app.account.instance || g_sync.instance != g_app.account.instance)
        return {};
    switch (g_sync.phase) {
    case AccountSyncPhase::recovering:
        return utf8_literal("Some updates could not be applied. Refreshing account data.");
    case AccountSyncPhase::stale:
        return utf8_literal("Some updates could not be applied. Restart MaiCord if anything looks out of date.");
    case AccountSyncPhase::healthy:
        break;
    }
    return {};
}

bool application_pump_account() noexcept {
    if (application_account_logout_blocked())
        return true;
    AccountHost& host = g_app.account;
    if (!host.instance || host.stop_failed) {
        return true;
    }
    const u64 now_ms = clock_monotonic_milliseconds();
    AccountUiRuntime& ui = *g_app.account_ui;
    message_draft_session_persist(ui.message_draft_session, ui.message_drafts, now_ms, false);
    application_channel_memory_pump(now_ms);
    application_startup_shell_pump(now_ms);
    u32 observations = 0;
    const AccountModuleStatus status =
        account_host_pump(&host, now_ms, clock_unix_milliseconds(), &observations);
    application_log_account_observations(status, observations);
    if (status.failed) {
        // Fatal worker results must be read before teardown destroys their
        // mailbox. This path consumes no ordinary dispatch/model callbacks.
        drain_terminal_gateway_diagnostics();
        return application_account_return_to_login(
            status.coordinator_error ==
            static_cast<u8>(UserAccountCoordinatorError::authentication_rejected));
    }
    if (!drain_account_events()) {
        application_account_stop_recoverable(g_drain_failure);
        return false;
    }
    application_scoped_read_synchronize();
    const u32 channel_publication =
        g_app.account_ui ? g_app.account_ui->chat_state.channel_mutations : 0;
    const u32 selection_publication =
        g_app.account_ui ? g_app.account_ui->chat_state.selection_revision : 0;
    if (!pump_message_history()) {
        log_write(LogLevel::error, "account", "message_history_pump_failed");
    }
    if (g_app.account_ui &&
        (channel_publication != g_app.account_ui->chat_state.channel_mutations ||
         selection_publication != g_app.account_ui->chat_state.selection_revision) &&
        (!synchronize_navigation_view() || !synchronize_selected_channel()))
        return false;
    if (!application_refresh_thread_actions_menu())
        return false;
    if (!application_scoped_read_menu_refresh())
        return false;
    if (!application_read_state_observe(now_ms))
        log_write(LogLevel::warning, "read", "automatic_ack_not_admitted");
    // Start model-ready requests before presentation work so their worker can
    // connect/receive while the UI prepares text. Coalesce coordinator and model
    // changes into one conditional presentation pass afterward.
    const u64 thread_publication =
        g_app.account_ui && !application_friends_active()
            ? application_thread_header_projection(
                  application_thread_actions_view(g_app.account_ui->chat_state.selected_channel_id))
                  .source_publication
            : 0;
    const bool thread_stale = g_app.thread_header.view.source_publication != thread_publication;
    const bool presence_repaint = flush_presence(now_ms);
    if (!refresh_account_text(thread_stale || presence_repaint ||
                              (observations & account_host_observation_phase) != 0))
        return false;
    if (presence_repaint && g_app.window)
        application_request_frame();
    if (!present_private_conversation_error() || !application_private_channel_present_error())
        log_write(LogLevel::warning, "private", "error_presentation_failed");
    // A startup shell released the view before READY; the plugins wait for it.
    if (status.running && (g_app.login.status == RendererLoginStatus::restoring_session ||
                           application_startup_shell_take_release())) {
        application_trace_load_stage("authenticated_shell_released");
        g_app.login.visible = false;
        g_app.login.status = RendererLoginStatus::idle;
        if (!application_synchronize_login_view())
            return false;
        if (!application_plugins_restore())
            log_write(LogLevel::warning, "plugins", "remembered_restore_incomplete");
        application_request_frame();
    }
    return true;
}

} // namespace maicord::app_detail
