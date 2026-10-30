#include "application_downloads.hpp"
#include "maicord/core/memory.hpp"
#include "application_read_state.hpp"
#include "application_state.hpp"
#include "application_window_focus.hpp"
#include "application_search.hpp"
#include "application_pinned_messages.hpp"
#include "application_thread_browser.hpp"
#include "application_scoped_read.hpp"
#include "application_account_presentation.hpp"
#include "application_message_mutation_notice.hpp"
#include "application_composer.hpp"
#include "maicord/app/read_ack_controller.hpp"
#include "maicord/app/read_ack_visibility.hpp"
#include "maicord/model/read_state.hpp"
#include "maicord/protocol/message_ack.hpp"
#include "maicord/ui/quick_switcher_scene.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/message_forward_scene.hpp"
#include "maicord/ui/friends_scene.hpp"
#include "maicord/ui/poll_controls.hpp"
#include "maicord/ui/call_scene.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/render/forum_post_surface.hpp"
#include "account_ui_runtime.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/ui/forum_post_scene.hpp"

namespace maicord::app_detail {
namespace {
struct ReadActionSource {
    u64 channel_id, channel_generation, message_id, cursor, serial;
    MessageHandle handle;
    u32 message_revision;
    bool message_target, manual;
    u8 attempts;
    bool scoped;
};
static_assert(sizeof(ReadActionSource) == 56);
struct ManualReadEcho {
    u64 channel_id, cursor, serial, request_id;
    u32 channel_generation;
    bool manual;
    u16 copies;
};
static_assert(sizeof(ManualReadEcho) == 40);
} // namespace
struct ApplicationReadState {
    ReadAckController queue;
    StableStorage intents;
    StableStorage token;
    StableStorage actions, echoes;
    ReadAckRequestTicket ticket;
    ReadActionSource submitted_manual, selected_manual, failed_manual;
    AccountUiRuntime* account_ui;
    void* account_instance;
    u64 account_serial, profile_id, action_serial;
    u64 user_id, submitted_user_id, submit_retry_at_ms;
    u32 read_generation, token_generation, submitted_token_generation, token_bytes, action_count,
        echo_count;
    bool token_known;
};
static_assert(sizeof(ApplicationReadState) == 712);
namespace {
bool forum_form_covers_messages() noexcept {
    return g_app.renderer.forum_form && g_app.renderer.forum_form->scene &&
           g_app.renderer.forum_form->scene->active;
}
bool read_surfaces_blocked() noexcept {
    return application_thread_browser_read_blocked() ||
           (g_app.renderer.poll_voters_scene && g_app.renderer.poll_voters_scene->active) ||
           forum_form_covers_messages() || application_downloads_active() ||
           (g_app.renderer.message_forward_scene && g_app.renderer.message_forward_scene->active);
}
bool account_live() noexcept {
    return g_app.account_ui && g_app.account.instance && g_app.account.api.message_ack &&
           !g_app.account.stop_failed && !g_app.login.visible && g_app.plugin_account_scope_serial &&
           g_app.account.status.profile_id && g_app.account_ui->chat_state.self_user_id;
}
// The owner was created for the account instance, profile and (given) user now in the app.
bool actor_matches(const ApplicationReadState& o, u64 user_id) noexcept {
    return o.account_ui == g_app.account_ui && o.account_instance == g_app.account.instance &&
           o.account_serial == g_app.plugin_account_scope_serial &&
           o.profile_id == g_app.account.status.profile_id && user_id == g_app.account_ui->chat_state.self_user_id;
}
bool scoped(const ApplicationReadState& o) noexcept {
    return account_live() && actor_matches(o, o.user_id) &&
           o.read_generation == g_app.account_ui->read_state_store.generation;
}
const ChannelRecord* readable(u64 id, bool unread = false) noexcept {
    if (!account_live())
        return nullptr;
    const auto& state = g_app.account_ui->chat_state;
    const auto* c = chat_state_store_find_channel(state, id);
    if (!c || (c->flags & kChannelObfuscatedFlag) || !channel_is_message_capable(c->type) ||
        !chat_state_store_channel_visible(state, *c) || !(c->actions & channel_action_view) ||
        !(c->actions & channel_action_read_history))
        return nullptr;
    if (c->guild_id) {
        const auto* guild = chat_state_store_find_guild(state, c->guild_id);
        if (!guild || guild->unavailable || !c->permissions_resolved)
            return nullptr;
    }
    if (unread && channel_is_thread(c->type)) {
        if (!c->thread_archived_known || c->thread_archived || !c->thread_self_joined_known ||
            !c->thread_self_joined)
            return nullptr;
    }
    if (unread && (c->flags & 128u))
        return nullptr; // A server resource channel (IS_GUILD_RESOURCE_CHANNEL flag).
    return c;
}
bool source_current(const ApplicationReadState& o, const ReadActionSource& s,
                    bool message = true) noexcept {
    const auto* c = scoped(o)
                        ? chat_state_store_find_channel(g_app.account_ui->chat_state, s.channel_id)
                        : nullptr;
    if (s.scoped) {
        if (!c || !c->occupied || (c->flags & kChannelObfuscatedFlag) || !c->permissions_resolved ||
            !(c->actions & channel_action_view) ||
            !(c->actions & channel_action_read_history) ||
            !chat_state_store_channel_visible(g_app.account_ui->chat_state, *c))
            return false;
        const auto* guild = chat_state_store_find_guild(g_app.account_ui->chat_state, c->guild_id);
        if (!guild || !guild->occupied || guild->unavailable)
            return false;
        if (channel_is_thread(c->type) &&
            (!c->thread_active_known || !c->thread_listed_active || !c->thread_self_joined_known ||
             !c->thread_self_joined || (c->thread_archived_known && c->thread_archived)))
            return false;
    } else
        c = c ? readable(s.channel_id, s.manual) : nullptr;
    if (!c || c->generation != s.channel_generation)
        return false;
    if (!message || !s.message_target)
        return true;
    const auto found = message_store_find(g_app.message_view.store, s.message_id);
    if (found.ok()) {
        const auto* record = message_store_resolve(g_app.message_view.store, s.handle);
        return record && record->id == s.message_id && record->channel_id == s.channel_id &&
               record->revision == s.message_revision &&
               record->delivery == MessageDeliveryState::authoritative;
    }
    // The boundary was captured from a server-confirmed message when the request
    // was accepted. A channel switch invalidates the timeline but not that intent.
    return g_app.account_ui->active_channel_id != s.channel_id &&
           g_app.account_ui->active_channel_id == g_app.account_ui->chat_state.selected_channel_id;
}
ReadActionSource* action(ApplicationReadState& o, u64 channel) noexcept {
    auto* values = stable_storage_data<ReadActionSource>(o.actions);
    for (u32 i = 0; i < o.action_count; ++i)
        if (values[i].channel_id == channel)
            return values + i;
    return nullptr;
}
void erase_action(ApplicationReadState& o, u64 channel) noexcept {
    auto* values = stable_storage_data<ReadActionSource>(o.actions);
    for (u32 i = 0; i < o.action_count; ++i)
        if (values[i].channel_id == channel) {
            values[i] = values[--o.action_count];
            SecureZeroMemory(values + o.action_count, sizeof(*values));
            return;
        }
}
void erase_echo(ApplicationReadState& o, u32 index) noexcept {
    auto* values = stable_storage_data<ManualReadEcho>(o.echoes);
    values[index] = values[--o.echo_count];
    SecureZeroMemory(values + o.echo_count, sizeof(*values));
}
bool has_echo(const ApplicationReadState& o, u64 channel) noexcept {
    const auto* values = stable_storage_data<ManualReadEcho>(o.echoes);
    for (u32 i = 0; i < o.echo_count; ++i)
        if (values[i].channel_id == channel)
            return true;
    return false;
}
bool has_pending(const ApplicationReadState& o, u64 channel) noexcept {
    for (u32 i = 0; i < o.queue.count; ++i)
        if (o.queue.pending[i].channel_id == channel)
            return true;
    return false;
}
void prune_action(ApplicationReadState& o, u64 channel) noexcept {
    const auto* retained = action(o, channel);
    if (retained && retained->scoped)
        return;
    if (!has_pending(o, channel) && !has_echo(o, channel) && o.selected_manual.channel_id != channel)
        erase_action(o, channel);
}
bool action_capacity(ApplicationReadState& o, u64 channel) noexcept {
    return action(o, channel) || (o.action_count < kReadAckMaximumIntents &&
                                  stable_storage_reserve(&o.actions, o.action_count + 1));
}
void store_action(ApplicationReadState& o, const ReadActionSource& source) noexcept {
    auto* value = action(o, source.channel_id);
    if (!value)
        value = stable_storage_data<ReadActionSource>(o.actions) + o.action_count++;
    *value = source;
}
void clear_manual_failure(ApplicationReadState& o, u64 channel = 0) noexcept {
    if (!channel || o.failed_manual.channel_id == channel) {
        const bool visible = o.failed_manual.channel_id != 0;
        reset_value(&o.failed_manual);
        application_message_mutation_notice_clear(ApplicationMessageMutationKind::mark_unread, channel);
        if (visible)
            refresh_message_composer_ui();
    }
}
void project(ApplicationReadState& o, const ReadActionSource& source) noexcept {
    if (!source_current(o, source, false))
        return;
    if (source.manual) {
        const auto* c = readable(source.channel_id, true);
        // Mute/block/mention suppression inputs are not retained in this cache.
        // Preserve the existing count as an incomplete estimate, never emit a guessed wire count.
        read_state_mark_unread_local(&g_app.account_ui->chat_state, source.channel_id,
                                             source.cursor, c->mention_count, false);
    } else {
        const auto* c = source.scoped ? chat_state_store_find_channel(g_app.account_ui->chat_state,
                                                                         source.channel_id)
                                      : readable(source.channel_id);
        if (c->last_message_id > source.cursor) {
            ReadStateEntry entry{};
            entry.channel_id = source.channel_id;
            entry.message_id = source.cursor;
            entry.mention_count = c->mention_count;
            entry.last_viewed = c->read_last_viewed;
            entry.flags = c->read_flags;
            entry.fields = read_state_cursor | read_state_mentions | read_state_mentions_incomplete |
                           read_state_last_viewed | read_state_flags;
            chat_state_store_apply_read_state(&g_app.account_ui->chat_state, entry);
            read_state_leave_channel(&g_app.account_ui->chat_state, source.channel_id);
        } else
            read_state_mark_local(&g_app.account_ui->chat_state, source.channel_id,
                                          source.cursor, c->read_last_viewed, c->read_flags);
    }
}
void maintain_selected(ApplicationReadState& o) noexcept {
    const auto source = o.selected_manual;
    if (!source.channel_id)
        return;
    const auto* c = source_current(o, source, false) ? readable(source.channel_id, true) : nullptr;
    if (!c || g_app.account_ui->chat_state.selected_channel_id != source.channel_id ||
        !c->read_auto_ack_suppressed) {
        reset_value(&o.selected_manual);
        prune_action(o, source.channel_id);
        return;
    }
    if (c->last_read_message_id != source.cursor)
        project(o, source);
}
ApplicationReadState* ensure_owner() noexcept {
    if (g_app.read_state)
        return g_app.read_state;
    if (!account_live())
        return nullptr;
    auto* owner = static_cast<ApplicationReadState*>(
        VirtualAlloc(nullptr, sizeof(ApplicationReadState), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!owner)
        return nullptr;
    if (!stable_storage_initialize(&owner->intents, kReadAckMaximumIntents, 0, sizeof(ReadAckIntent),
                                   alignof(ReadAckIntent)) ||
        !stable_storage_initialize(&owner->token, kMessageAckMaximumBodyBytes, 0, 1, 1) ||
        !stable_storage_initialize(&owner->actions, kReadAckMaximumIntents, 0, sizeof(ReadActionSource),
                                   alignof(ReadActionSource)) ||
        !stable_storage_initialize(&owner->echoes, kReadAckMaximumIntents, 0, sizeof(ManualReadEcho),
                                   alignof(ManualReadEcho)) ||
        !read_ack_controller_initialize(
            &owner->queue, {stable_storage_data<ReadAckIntent>(owner->intents), kReadAckMaximumIntents}) ||
        !read_ack_controller_set_reserve(
            &owner->queue,
            [](void* context, u32 count) noexcept {
                return stable_storage_reserve(&static_cast<ApplicationReadState*>(context)->intents, count);
            },
            owner)) {
        stable_storage_release(&owner->intents);
        stable_storage_release(&owner->token);
        stable_storage_release(&owner->actions);
        stable_storage_release(&owner->echoes);
        VirtualFree(owner, 0, MEM_RELEASE);
        return nullptr;
    }
    owner->user_id = g_app.account_ui->chat_state.self_user_id;
    owner->read_generation = g_app.account_ui->read_state_store.generation;
    owner->account_ui = g_app.account_ui;
    owner->account_instance = g_app.account.instance;
    owner->account_serial = g_app.plugin_account_scope_serial;
    owner->profile_id = g_app.account.status.profile_id;
    g_app.read_state = owner;
    return owner;
}
// Whole days since the Snowflake epoch, rounded up: the ACK "last viewed" field.
u32 viewed_day() noexcept {
    const u64 unix_ms = clock_unix_milliseconds();
    const u64 day = unix_ms > kSnowflakeEpochMilliseconds
                        ? (unix_ms - kSnowflakeEpochMilliseconds + 86'399'999ull) / 86'400'000ull
                        : 0;
    return static_cast<u32>(minimum<u64>(day, ~u32{0}));
}
void clear_token(ApplicationReadState& owner) noexcept {
    if (owner.token_bytes)
        SecureZeroMemory(owner.token.arena.base, owner.token_bytes);
    owner.token_bytes = 0;
    owner.token_known = false;
    ++owner.token_generation;
}
void retire_channel(ApplicationReadState& o, u64 channel_id, bool own_activity) noexcept {
    const auto* c = scoped(o) ? readable(channel_id) : nullptr;
    const bool retain_read = own_activity && c && !c->read_auto_ack_suppressed && c->last_message_id &&
                             c->last_read_message_id == c->last_message_id && has_echo(o, channel_id) &&
                             o.action_serial != ~u64{0} && action_capacity(o, channel_id);
    ReadActionSource read{};
    if (retain_read) {
        read.channel_id = channel_id;
        read.channel_generation = c->generation;
        read.message_id = read.cursor = c->last_read_message_id;
        read.serial = ++o.action_serial;
    }
    read_ack_controller_cancel(&o.queue, channel_id);
    erase_action(o, channel_id);
    if (o.selected_manual.channel_id == channel_id)
        reset_value(&o.selected_manual);
    clear_manual_failure(o, channel_id);
    if (retain_read)
        store_action(o, read);
    application_scoped_read_superseded(channel_id, retain_read ? read.serial : 0);
    if (!c)
        for (u32 i = 0; i < o.echo_count;) {
            if (stable_storage_data<ManualReadEcho>(o.echoes)[i].channel_id == channel_id)
                erase_echo(o, i);
            else
                ++i;
        }
}
u64 predecessor(u64 channel, u64 message) noexcept {
    const auto& store = g_app.message_view.store;
    u64 previous = 0;
    if (store.records && store.order && store.count <= store.capacity) {
        for (u32 i = 0; i < store.count; ++i) {
            const u32 slot = store.order[i];
            if (slot >= store.capacity)
                continue;
            const auto& record = store.records[slot];
            if (record.occupied && record.channel_id == channel &&
                record.delivery == MessageDeliveryState::authoritative && record.id < message &&
                record.id > previous)
                previous = record.id;
        }
    }
    if (previous)
        return previous;
    const u64 timestamp = message >> 22;
    return timestamp ? (timestamp - 1) << 22 : 0;
}
u32 mentions_after(u64 channel, u64 cursor) noexcept {
    const auto& store = g_app.message_view.store;
    u32 count = 0;
    if (g_app.account_ui->chat_state.selected_channel_id != channel || !store.records || !store.order ||
        store.count > store.capacity)
        return 0;
    for (u32 i = 0; i < store.count; ++i) {
        const u32 slot = store.order[i];
        if (slot >= store.capacity)
            continue;
        const auto& record = store.records[slot];
        // A message with a cached badge decision (live CREATE) uses it; otherwise the raw mention flag.
        const u8 badge = (record.presentation_flags & message_presentation_read_badge_known)
                             ? message_presentation_read_badge
                             : message_presentation_mentions_current_user;
        if (record.occupied && record.channel_id == channel && record.id > cursor &&
            record.delivery == MessageDeliveryState::authoritative && (record.presentation_flags & badge))
            count = count == ~0u ? count : count + 1;
    }
    return count;
}
void manual_failed(ApplicationReadState& o, const ReadActionSource& source,
                   const RestClientResponse& response) noexcept {
    const auto* latest = action(o, source.channel_id);
    if (!latest || latest->serial != source.serial || !latest->manual || source.serial != o.action_serial ||
        !scoped(o))
        return;
    const auto* c = chat_state_store_find_channel(g_app.account_ui->chat_state, source.channel_id);
    if (!c || c->generation != source.channel_generation)
        return;
    o.failed_manual = source;
    application_message_mutation_notice_begin(ApplicationMessageMutationKind::mark_unread,
                                              source.channel_id, source.message_id);
    application_message_mutation_notice_failed(ApplicationMessageMutationKind::mark_unread,
                                               source.channel_id, source.message_id, response);
    refresh_message_composer_ui();
    synchronize_navigation_view();
    application_request_frame();
}
void manual_failed(ApplicationReadState& o, const ReadActionSource& source, u16 status) noexcept {
    RestClientResponse response{};
    response.status_code = status;
    manual_failed(o, source, response);
}
// A queued ACK cannot be sent: drop it and tell whoever owns its action (the manual notice, a scoped plan).
void abandon(ApplicationReadState& o, u64 channel_id, const ReadActionSource* captured, bool manual,
             u16 status) noexcept {
    read_ack_controller_cancel(&o.queue, channel_id);
    if (manual && captured)
        manual_failed(o, *captured, status);
    if (captured && captured->scoped)
        application_scoped_read_individual_result(channel_id, captured->serial, false, false);
}
bool queue_manual(ApplicationReadState& o, ReadActionSource source) noexcept {
    if (!source_current(o, source) || o.action_serial == ~u64{0} || !action_capacity(o, source.channel_id))
        return false;
    ReadAckIntent desired{};
    desired.channel_id = source.channel_id;
    desired.message_id = source.cursor;
    desired.kind = ReadAckKind::manual;
    // As the client counts: resident messages after the boundary that carry a mention badge.
    desired.mention_count_known = true;
    desired.mention_count = mentions_after(source.channel_id, source.cursor);
    if (!read_ack_controller_queue(&o.queue, desired, clock_monotonic_milliseconds(), true))
        return false;
    source.serial = ++o.action_serial;
    source.attempts = 0;
    store_action(o, source);
    clear_manual_failure(o);
    application_scoped_read_superseded(source.channel_id, source.serial);
    project(o, source);
    if (g_app.account_ui->chat_state.selected_channel_id == source.channel_id)
        o.selected_manual = source;
    application_message_mutation_notice_begin(ApplicationMessageMutationKind::mark_unread,
                                              source.channel_id, source.message_id);
    refresh_message_composer_ui();
    if (g_app.window)
        PostMessageW(g_app.window, WM_NULL, 0, 0);
    return synchronize_navigation_view();
}
} // namespace

void application_read_state_activity() noexcept {
    g_app.read_last_activity_ms = clock_monotonic_milliseconds();
}
bool application_read_state_eligible() noexcept {
    // A startup shell's channel is acknowledged once READY has its read state.
    if (!g_app.account_ui || !g_app.account.instance || !g_app.window || g_app.startup_shell_provisional)
        return false;
    if (read_surfaces_blocked() ||
        (g_app.renderer.quick_switcher_scene && g_app.renderer.quick_switcher_scene->active) ||
        application_search_read_blocked() || application_pinned_messages_read_blocked() ||
        (g_app.renderer.call_scene && g_app.renderer.call_scene->active && !g_app.renderer.call_scene->chat_open))
        return false;
    const auto* pane =
        ui_scene_node(g_app.shell_scene, UiSemantic::message_view);
    ReadAckVisibility visibility{
        application_window_focused(g_app.window),
        !g_app.login.visible && pane && ui_rect_has_area(pane->rectangle) &&
            !(g_app.renderer.friends_scene && g_app.renderer.friends_scene->active) &&
            !(g_app.renderer.settings_scene && g_app.renderer.settings_scene->active) &&
            !(g_app.renderer.profile_scene && g_app.renderer.profile_scene->active) &&
            !(g_app.renderer.text_form && g_app.renderer.text_form->scene &&
              g_app.renderer.text_form->scene->active) &&
            !g_app.reaction_viewer.active && !g_app.image_viewer.scene.active && !g_app.popup_scene.active,
        g_app.message_view.timeline.follow_bottom,
        g_app.message_catchup.state == MessageHistoryCatchupState::complete &&
            !g_app.pending_message_jump_message_id &&
            g_app.account_ui->active_channel_id == g_app.account_ui->chat_state.selected_channel_id,
        g_app.message_older.state == MessageHistoryOlderState::request_required ||
            g_app.message_older.state == MessageHistoryOlderState::awaiting_page ||
            g_app.message_older.state == MessageHistoryOlderState::waiting_retry,
        g_app.message_view.timeline.newer_refetch_required,
        g_app.message_pump.catchup_required,
        g_app.message_pump.resync_required,
        false,
        false};
    if (!read_ack_automatic_eligible(visibility))
        return false;
    LASTINPUTINFO input{sizeof(input), 0};
    if (!GetLastInputInfo(&input))
        return false;
    const u64 now = clock_monotonic_milliseconds();
    const u32 system_idle = GetTickCount() - input.dwTime;
    const bool locally_idle =
        !g_app.read_last_activity_ms ||
        (now >= g_app.read_last_activity_ms && now - g_app.read_last_activity_ms >= 600'000);
    visibility.idle = locally_idle && system_idle >= 600'000;
    const auto* channel = chat_state_store_find_channel(
        g_app.account_ui->chat_state, g_app.account_ui->chat_state.selected_channel_id);
    visibility.manual_suppressed = channel && channel->read_auto_ack_suppressed;
    return read_ack_automatic_eligible(visibility);
}
void application_read_state_ready() noexcept {
    if (!g_app.read_state || !g_app.account_ui)
        return;
    auto& owner = *g_app.read_state;
    const auto& ui = *g_app.account_ui;
    const bool same_actor = actor_matches(owner, owner.user_id);
    if (same_actor && owner.read_generation == ui.read_state_store.generation)
        return;
    read_ack_controller_reset(&owner.queue);
    clear_token(owner);
    if (!same_actor) {
        reset_value(&owner.ticket);
        owner.submitted_user_id = 0;
    }
    owner.read_generation = ui.read_state_store.generation;
    owner.user_id = ui.chat_state.self_user_id;
    owner.account_ui = g_app.account_ui;
    owner.account_instance = g_app.account.instance;
    owner.account_serial = g_app.plugin_account_scope_serial;
    owner.profile_id = g_app.account.status.profile_id;
    if (owner.action_count)
        SecureZeroMemory(owner.actions.arena.base, owner.action_count * sizeof(ReadActionSource));
    if (owner.echo_count)
        SecureZeroMemory(owner.echoes.arena.base, owner.echo_count * sizeof(ManualReadEcho));
    owner.action_count = owner.echo_count = 0;
    reset_value(&owner.submitted_manual);
    reset_value(&owner.selected_manual);
    clear_manual_failure(owner);
    owner.submit_retry_at_ms = 0;
    application_scoped_read_reset();
}
bool application_read_state_mark(u64 channel_id, bool immediate) noexcept {
    if (!account_live())
        return false;
    auto& state = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, channel_id);
    if (!channel || !channel_is_message_capable(channel->type) ||
        !chat_state_store_channel_visible(state, *channel) ||
        !(channel->actions & channel_action_read_history))
        return false;
    if (!channel->last_message_id)
        return true;
    application_read_state_ready();
    auto* owner = ensure_owner();
    if (!owner)
        return false;
    const bool tracked = action(*owner, channel_id) || has_echo(*owner, channel_id);
    const bool clears_failure = owner->failed_manual.channel_id != 0;
    if ((tracked || clears_failure) && owner->action_serial == ~u64{0})
        return false;
    if (tracked && !action_capacity(*owner, channel_id))
        return false;
    const u32 flags = channel_is_thread(channel->type) ? 2u : channel->guild_id ? 1u : 0u;
    ReadAckIntent desired{channel_id, channel->last_message_id, viewed_day(), flags, 0};
    // The client sends flags only when they differ from the value the server holds.
    desired.flags_known = channel->read_flags != flags;
    if (!read_ack_controller_queue(&owner->queue, desired, clock_monotonic_milliseconds(),
                                   immediate || channel->mention_count != 0))
        return false;
    if (tracked) {
        ReadActionSource source{};
        source.channel_id = channel_id;
        source.channel_generation = channel->generation;
        source.message_id = desired.message_id;
        source.cursor = desired.message_id;
        source.serial = ++owner->action_serial;
        store_action(*owner, source);
        application_scoped_read_superseded(channel_id, source.serial);
    } else if (clears_failure)
        ++owner->action_serial;
    if (owner->selected_manual.channel_id == channel_id)
        reset_value(&owner->selected_manual);
    clear_manual_failure(*owner, (tracked || clears_failure) ? 0 : channel_id);
    if (!read_state_mark_local(&state, channel_id, desired.message_id, desired.last_viewed,
                                       desired.flags))
        return false;
    if (g_app.window)
        PostMessageW(g_app.window, WM_NULL, 0, 0);
    return synchronize_navigation_view();
}
bool application_read_state_message_unread_available(u64 message_id) noexcept {
    if (!account_live() || !message_id)
        return false;
    const auto* record = application_message_record(message_id);
    return record && record->delivery == MessageDeliveryState::authoritative &&
           readable(record->channel_id, true) && record->channel_id == g_app.account_ui->active_channel_id;
}
bool application_read_state_channel_unread_available(u64 channel_id) noexcept {
    const auto* c = readable(channel_id, true);
    return c && c->last_message_id && c->read_state_known && !c->mention_count &&
           c->last_read_message_id >= c->last_message_id;
}
bool application_read_state_mark_message_unread(u64 message_id) noexcept {
    if (!application_read_state_message_unread_available(message_id))
        return false;
    application_read_state_ready();
    auto* o = ensure_owner();
    if (!o)
        return false;
    const auto found = message_store_find(g_app.message_view.store, message_id);
    const auto* record =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    const auto* c = record ? readable(record->channel_id, true) : nullptr;
    if (!c)
        return false;
    ReadActionSource source{};
    source.channel_id = c->id;
    source.channel_generation = c->generation;
    source.message_id = message_id;
    source.cursor = predecessor(c->id, message_id);
    source.handle = found.handle;
    source.message_revision = record->revision;
    source.message_target = source.manual = true;
    return queue_manual(*o, source);
}
bool application_read_state_mark_channel_unread(u64 channel_id) noexcept {
    if (!application_read_state_channel_unread_available(channel_id))
        return false;
    application_read_state_ready();
    auto* o = ensure_owner();
    if (!o)
        return false;
    const auto* c = readable(channel_id, true);
    if (!c)
        return false;
    ReadActionSource source{};
    source.channel_id = c->id;
    source.channel_generation = c->generation;
    source.message_id = c->last_message_id;
    source.cursor = predecessor(c->id, c->last_message_id);
    source.manual = true;
    return queue_manual(*o, source);
}
bool application_read_state_channel_unread_retry_available(u64 channel_id) noexcept {
    const auto* o = g_app.read_state;
    return o && o->failed_manual.channel_id == channel_id && o->failed_manual.serial == o->action_serial &&
           source_current(*o, o->failed_manual);
}
bool application_read_state_retry_channel_unread(u64 channel_id) noexcept {
    if (!application_read_state_channel_unread_retry_available(channel_id))
        return false;
    return queue_manual(*g_app.read_state, g_app.read_state->failed_manual);
}
bool application_read_state_observe(u64) noexcept {
    application_read_state_ready();
    if (g_app.read_state)
        maintain_selected(*g_app.read_state);
    if (!application_read_state_eligible())
        return true;
    const auto& state = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, state.selected_channel_id);
    if (!channel || channel->read_auto_ack_suppressed || !channel->last_message_id ||
        (channel->last_read_message_id >= channel->last_message_id && !channel->mention_count))
        return true;
    return application_read_state_mark(channel->id, false);
}
u64 application_read_state_next_wake() noexcept {
    application_read_state_ready();
    if (read_surfaces_blocked() || !g_app.read_state ||
        g_app.message_history_request_owner != MessageHistoryRequestOwner::none)
        return 0;
    u64 due = 0;
    if (!read_ack_controller_next_due(g_app.read_state->queue, &due))
        return 0;
    return maximum<u64>(due ? due : 1, g_app.read_state->submit_retry_at_ms);
}
bool application_read_state_submit(u64 request_id, u64 now_ms) noexcept {
    application_read_state_ready();
    application_scoped_read_synchronize();
    if (read_surfaces_blocked() || !g_app.read_state || !account_live() ||
        now_ms < g_app.read_state->submit_retry_at_ms)
        return false;
    auto& owner = *g_app.read_state;
    ReadAckIntent intent{};
    if (!read_ack_controller_peek_due(owner.queue, now_ms, &intent))
        return false;
    auto* captured = action(owner, intent.channel_id);
    const auto* channel =
        captured && captured->scoped
            ? (source_current(owner, *captured, false)
                   ? chat_state_store_find_channel(g_app.account_ui->chat_state, intent.channel_id)
                   : nullptr)
            : readable(intent.channel_id, intent.kind == ReadAckKind::manual);
    const bool manual = intent.kind == ReadAckKind::manual;
    if (!channel || (manual && (!captured || !captured->manual || captured->cursor != intent.message_id ||
                                !source_current(owner, *captured)))) {
        abandon(owner, intent.channel_id, captured, manual, 403);
        prune_action(owner, intent.channel_id);
        return false;
    }
    // First ordinary ACKs also need owned echo evidence. Reserve before any
    // the request is queued; marking alone keeps the idle footprint small.
    if (!captured) {
        if (owner.action_serial == ~u64{0} || !action_capacity(owner, intent.channel_id))
            return false;
        ReadActionSource source{};
        source.channel_id = intent.channel_id;
        source.channel_generation = channel->generation;
        source.cursor = intent.message_id;
        source.serial = ++owner.action_serial;
        store_action(owner, source);
        captured = action(owner, intent.channel_id);
    }
    const ApplicationReadEchoProof proof{intent.channel_id, intent.message_id, captured->serial};
    if (!application_read_state_echo_prepare({&proof, 1})) {
        abandon(owner, intent.channel_id, captured, manual, 507);
        return false;
    }
    // Flags ride along only when they changed (the intent's flags_known), as in the client.
    MessageAckRequest body_request{};
    if (manual) {
        body_request.manual = true;
        body_request.mention_count = intent.mention_count;
        body_request.mention_count_known = intent.mention_count_known;
    } else {
        body_request.token = {stable_storage_data<char>(owner.token), owner.token_bytes};
        body_request.last_viewed = intent.last_viewed;
        body_request.flags = intent.flags;
        body_request.token_known = owner.token_known;
        body_request.last_viewed_known = true;
        body_request.flags_known = intent.flags_known;
    }
    const auto body = write_message_ack_body(
        body_request, {g_app.account_ui->response_shape_scratch.response, kMessageAckMaximumBodyBytes});
    if (!body.ok()) {
        log_write(LogLevel::warning, "read", "ack_body_capacity_or_encoding");
        abandon(owner, intent.channel_id, captured, manual, 400);
        return false;
    }
    const auto submitted = g_app.account.api.message_ack(
        g_app.account.instance, intent.channel_id, intent.message_id,
        {g_app.account_ui->response_shape_scratch.response, body.bytes}, request_id);
    if (!submitted.ok()) {
        owner.submit_retry_at_ms = deadline_after(now_ms, 100);
        if (submitted.error != RestClientSubmitError::busy) {
            log_u64(LogLevel::warning, "read", "ack_submit_error", static_cast<u64>(submitted.error));
            abandon(owner, intent.channel_id, captured, manual, 400);
        }
        return false;
    }
    if (!read_ack_controller_admit(&owner.queue, intent, request_id, &owner.ticket))
        return false;
    auto* latest = action(owner, intent.channel_id);
    if (latest->attempts != 255)
        ++latest->attempts;
    owner.submitted_manual = *latest;
    application_read_state_echo_admitted({&proof, 1}, request_id);
    owner.submitted_user_id = owner.user_id;
    owner.submitted_token_generation = owner.token_generation;
    owner.submit_retry_at_ms = 0;
    g_app.primary_request_id = request_id;
    return true;
}
bool application_read_state_response(const RestClientResponse& response, const u8* body, u32 bytes,
                                     u64 now_ms) noexcept {
    if (!g_app.read_state || !g_app.account_ui)
        return true;
    // A same-actor response may arrive after READY canceled its queued request.
    // Apply its server-confirmed minimum before changing that epoch.
    const bool same_actor = actor_matches(*g_app.read_state, g_app.read_state->submitted_user_id);
    const u64 late_floor =
        same_actor && response.request_id == g_app.read_state->ticket.request_id &&
                response.transport_ok() && response.status_code == 429
            ? (response.retry_at_ms ? response.retry_at_ms : deadline_after(now_ms, 1000))
            : 0;
    application_read_state_ready();
    auto& owner = *g_app.read_state;
    owner.submit_retry_at_ms = maximum(owner.submit_retry_at_ms, late_floor);
    if (!owner.queue.inflight_active || response.request_id != owner.ticket.request_id ||
        owner.ticket.generation != owner.queue.generation)
        return true;
    const bool success =
        response.transport_ok() && response.status_code >= 200 && response.status_code < 300;
    const auto source = owner.submitted_manual;
    const u64 completed_channel = owner.queue.inflight.channel_id;
    const bool manual = source.manual;
    // The verified manual helper ignores its response payload. Only ordinary
    // ACKs rotate the opaque token, including valid older superseded flights.
    if (!manual && scoped(owner) && success && bytes && body && owner.submitted_user_id == owner.user_id &&
        owner.submitted_token_generation == owner.token_generation) {
        MessageAckResponse decoded{};
        if (decode_message_ack_response({reinterpret_cast<const char*>(body), bytes},
                                        {g_app.account_ui->message_history_object_structurals,
                                         kMessageHistoryObjectStructuralCount},
                                        &decoded) &&
            decoded.token_known) {
            if (decoded.token_null)
                clear_token(owner);
            else {
                const auto count = json_string_decode_utf8(decoded.token, {});
                if ((count.ok() || count.error == JsonStringDecodeError::output_too_small) &&
                    count.bytes <= kMessageAckMaximumBodyBytes &&
                    (!count.bytes || stable_storage_reserve(&owner.token, count.bytes))) {
                    clear_token(owner);
                    const auto copied = json_string_decode_utf8(
                        decoded.token, {stable_storage_data<char>(owner.token), count.bytes});
                    if (copied.ok()) {
                        owner.token_bytes = copied.bytes;
                        owner.token_known = true;
                    }
                } else
                    log_write(LogLevel::warning, "read", "ack_token_capacity");
            }
        } else
            log_write(LogLevel::warning, "read", "ack_response_decode");
    }
    const auto* current_action = manual ? action(owner, source.channel_id) : nullptr;
    const bool latest_manual = current_action && current_action->serial == source.serial && current_action->manual;
    if (response.transport_ok() && response.status_code >= 400 && response.status_code < 500)
        application_read_state_echo_rejected(response.request_id);
    const bool transient =
        !response.transport_ok() || response.status_code >= 500 || response.status_code == 429;
    // Retry-After constrains the route even when this action has exhausted its
    // retry budget or a newer action superseded the accepted request.
    if (response.transport_ok() && response.status_code == 429)
        owner.submit_retry_at_ms =
            maximum(owner.submit_retry_at_ms,
                    response.retry_at_ms ? response.retry_at_ms : deadline_after(now_ms, 1'000));
    const bool retry = transient && !api_rejection_requires_action(response.rejection) &&
                       ((!manual && !source.scoped) || source.attempts < 3);
    const u64 retry_delay = source.scoped ? (source.attempts < 2 ? 2000 : 4000) : 1000;
    // Non-rate-limit metadata may extend the ordinary ACK backoff, never shorten it.
    const u64 server_retry_at = response.transport_ok() ? response.retry_at_ms : 0;
    const u64 local_retry_at = deadline_after(now_ms, retry_delay);
    const u64 retry_due = source.scoped ? maximum(response.retry_at_ms, local_retry_at)
                          : response.status_code == 429 && server_retry_at
                              ? server_retry_at
                              : maximum(server_retry_at, local_retry_at);
    const auto outcome = success ? ReadAckCompletion::success
                         : retry ? ReadAckCompletion::retry
                                 : ReadAckCompletion::terminal;
    read_ack_controller_complete(&owner.queue, owner.ticket, outcome, retry ? retry_due : 0);
    if (source.scoped)
        application_scoped_read_individual_result(source.channel_id, source.serial, success, retry);
    if (!success)
        log_u64(LogLevel::warning, "read", "ack_http_status", response.status_code);
    if (latest_manual) {
        if (success) {
            project(owner, source);
            clear_manual_failure(owner, source.channel_id);
            application_message_mutation_notice_succeeded(ApplicationMessageMutationKind::mark_unread,
                                                          source.channel_id, source.message_id);
            refresh_message_composer_ui();
        } else if (!retry)
            manual_failed(owner, source, response);
        prune_action(owner, source.channel_id);
        synchronize_navigation_view();
        application_request_frame();
    } else if (!manual)
        prune_action(owner, completed_channel);
    return true;
}
void application_read_state_cancel(void*, u64 channel_id) noexcept {
    auto* o = g_app.read_state;
    if (!o)
        return;
    retire_channel(*o, channel_id, true);
}
void application_read_state_ack_observed(void*, u64 channel_id, u64 cursor, bool manual) noexcept {
    application_read_state_ready();
    auto* o = g_app.read_state;
    if (!o || !scoped(*o))
        return;
    auto* latest = action(*o, channel_id);
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id);
    auto* echoes = stable_storage_data<ManualReadEcho>(o->echoes);
    u32 matched = ~u32{0};
    u64 matched_serial = ~u64{0};
    for (u32 i = 0; channel && i < o->echo_count; ++i)
        if (echoes[i].channel_id == channel_id && echoes[i].channel_generation == channel->generation &&
            echoes[i].cursor == cursor && echoes[i].serial < matched_serial) {
            if (echoes[i].manual != manual)
                continue;
            matched = i;
            matched_serial = echoes[i].serial;
        }
    if (matched != ~u32{0}) {
        const bool superseded =
            latest && matched_serial < latest->serial && source_current(*o, *latest, false);
        if (echoes[matched].copies > 1)
            --echoes[matched].copies;
        else
            erase_echo(*o, matched);
        if (superseded) {
            const auto source = *latest;
            project(*o, source);
            prune_action(*o, channel_id);
            synchronize_navigation_view();
            application_request_frame();
            return;
        }
    }
    if (!manual) {
        maintain_selected(*o);
        prune_action(*o, channel_id);
        return;
    }
    // A matching current echo or unrelated remote manual action becomes the
    // model's latest state. Only recognized older echoes of our own requests
    // preserve newer work.
    retire_channel(*o, channel_id, false);
}
void application_read_state_reset_token(void*) noexcept {
    if (g_app.read_state)
        clear_token(*g_app.read_state);
}
void application_read_state_release(Application& application) noexcept {
    if (!application.read_state)
        return;
    clear_token(*application.read_state);
    stable_storage_release(&application.read_state->intents);
    stable_storage_release(&application.read_state->token);
    stable_storage_release(&application.read_state->actions);
    stable_storage_release(&application.read_state->echoes);
    VirtualFree(application.read_state, 0, MEM_RELEASE);
    application.read_state = nullptr;
    application.read_last_activity_ms = 0;
}
u32 application_read_state_owner_bytes() noexcept {
    const auto* o = g_app.read_state;
    return o ? static_cast<u32>(sizeof(ApplicationReadState) + o->intents.arena.committed +
                                o->token.arena.committed + o->actions.arena.committed +
                                o->echoes.arena.committed)
             : 0;
}
ApplicationReadStateMetrics application_read_state_metrics() noexcept {
    ApplicationReadStateMetrics m{};
    const auto* o = g_app.read_state;
    if (!o)
        return m;
    MEMORY_BASIC_INFORMATION region{};
    VirtualQuery(o, &region, sizeof(region));
    m.owner_bytes = sizeof(*o);
    m.owner_reserved_bytes = static_cast<u32>(region.RegionSize);
    m.owner_committed_bytes = m.owner_reserved_bytes;
    m.intents_reserved_bytes = static_cast<u32>(o->intents.arena.reserved);
    m.intents_committed_bytes = static_cast<u32>(o->intents.arena.committed);
    m.token_reserved_bytes = static_cast<u32>(o->token.arena.reserved);
    m.token_committed_bytes = static_cast<u32>(o->token.arena.committed);
    m.actions_reserved_bytes = static_cast<u32>(o->actions.arena.reserved);
    m.actions_committed_bytes = static_cast<u32>(o->actions.arena.committed);
    m.echoes_reserved_bytes = static_cast<u32>(o->echoes.arena.reserved);
    m.echoes_committed_bytes = static_cast<u32>(o->echoes.arena.committed);
    m.commits = 1 + o->intents.arena.commit_count + o->token.arena.commit_count +
                o->actions.arena.commit_count + o->echoes.arena.commit_count;
    m.action_serial = o->action_serial;
    return m;
}
bool application_read_state_scoped_prepare(ConstSlice<ReadScopeTarget> targets, bool individual,
                                           u64* first_serial) noexcept {
    if (!first_serial || (!targets.data && targets.count) || targets.count > kReadAckMaximumIntents ||
        !account_live())
        return false;
    application_read_state_ready();
    auto* o = ensure_owner();
    if (!o || o->action_serial == ~u64{0} || targets.count > ~u64{0} - o->action_serial)
        return false;
    u32 actions = o->action_count, pending = o->queue.count, echoes = o->echo_count;
    for (u32 i = 0; i < targets.count; ++i) {
        const auto& t = targets[i];
        ReadActionSource source{};
        source.channel_id = t.channel_id;
        source.channel_generation = t.generation;
        source.scoped = true;
        if (!source_current(*o, source, false) || t.outbound != (t.message_id != 0))
            return false;
        for (u32 j = 0; j < i; ++j)
            if (targets[j].channel_id == t.channel_id)
                return false;
        if (!action(*o, t.channel_id))
            ++actions;
        if (t.outbound) {
            ++echoes;
            if (individual && !has_pending(*o, t.channel_id))
                ++pending;
        }
    }
    if (actions > kReadAckMaximumIntents || pending > kReadAckMaximumIntents ||
        echoes > kReadAckMaximumIntents || (actions && !stable_storage_reserve(&o->actions, actions)) ||
        (pending && !stable_storage_reserve(&o->intents, pending)) ||
        (echoes && !stable_storage_reserve(&o->echoes, echoes)))
        return false;
    o->queue.backed_count = maximum(o->queue.backed_count, pending);
    *first_serial = o->action_serial + 1;
    return true;
}
bool application_read_state_scoped_commit(ConstSlice<ReadScopeTarget> targets, bool individual,
                                          u64 first_serial) noexcept {
    auto* o = g_app.read_state;
    if (!o || !scoped(*o) || !first_serial || first_serial != o->action_serial + 1 ||
        (!targets.data && targets.count) || targets.count > ~u64{0} - o->action_serial)
        return false;
    // Preparation established all writable prefixes. Validate the whole plan
    // and backing again before the first mutation. This cannot invoke UI input.
    u32 required_actions = o->action_count, required_pending = o->queue.count,
        required_echoes = o->echo_count;
    for (u32 i = 0; i < targets.count; ++i) {
        if (!targets[i].channel_id || targets[i].outbound != (targets[i].message_id != 0))
            return false;
        for (u32 j = 0; j < i; ++j)
            if (targets[j].channel_id == targets[i].channel_id)
                return false;
        if (!action(*o, targets[i].channel_id))
            ++required_actions;
        if (targets[i].outbound)
            ++required_echoes;
        if (individual && targets[i].outbound && !has_pending(*o, targets[i].channel_id))
            ++required_pending;
    }
    if (required_actions > o->actions.capacity || required_pending > o->queue.backed_count ||
        required_echoes > o->echoes.capacity)
        return false;
    for (u32 i = 0; i < targets.count; ++i) {
        ReadActionSource source{};
        source.channel_id = targets[i].channel_id;
        source.channel_generation = targets[i].generation;
        source.scoped = true;
        if (!source_current(*o, source, false))
            return false;
    }
    const u32 viewed = viewed_day();
    for (u32 i = 0; i < targets.count; ++i) {
        const auto& t = targets[i];
        const u32 flags = channel_is_thread(t.type) ? 2u : 1u;
        read_ack_controller_cancel(&o->queue, t.channel_id);
        ReadActionSource source{};
        source.channel_id = t.channel_id;
        source.channel_generation = t.generation;
        source.cursor = source.message_id = t.message_id;
        source.serial = ++o->action_serial;
        source.scoped = true;
        store_action(*o, source);
        application_scoped_read_superseded(t.channel_id, source.serial);
        if (o->selected_manual.channel_id == t.channel_id)
            reset_value(&o->selected_manual);
        if (individual && t.outbound) {
            ReadAckIntent intent{t.channel_id, t.message_id, viewed, flags, 0};
            const auto* current = chat_state_store_find_channel(g_app.account_ui->chat_state, t.channel_id);
            intent.flags_known = !current || current->read_flags != flags;
            if (!read_ack_controller_queue(&o->queue, intent, clock_monotonic_milliseconds(), true))
                return false;
        }
        if (!read_state_mark_local(&g_app.account_ui->chat_state, t.channel_id, t.message_id,
                                           viewed, flags))
            return false;
    }
    clear_manual_failure(*o);
    return true;
}
bool application_read_state_action_current(u64 channel_id, u64 serial) noexcept {
    auto* o = g_app.read_state;
    if (!o || !serial || !scoped(*o))
        return false;
    const auto* latest = action(*o, channel_id);
    return latest && latest->serial == serial && source_current(*o, *latest, false);
}
void application_read_state_scoped_cancel(u64 channel_id, u64 serial) noexcept {
    auto* o = g_app.read_state;
    if (!o || !serial)
        return;
    const auto* current = action(*o, channel_id);
    if (!current || !current->scoped || current->serial != serial)
        return;
    read_ack_controller_cancel(&o->queue, channel_id);
    erase_action(*o, channel_id);
}
bool application_read_state_echo_prepare(ConstSlice<ApplicationReadEchoProof> proofs) noexcept {
    auto* o = g_app.read_state;
    if (!o || !scoped(*o) || (!proofs.data && proofs.count))
        return false;
    u32 required = o->echo_count;
    const auto* entries = stable_storage_data<ManualReadEcho>(o->echoes);
    for (u32 i = 0; i < proofs.count; ++i) {
        if (!application_read_state_action_current(proofs[i].channel_id, proofs[i].action_serial) ||
            action(*o, proofs[i].channel_id)->cursor != proofs[i].cursor)
            return false;
        for (u32 j = 0; j < i; ++j)
            if (proofs[j].channel_id == proofs[i].channel_id)
                return false;
        bool found = false;
        for (u32 j = 0; j < o->echo_count; ++j)
            if (entries[j].channel_id == proofs[i].channel_id &&
                entries[j].serial == proofs[i].action_serial && entries[j].cursor == proofs[i].cursor) {
                if (entries[j].copies == 65535)
                    return false;
                found = true;
                break;
            }
        if (!found)
            ++required;
    }
    return required <= kReadAckMaximumIntents &&
           (!required || stable_storage_reserve(&o->echoes, required));
}
void application_read_state_echo_admitted(ConstSlice<ApplicationReadEchoProof> proofs,
                                          u64 request_id) noexcept {
    auto* o = g_app.read_state;
    if (!o || !request_id)
        return;
    auto* entries = stable_storage_data<ManualReadEcho>(o->echoes);
    for (u32 i = 0; i < proofs.count; ++i) {
        const auto* latest = action(*o, proofs[i].channel_id);
        if (!latest || latest->serial != proofs[i].action_serial)
            continue;
        ManualReadEcho* found{};
        for (u32 j = 0; j < o->echo_count; ++j)
            if (entries[j].channel_id == proofs[i].channel_id &&
                entries[j].serial == proofs[i].action_serial && entries[j].cursor == proofs[i].cursor) {
                found = entries + j;
                break;
            }
        if (found) {
            ++found->copies;
            found->request_id = request_id;
        } else
            entries[o->echo_count++] = {latest->channel_id,
                                        proofs[i].cursor,
                                        latest->serial,
                                        request_id,
                                        static_cast<u32>(latest->channel_generation),
                                        latest->manual,
                                        1};
    }
}
void application_read_state_echo_rejected(u64 request_id) noexcept {
    auto* o = g_app.read_state;
    if (!o)
        return;
    auto* entries = stable_storage_data<ManualReadEcho>(o->echoes);
    for (u32 i = 0; i < o->echo_count;) {
        if (entries[i].request_id != request_id) {
            ++i;
            continue;
        }
        if (entries[i].copies > 1) {
            --entries[i].copies;
            ++i;
        } else
            erase_echo(*o, i);
    }
}
} // namespace maicord::app_detail
