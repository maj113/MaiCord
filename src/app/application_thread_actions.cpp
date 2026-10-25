#include "application_thread_actions.hpp"
#include "maicord/core/memory.hpp"
#include "application_state.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/protocol/channel_overview.hpp"
#include "application_account_access.hpp"

namespace maicord::app_detail {
namespace {
using Operation = ThreadActionOperation;
using Phase = ApplicationThreadActionPhase;
using Admission = ApplicationThreadActionEnqueueResult;
using Membership = ApplicationThreadMembershipStatus;
struct Flight {
    ApplicationThreadActionTicket source;
    u64 request_id;
    Operation operation;
    bool active, retired, cancel_requested, metadata;
};
struct ThreadOwner {
    ApplicationThreadActionTicket source;
    Flight flight;
    ApplicationThreadActionTicket refusal_source;
    Utf8View refusal_text;
    Utf8View status;
    u64 actor_floor, due;
    u32 revision;
    Operation operation;
    Phase phase;
    bool error, closing, metadata_checked;
};
static_assert(sizeof(ThreadOwner) <= 4096);
ThreadOwner* owner{};
u64 next_serial{};
bool thread(u8 type) noexcept {
    return type == 10 || type == 11 || type == 12;
}
bool actor_live() noexcept {
    return g_app.account_ui && g_app.account.instance && g_app.account.status.running &&
           !g_app.account.stop_failed && !g_app.login.visible && g_app.plugin_account_scope_serial &&
           g_app.account.status.profile_id && g_app.account_ui->chat_state.self_user_id;
}
bool actor_current(const ApplicationThreadActionTicket& source) noexcept {
    return actor_live() && source.account_scope == g_app.plugin_account_scope_serial &&
           source.profile_id == g_app.account.status.profile_id &&
           source.self_user_id == g_app.account_ui->chat_state.self_user_id &&
           source.account_instance == reinterpret_cast<usize>(g_app.account.instance);
}
const ChannelRecord* source_current(const ApplicationThreadActionTicket& source) noexcept {
    const auto* value = actor_current(source) ? application_channel_record(source.thread_id) : nullptr;
    return value && value->occupied && thread(value->type) && value->generation == source.channel_generation
               ? value
               : nullptr;
}
bool readable(const ChannelRecord& value) noexcept {
    constexpr u32 required = channel_action_view | channel_action_read_history;
    if (!value.occupied || (value.flags & kChannelObfuscatedFlag) ||
        !channel_is_message_capable(value.type) ||
        !chat_state_store_channel_visible(g_app.account_ui->chat_state, value) ||
        (value.actions & required) != required)
        return false;
    if (!value.guild_id)
        return value.type == 1 || value.type == 3;
    const auto* guild = chat_state_store_find_guild(g_app.account_ui->chat_state, value.guild_id);
    return guild && !guild->unavailable && value.permissions_resolved;
}
bool fetchable(const ChannelRecord& value) noexcept {
    if (!thread(value.type) || !value.guild_id || !value.parent_id ||
        (value.flags & kChannelObfuscatedFlag))
        return false;
    const auto* parent = application_channel_record(value.parent_id);
    const auto* guild = chat_state_store_find_guild(g_app.account_ui->chat_state, value.guild_id);
    constexpr u32 required = channel_action_view | channel_action_read_history;
    return parent && parent->occupied && parent->guild_id == value.guild_id &&
           channel_is_thread_parent(parent->type) &&
           !(parent->flags & kChannelObfuscatedFlag) && guild && !guild->unavailable &&
           parent->permissions_resolved && (parent->actions & required) == required &&
           chat_state_store_channel_visible(g_app.account_ui->chat_state, *parent);
}
u8 available(const ChannelRecord& value) noexcept {
    u8 actions = fetchable(value) ? thread_action_fetch : 0;
    if (g_app.account.status.required_action != AccountRequirement::none)
        return actions;
    if (!readable(value))
        return actions;
    const bool manage = (value.effective_permissions & kPermissionManageThreads) != 0;
    if (value.thread_archived_known && !value.thread_archived && value.thread_self_joined_known) {
        if (value.thread_self_joined)
            actions |= thread_action_leave;
        else if (value.type != 12 || manage)
            actions |= thread_action_join;
    }
    if (value.thread_archived_known && value.thread_archived && value.thread_locked_known &&
        (manage ||
         (!value.thread_locked && (value.effective_permissions & kPermissionSendMessagesInThreads))))
        actions |= thread_action_unarchive;
    return actions;
}
u8 flag(Operation operation) noexcept {
    switch (operation) {
    case Operation::fetch_self:
        return thread_action_fetch;
    case Operation::join:
        return thread_action_join;
    case Operation::leave:
        return thread_action_leave;
    case Operation::unarchive:
        return thread_action_unarchive;
    }
    return 0;
}
ApplicationThreadActionTicket capture(const ChannelRecord& value) noexcept {
    return {0,
            g_app.plugin_account_scope_serial,
            g_app.account.status.profile_id,
            g_app.account_ui->chat_state.self_user_id,
            value.id,
            reinterpret_cast<usize>(g_app.account.instance),
            value.revision,
            owner ? owner->revision : 0,
            value.generation};
}
void publish(Utf8View status, Phase phase, bool error = false) noexcept {
    if (owner->phase == phase && owner->error == error && utf8_equal(owner->status, status))
        return;
    owner->status = status;
    owner->phase = phase;
    owner->error = error;
    ++owner->revision;
    application_request_frame();
}
bool queued() noexcept {
    return owner && (owner->phase == Phase::queued || owner->phase == Phase::retry_wait);
}
void cancel_flight() noexcept {
    if (!owner || !owner->flight.active)
        return;
    auto& flight = owner->flight;
    flight.retired = true;
    if (!flight.cancel_requested &&
        flight.source.account_instance == reinterpret_cast<usize>(g_app.account.instance)) {
        // Both stages use the same DETAILS transport. The nullable thread
        // callback cancels that ID; channel_overview has no cancel opcode.
        if (g_app.account.api.thread_action)
            g_app.account.api.thread_action(g_app.account.instance, nullptr, flight.request_id);
        else
            return;
        flight.cancel_requested = true;
    }
}
void invalidate_uncertain_membership(const Flight& flight) noexcept {
    const auto* value = source_current(flight.source);
    if (value && value->revision == flight.source.channel_revision &&
        (flight.operation == Operation::join || flight.operation == Operation::leave))
        chat_state_store_set_thread_self_membership(&g_app.account_ui->chat_state, value->id,
                                                       flight.source.self_user_id, false, false);
}
void free_owner() noexcept {
    VirtualFree(owner, 0, MEM_RELEASE);
    owner = nullptr;
}
Admission queue(u64 id, Operation operation, ApplicationThreadActionTicket* output) noexcept {
    application_thread_actions_synchronize();
    const auto* value = actor_live() ? application_channel_record(id) : nullptr;
    if (!value || !thread(value->type))
        return Admission::stale;
    if (!flag(operation))
        return Admission::invalid;
    if (!g_app.account.api.thread_action)
        return Admission::invalid;
    if (!(available(*value) & flag(operation)))
        return Admission::permission;
    if (owner && owner->closing)
        return Admission::busy;
    if (owner && source_current(owner->source) && owner->source.thread_id == id &&
        owner->operation == operation && (queued() || owner->phase == Phase::submitting)) {
        if (output)
            *output = owner->source;
        return Admission::coalesced;
    }
    if (!owner) {
        owner = static_cast<ThreadOwner*>(
            VirtualAlloc(nullptr, sizeof(ThreadOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!owner)
            return Admission::storage;
    }
    // One latest choice replaces any unsubmitted choice. An already accepted
    // request stays as a cancelled entry until its response is consumed.
    cancel_flight();
    if (owner->flight.active)
        invalidate_uncertain_membership(owner->flight);
    owner->source = capture(*value);
    owner->source.serial = ++next_serial;
    if (!owner->source.serial)
        owner->source.serial = ++next_serial;
    owner->operation = operation;
    owner->metadata_checked = false;
    reset_value(&owner->refusal_source);
    reset_value(&owner->refusal_text);
    owner->due = owner->actor_floor;
    ++owner->revision;
    publish(utf8_literal("Updating thread…"), Phase::queued);
    application_request_frame();
    if (output)
        *output = owner->source;
    return Admission::accepted;
}
bool channel_receipt(const Flight& flight, const ChannelRecord& current, Utf8View body,
                     GatewayClientStateMutation* output) noexcept {
    auto& ui = *g_app.account_ui;
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    ConstSlice<u8> payload{};
    const auto result = gateway_client_route_channel_create(
        {body, 0, JsonValueKind::object}, {ui.message_history_page_structurals, kMessageHistoryPageStructuralCount}, 32,
        {reinterpret_cast<char*>(&ui.projection_scratch), sizeof(ui.projection_scratch)},
        flight.source.self_user_id, &event, &payload);
    const bool valid = result.ok() && event.state.id == current.id &&
                       event.state.guild_id == current.guild_id &&
                       event.state.parent_id == current.parent_id && event.state.type == current.type;
    if (valid)
        *output = event.state;
    return valid;
}
} // namespace

ApplicationThreadActionView application_thread_actions_view(u64 id) noexcept {
    ApplicationThreadActionView result{};
    const auto* value = actor_live() ? application_channel_record(id) : nullptr;
    if (!value || !thread(value->type)) {
        result.membership = Membership::stale;
        return result;
    }
    result.ticket = capture(*value);
    result.membership = value->thread_self_joined_known
                            ? value->thread_self_joined ? Membership::joined : Membership::not_joined
                            : Membership::unknown;
    result.archived = value->thread_archived;
    result.archived_known = value->thread_archived_known;
    result.locked = value->thread_locked;
    result.locked_known = value->thread_locked_known;
    result.actions = g_app.account.api.thread_action ? available(*value) : 0;
    if (owner && source_current(owner->source) && owner->source.thread_id == id) {
        result.ticket.serial = owner->source.serial;
        result.phase = owner->phase;
        result.operation = owner->operation;
        result.status = owner->status;
        result.error = owner->error;
        if (queued() || owner->phase == Phase::submitting) {
            result.actions = thread_action_cancel;
            if (owner->operation == Operation::fetch_self && !value->thread_self_joined_known)
                result.membership = Membership::pending;
        } else if (owner->phase == Phase::failed && (available(*value) & flag(owner->operation)))
            result.actions |= thread_action_retry;
    }
    if (owner && owner->refusal_text.bytes && owner->refusal_source.thread_id == id &&
        source_current(owner->refusal_source) &&
        owner->refusal_source.channel_revision == value->revision) {
        result.status = owner->refusal_text;
        result.error = true;
    }
    return result;
}
bool application_thread_actions_current(const ApplicationThreadActionTicket& source) noexcept {
    const auto* value = source_current(source);
    return value && value->revision == source.channel_revision &&
           source.publication_revision == (owner ? owner->revision : 0) &&
           source.serial ==
               (owner && owner->source.thread_id == source.thread_id ? owner->source.serial : 0);
}
Admission application_thread_actions_activate(const ApplicationThreadActionTicket& source,
                                              Operation operation) noexcept {
    if (!application_thread_actions_current(source))
        return Admission::stale;
    const auto result = queue(source.thread_id, operation, nullptr);
    if (result != Admission::accepted && result != Admission::coalesced)
        application_thread_actions_admission_status(source, result);
    return result;
}
Admission application_thread_actions_fetch(u64 id, ApplicationThreadActionTicket* ticket) noexcept {
    if (ticket)
        reset_value(ticket);
    return queue(id, Operation::fetch_self, ticket);
}
Membership application_thread_actions_membership(const ApplicationThreadActionTicket& source) noexcept {
    const auto* value = source_current(source);
    if (!value || !owner || owner->source.serial != source.serial ||
        owner->source.thread_id != source.thread_id)
        return Membership::stale;
    if (value->revision != owner->source.channel_revision && value->thread_self_joined_known)
        return value->thread_self_joined ? Membership::joined : Membership::not_joined;
    if (queued() || owner->phase == Phase::submitting)
        return Membership::pending;
    if (owner->phase == Phase::failed)
        return Membership::failed;
    if (owner->phase == Phase::cancelled)
        return Membership::stale;
    return value->thread_self_joined_known
               ? value->thread_self_joined ? Membership::joined : Membership::not_joined
               : Membership::unknown;
}
bool application_thread_actions_retry(const ApplicationThreadActionTicket& source) noexcept {
    return application_thread_actions_current(source) && owner && owner->phase == Phase::failed &&
           queue(source.thread_id, owner->operation, nullptr) == Admission::accepted;
}
bool application_thread_actions_cancel(const ApplicationThreadActionTicket& source) noexcept {
    if (!application_thread_actions_current(source) || !owner ||
        (!queued() && owner->phase != Phase::submitting))
        return false;
    cancel_flight();
    if (owner->flight.active)
        invalidate_uncertain_membership(owner->flight);
    publish(Utf8View{}, Phase::cancelled);
    return true;
}
Utf8View application_thread_actions_admission_text(Admission value) noexcept {
    switch (value) {
    case Admission::accepted:
    case Admission::coalesced:
        return {};
    case Admission::busy:
        return utf8_literal("Still working on the previous action.");
    case Admission::permission:
        return utf8_literal("You don't have permission to do that.");
    case Admission::stale:
        return utf8_literal("This thread changed. Try again.");
    case Admission::storage:
        return utf8_literal("Something went wrong. Try again.");
    default:
        return utf8_literal("Thread actions aren't available right now.");
    }
}
bool application_thread_actions_admission_status(const ApplicationThreadActionTicket& source,
                                                 Admission why) noexcept {
    if (why == Admission::accepted || why == Admission::coalesced || why == Admission::stale ||
        !application_thread_actions_current(source))
        return false;
    if (!owner) {
        owner = static_cast<ThreadOwner*>(
            VirtualAlloc(nullptr, sizeof(ThreadOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!owner)
            return false;
        owner->source = source;
    }
    if (owner->source.thread_id != source.thread_id || !source_current(owner->source)) {
        const auto text = application_thread_actions_admission_text(why);
        if (owner->refusal_source.thread_id == source.thread_id &&
            owner->refusal_source.channel_revision == source.channel_revision &&
            utf8_equal(owner->refusal_text, text))
            return true;
        owner->refusal_source = source;
        owner->refusal_text = text;
        ++owner->revision;
        application_request_frame();
        return true;
    }
    publish(application_thread_actions_admission_text(why), owner->phase, true);
    return true;
}
bool application_thread_actions_synchronize() noexcept {
    if (!owner)
        return true;
    if (owner->closing || !source_current(owner->source)) {
        owner->closing = true;
        cancel_flight();
        if (!owner->flight.active)
            free_owner();
        return true; // A canceled in-flight request must still be consumable by the shared worker queue.
    }
    if (owner->phase == Phase::submitting && !fetchable(*application_channel_record(owner->source.thread_id))) {
        cancel_flight();
        invalidate_uncertain_membership(owner->flight);
        publish(utf8_literal("Your access to this thread changed."), Phase::failed, true);
    }
    if (queued() && !(available(*application_channel_record(owner->source.thread_id)) & flag(owner->operation)))
        publish(utf8_literal("Your access to this thread changed."), Phase::failed,
                true);
    return true;
}
bool application_thread_actions_pending(u64 now) noexcept {
    application_thread_actions_synchronize();
    return queued() && !owner->closing && !owner->flight.active &&
           now >= maximum(owner->due, owner->actor_floor);
}
bool application_thread_actions_submit(u64 id, u64 now) noexcept {
    if (!id || !application_thread_actions_pending(now))
        return false;
    const auto* value = application_channel_record(owner->source.thread_id);
    if (!value || !(available(*value) & flag(owner->operation)))
        return false;
    ThreadActionRequest request{value->id, owner->source.self_user_id, owner->operation, {}};
    const bool metadata = owner->operation == Operation::fetch_self && !owner->metadata_checked &&
                          (!value->thread_archived_known || !value->thread_locked_known);
    if (metadata && !g_app.account.api.channel_overview) {
        publish(utf8_literal("Couldn't check this thread."), Phase::failed, true);
        return false;
    }
    const auto result = metadata ? g_app.account.api.channel_overview(g_app.account.instance,
                                                                      ChannelOverviewOperation::fetch,
                                                                      value->id, nullptr, id)
                                 : g_app.account.api.thread_action(g_app.account.instance, &request, id);
    if (!result.ok()) {
        if (result.error == RestClientSubmitError::busy)
            owner->due = deadline_after(now, 50);
        else
            publish(utf8_literal("Couldn't update the thread. Try again."),
                    Phase::failed, true);
        return false;
    }
    owner->source.channel_revision = value->revision;
    owner->flight = {owner->source, id, owner->operation, true, false, false, metadata};
    publish(utf8_literal("Updating thread status…"), Phase::submitting);
    return true;
}
bool application_thread_actions_response(const RestClientResponse& response,
                                         Utf8View body) noexcept {
    if (!owner || !owner->flight.active || !response.request_id ||
        response.request_id != owner->flight.request_id)
        return false;
    const auto flight = owner->flight;
    reset_value(&owner->flight);
    const auto* value = source_current(flight.source);
    const bool same =
        !flight.retired && !owner->closing && value && owner->source.serial == flight.source.serial;
    const u64 now = clock_monotonic_milliseconds();
    if (actor_current(flight.source) && response.transport_ok() && response.status_code == 429 &&
        response.rate_limited && !api_rejection_requires_action(response.rejection))
        owner->actor_floor =
            maximum(owner->actor_floor, maximum(response.retry_at_ms, deadline_after(now, 50)));
    if (!same) {
        if (!owner->closing)
            invalidate_uncertain_membership(flight);
        application_thread_actions_synchronize();
        return true;
    }
    if (response.transport_ok() && response.status_code == 429 && response.rate_limited &&
        !api_rejection_requires_action(response.rejection)) {
        owner->due = owner->actor_floor;
        publish(utf8_literal("Too many requests. Retrying…"),
                Phase::retry_wait);
        return true;
    }
    if (response.status_code == 401 || api_rejection_requires_action(response.rejection) ||
        response.error == RestClientError::account_restricted) {
        publish(response.status_code == 401
                    ? utf8_literal("Your session expired. Sign in again.")
                    : utf8_literal(
                          "Verify your account to continue."),
                Phase::failed, true);
        return true;
    }
    // Any newer channel update is the latest server state. Verify current membership
    // rather than overwriting it with an older mutation response.
    if (value->revision != flight.source.channel_revision) {
        owner->operation = Operation::fetch_self;
        owner->metadata_checked = false;
        owner->due = owner->actor_floor;
        publish(utf8_literal("Checking thread…"), Phase::queued);
        return true;
    }
    bool success = false;
    auto& state = g_app.account_ui->chat_state;
    if (flight.metadata) {
        GatewayClientStateMutation metadata{};
        if (response.transport_ok() && response.status_code == 200 && body.data && body.bytes &&
            body.bytes == response.body_bytes && channel_receipt(flight, *value, body, &metadata)) {
            ChannelInput input{value->id,       value->guild_id, value->parent_id, {},
                                      value->position, value->flags,    value->type};
            input.thread_archived = metadata.thread_archived;
            input.thread_archived_known = metadata.thread_archived_known;
            input.thread_locked = metadata.thread_locked;
            input.thread_locked_known = metadata.thread_locked_known;
            if (chat_state_store_upsert_channel(&state, input).ok()) {
                owner->metadata_checked = true;
                owner->due = owner->actor_floor;
                publish(utf8_literal("Checking thread…"), Phase::queued);
                return true;
            }
        }
        publish(utf8_literal("Couldn't check this thread. Try again."), Phase::failed,
                true);
        return true;
    }
    if (flight.operation == Operation::fetch_self) {
        ThreadMemberSnapshot member{};
        const bool joined =
            response.transport_ok() && response.status_code == 200 && body.bytes == response.body_bytes &&
            thread_member_decode(body, value->id, flight.source.self_user_id, &member).ok();
        const bool absent = response.transport_ok() && response.status_code == 404;
        if (joined || absent) {
            success = chat_state_store_set_thread_self_membership(&state, value->id,
                                                                     flight.source.self_user_id, joined);
            if (success && joined)
                chat_state_store_set_thread_join_time(&state, value->id, flight.source.self_user_id,
                                                      thread_join_seconds_from_ms(member.join_timestamp_ms));
        } else if (response.transport_ok() && response.status_code == 403)
            chat_state_store_set_thread_self_membership(&state, value->id, flight.source.self_user_id,
                                                           false, false);
    } else if (flight.operation == Operation::unarchive) {
        GatewayClientStateMutation metadata{};
        if (response.transport_ok() && response.status_code == 200 && body.data && body.bytes &&
            body.bytes == response.body_bytes && channel_receipt(flight, *value, body, &metadata) &&
            metadata.thread_archived_known && !metadata.thread_archived) {
            success = chat_state_store_set_thread_archived(&state, value->id, flight.source.self_user_id,
                                                              false);
            if (success)
                chat_state_store_set_thread_active(&state, value->id, flight.source.self_user_id, true);
        }
    } else if (response.transport_ok() && response.status_code == 204) {
        success = chat_state_store_set_thread_self_membership(
            &state, value->id, flight.source.self_user_id, flight.operation == Operation::join);
        // The server's THREAD_MEMBER_UPDATE replaces this local join time.
        if (success && flight.operation == Operation::join)
            chat_state_store_set_thread_join_time(&state, value->id, flight.source.self_user_id,
                                                  thread_join_seconds_from_ms(clock_unix_milliseconds()));
    }
    if (!response.transport_ok())
        invalidate_uncertain_membership(flight);
    publish(success ? Utf8View{} : utf8_literal("Couldn't update the thread. Try again."),
            success ? Phase::settled : Phase::failed, !success);
    return true;
}
u64 application_thread_actions_next_wake() noexcept {
    if (!queued() || owner->closing || owner->flight.active ||
        g_app.details_request_owner != MessageHistoryRequestOwner::none)
        return 0;
    const auto due = maximum(owner->due, owner->actor_floor);
    return due ? due : 1;
}
void application_thread_actions_release() noexcept {
    if (!owner)
        return;
    owner->closing = true;
    cancel_flight();
    if (owner->flight.active &&
        owner->flight.source.account_instance == reinterpret_cast<usize>(g_app.account.instance) &&
        g_app.details_request_id == owner->flight.request_id)
        return;
    free_owner();
}
u32 application_thread_actions_publication_revision() noexcept {
    return owner ? owner->revision : 0;
}
ApplicationThreadActionMetrics application_thread_actions_metrics() noexcept {
    if (!owner)
        return {};
    Win32MemoryInformation memory{};
    const auto bytes = VirtualQuery(owner, memory.native(), sizeof(memory));
    return {sizeof(ThreadOwner),
            bytes && memory.State == MEM_COMMIT ? static_cast<u32>(memory.RegionSize) : 0, 1,
            owner->revision, owner->flight.active};
}
bool application_thread_actions_channel_readable(u64 id) noexcept {
    const auto* value = actor_live() ? application_channel_record(id) : nullptr;
    return value && readable(*value);
}
bool application_thread_actions_channel_sendable(u64 id) noexcept {
    const auto* value = actor_live() ? application_channel_record(id) : nullptr;
    constexpr u32 required = channel_action_view | channel_action_send_messages;
    if (!value || !value->occupied || !channel_is_message_capable(value->type) ||
        (value->flags & kChannelObfuscatedFlag) || (value->actions & required) != required ||
        !chat_state_store_channel_visible(g_app.account_ui->chat_state, *value) ||
        g_app.account.status.required_action != AccountRequirement::none)
        return false;
    if (!value->guild_id)
        return value->type == 1 || value->type == 3;
    const auto* guild = chat_state_store_find_guild(g_app.account_ui->chat_state, value->guild_id);
    return guild && !guild->unavailable && value->permissions_resolved &&
           (!thread(value->type) || (value->thread_archived_known && !value->thread_archived));
}
} // namespace maicord::app_detail
