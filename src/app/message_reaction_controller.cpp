#include "maicord/app/message_reaction_controller.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
static_assert(sizeof(MessageReactionIntent) == 232);
static_assert(sizeof(MessageReactionController) == 1408);
static_assert(sizeof(MessageReactionTicket) == 24);
namespace {
constexpr u32 kMissingIndex = 0xffffffffu;
u64 add_time(u64 value, u64 delay) noexcept {
    return value > ~u64{0} - delay ? ~u64{0} : value + delay;
}

bool actor_equal(MessageReactionActor left, MessageReactionActor right) noexcept {
    return left.account_scope == right.account_scope && left.profile_id == right.profile_id &&
           left.self_user_id == right.self_user_id && left.instance == right.instance;
}

bool actor_valid(MessageReactionActor actor) noexcept {
    return actor.account_scope && actor.profile_id && actor.self_user_id && actor.instance;
}

bool source_equal(MessageReactionSource left, MessageReactionSource right) noexcept {
    return left.handle.slot == right.handle.slot && left.handle.generation == right.handle.generation &&
           left.revision == right.revision && left.channel_generation == right.channel_generation;
}

bool source_valid(MessageReactionSource source) noexcept {
    return source.handle.generation && source.channel_generation && source.revision;
}

bool emoji_equal(const MessageReactionTarget& left, const MessageReactionTarget& right) noexcept {
    return left.channel_id == right.channel_id && left.message_id == right.message_id &&
           left.emoji_id == right.emoji_id &&
           (left.emoji_id ||
            (left.name_bytes <= kMessageReactionMaximumEmojiNameBytes &&
             right.name_bytes <= kMessageReactionMaximumEmojiNameBytes &&
             left.name_bytes == right.name_bytes && bytes_equal(left.name, right.name, left.name_bytes)));
}

bool target_valid(const MessageReactionTarget& target, bool route) noexcept {
    return target.channel_id && target.message_id &&
           target.name_bytes <= kMessageReactionMaximumEmojiNameBytes &&
           (target.type == MessageReactionType::normal || target.type == MessageReactionType::burst ||
            (!route && target.type == MessageReactionType::vote)) &&
           ((target.name_bytes && utf8_validate({target.name, target.name_bytes}).valid) ||
            (!route && target.emoji_id));
}

bool authority_valid(MessageReactionAuthority authority) noexcept {
    return authority.known && authority.normal <= authority.total && authority.burst <= authority.total &&
           u64{authority.normal} + authority.burst <= authority.total &&
           (!authority.mine_normal || authority.normal) && (!authority.mine_burst || authority.burst);
}

bool mine(MessageReactionAuthority authority, MessageReactionType type) noexcept {
    return type == MessageReactionType::burst ? authority.mine_burst : authority.mine_normal;
}

void set_mine(MessageReactionAuthority& authority, MessageReactionType type, bool value) noexcept {
    const bool old = mine(authority, type);
    if (old == value)
        return;
    u32& count = type == MessageReactionType::burst ? authority.burst : authority.normal;
    if (value) {
        if (count != ~u32{0})
            ++count;
        if (authority.total != ~u32{0})
            ++authority.total;
    } else {
        if (count)
            --count;
        if (authority.total)
            --authority.total;
    }
    if (type == MessageReactionType::burst)
        authority.mine_burst = value;
    else
        authority.mine_normal = value;
    authority.present = authority.total != 0;
}

u32 find_intent(const MessageReactionController& controller, const MessageReactionTarget& target) noexcept {
    for (u32 index = 0; index < controller.count; ++index)
        if (message_reaction_target_equal(controller.entries[index].target, target))
            return index;
    return kMissingIndex;
}

void erase_intent(MessageReactionController& controller, u32 index) noexcept {
    for (u32 candidate = index + 1; candidate < controller.count; ++candidate)
        controller.entries[candidate - 1] = controller.entries[candidate];
    reset_value(&controller.entries[--controller.count]);
}

bool flight_matches(const MessageReactionController& controller,
                    const MessageReactionIntent& intent) noexcept {
    return controller.flight.active && !controller.flight.retired &&
           actor_equal(controller.actor, controller.flight.actor) &&
           message_reaction_target_equal(controller.flight.intent.target, intent.target) &&
           source_equal(controller.flight.intent.source, intent.source);
}

u64 rate_floor(const MessageReactionController& controller, u64 channel_id) noexcept {
    u64 result = controller.global_cooldown_ms;
    for (const auto& entry : controller.cooldowns)
        if (entry.channel_id == channel_id && entry.until_ms > result)
            result = entry.until_ms;
    return result;
}

u64 send_due(const MessageReactionController& controller, const MessageReactionIntent& intent) noexcept {
    const u64 rate = rate_floor(controller, intent.target.channel_id);
    return intent.due_at_ms > rate ? intent.due_at_ms : rate;
}

void retain_channel_cooldown(MessageReactionController& controller, u64 channel_id, u64 until,
                             u64 now_ms) noexcept {
    u32 vacant = kMissingIndex;
    for (u32 index = 0; index < kMessageReactionMaximumIntents; ++index) {
        auto& entry = controller.cooldowns[index];
        if (entry.channel_id == channel_id) {
            if (until > entry.until_ms)
                entry.until_ms = until;
            return;
        }
        if (!entry.channel_id || entry.until_ms <= now_ms)
            vacant = index;
    }
    if (vacant != kMissingIndex)
        controller.cooldowns[vacant] = {channel_id, until};
    else if (until > controller.global_cooldown_ms)
        controller.global_cooldown_ms = until;
}

void shared_authority(MessageReactionController& controller, const MessageReactionIntent& intent,
                      MessageReactionAuthority authority, u64 revision, bool own = true) noexcept {
    for (u32 index = 0; index < controller.count; ++index) {
        auto& entry = controller.entries[index];
        if (emoji_equal(entry.target, intent.target) && source_equal(entry.source, intent.source)) {
            entry.authority = authority;
            if (own && entry.target.type == intent.target.type)
                entry.mine_revision = revision;
        }
    }
}

MessageReactionProjection project_intent(const MessageReactionController& controller,
                                         const MessageReactionTarget& target,
                                         const MessageReactionSource& source,
                                         const MessageReactionAuthority& authority) noexcept {
    MessageReactionProjection projected{authority, authority.mine_normal, authority.mine_burst, false};
    for (u32 index = 0; index < controller.count; ++index) {
        const auto& entry = controller.entries[index];
        if (!emoji_equal(entry.target, target) || !source_equal(entry.source, source))
            continue;
        if (entry.target.type == MessageReactionType::burst)
            projected.desired_burst = entry.desired_mine;
        else
            projected.desired_normal = entry.desired_mine;
    }
    projected.optimistic = projected.desired_normal != projected.authority.mine_normal ||
                           projected.desired_burst != projected.authority.mine_burst;
    return projected;
}

MessageReactionProjection project_intent(const MessageReactionController& controller,
                                         const MessageReactionIntent& intent) noexcept {
    return project_intent(controller, intent.target, intent.source, intent.authority);
}

// The outcome keeps the intent's identity; its projection is taken separately
// so an intent can be erased in between without being copied.
void begin_outcome(MessageReactionOutcome* result, const MessageReactionIntent& intent,
                   MessageReactionOutcomeKind kind) noexcept {
    reset_value(result);
    copy_value(&result->target, intent.target);
    result->source = intent.source;
    result->serial = intent.serial;
    result->kind = kind;
    result->project = true;
}

void make_outcome(MessageReactionOutcome* result, const MessageReactionController& controller,
                  const MessageReactionIntent& intent, MessageReactionOutcomeKind kind) noexcept {
    begin_outcome(result, intent, kind);
    result->projection = project_intent(controller, intent);
}

// Outcome for an intent that is erased: its projection no longer counts it.
void erase_with_outcome(MessageReactionOutcome* result, MessageReactionController& controller, u32 index,
                        MessageReactionOutcomeKind kind) noexcept {
    const MessageReactionIntent& intent = controller.entries[index];
    begin_outcome(result, intent, kind);
    const MessageReactionAuthority authority = intent.authority;
    erase_intent(controller, index);
    result->projection = project_intent(controller, result->target, result->source, authority);
}
} // namespace
bool message_reaction_target_equal(const MessageReactionTarget& left,
                                   const MessageReactionTarget& right) noexcept {
    return left.type == right.type && emoji_equal(left, right);
}

bool message_reaction_controller_initialize(MessageReactionController* controller,
                                            Slice<MessageReactionIntent> storage,
                                            MessageReactionActor actor) noexcept {
    if (!controller || !storage.data || !storage.count || storage.count > kMessageReactionMaximumIntents ||
        !actor_valid(actor))
        return false;
    reset_value(controller);
    controller->entries = storage;
    controller->actor = actor;
    for (u32 index = 0; index < storage.count; ++index)
        reset_value(&storage[index]);
    return true;
}

bool message_reaction_controller_grow_storage(MessageReactionController* controller,
                                              Slice<MessageReactionIntent> storage) noexcept {
    if (!controller || !storage.data || !storage.count || storage.data != controller->entries.data ||
        storage.count < controller->entries.count || storage.count > kMessageReactionMaximumIntents)
        return false;
    for (u32 index = controller->entries.count; index < storage.count; ++index)
        reset_value(&storage[index]);
    controller->entries = storage;
    return true;
}

void message_reaction_controller_queue(MessageReactionOutcome* out, MessageReactionController* controller,
                                       const MessageReactionTarget& target,
                                                         MessageReactionSource source,
                                                         MessageReactionAuthority authority, bool desired,
                                                         u64 now_ms) noexcept {
    reset_value(out);
    if (!controller || !actor_valid(controller->actor) || !target_valid(target, true) ||
        !source_valid(source) || !authority_valid(authority)) {
        out->error = MessageReactionError::invalid_input;
        return;
    }
    const MessageReactionTarget copied_target = target;
    u32 index = find_intent(*controller, copied_target);
    if (controller->serial == ~u64{0}) {
        out->error = MessageReactionError::serial_exhausted;
        return;
    }
    u32 reclaimed = 0;
    for (u32 candidate = 0; candidate < controller->count; ++candidate)
        if (emoji_equal(controller->entries[candidate].target, copied_target) &&
            !source_equal(controller->entries[candidate].source, source))
            ++reclaimed;
    if (index == kMissingIndex && controller->count == controller->entries.count && !reclaimed) {
        out->error = MessageReactionError::capacity;
        return;
    }
    // A source replacement is one barrier across normal/burst intents for this
    // emoji. Preflight capacity above precedes any retirement or projection.
    for (u32 candidate = 0; candidate < controller->count;) {
        if (emoji_equal(controller->entries[candidate].target, copied_target) &&
            !source_equal(controller->entries[candidate].source, source)) {
            if (flight_matches(*controller, controller->entries[candidate]))
                controller->flight.retired = true;
            erase_intent(*controller, candidate);
        } else
            ++candidate;
    }
    index = find_intent(*controller, copied_target);
    if (index == kMissingIndex) {
        for (u32 candidate = 0; candidate < controller->count; ++candidate)
            if (emoji_equal(controller->entries[candidate].target, copied_target) &&
                source_equal(controller->entries[candidate].source, source)) {
                authority = controller->entries[candidate].authority;
                break;
            }
        index = controller->count++;
        reset_value(&controller->entries[index]);
        controller->entries[index].authority = authority;
        controller->entries[index].mine_revision = controller->mine_revision;
        controller->entries[index].deadline_at_ms = add_time(now_ms, kMessageReactionIntentLifetimeMs);
        controller->entries[index].due_at_ms = now_ms;
    }
    auto& intent = controller->entries[index];
    const bool was_awaiting_echo = intent.awaiting_echo;
    intent.target = copied_target;
    intent.source = source;
    intent.desired_mine = desired;
    intent.awaiting_echo = false;
    intent.echo_matches = false;
    intent.serial = ++controller->serial;
    ++controller->revision;
    if (desired == mine(intent.authority, intent.target.type) && !flight_matches(*controller, intent)) {
        if (was_awaiting_echo) {
            intent.awaiting_echo = true;
            return make_outcome(out, *controller, intent, MessageReactionOutcomeKind::settled);
        }
        return erase_with_outcome(out, *controller, index, MessageReactionOutcomeKind::settled);
    }
    return make_outcome(out, *controller, intent, MessageReactionOutcomeKind::queued);
}

bool message_reaction_controller_project(const MessageReactionController& controller,
                                         const MessageReactionTarget& target,
                                         MessageReactionProjection* projected) noexcept {
    if (!projected)
        return false;
    for (u32 index = 0; index < controller.count; ++index)
        if (emoji_equal(controller.entries[index].target, target)) {
            *projected = project_intent(controller, controller.entries[index]);
            return true;
        }
    return false;
}

bool message_reaction_controller_next_due(const MessageReactionController& controller,
                                          u64* due_at_ms) noexcept {
    if (!due_at_ms || !controller.count)
        return false;
    u64 result = ~u64{0};
    for (u32 index = 0; index < controller.count; ++index) {
        const auto& intent = controller.entries[index];
        u64 value = intent.deadline_at_ms;
        if (!controller.flight.active && !intent.awaiting_echo && send_due(controller, intent) < value)
            value = send_due(controller, intent);
        if (value < result)
            result = value;
    }
    *due_at_ms = result;
    return true;
}

bool message_reaction_controller_peek_due(const MessageReactionController& controller, u64 now_ms,
                                          MessageReactionIntent* output) noexcept {
    if (!output || controller.flight.active)
        return false;
    u32 selected = kMissingIndex;
    u64 earliest = ~u64{0};
    for (u32 index = 0; index < controller.count; ++index) {
        const auto& intent = controller.entries[index];
        const u64 due_at_ms = send_due(controller, intent);
        if (intent.awaiting_echo || now_ms >= intent.deadline_at_ms || due_at_ms > now_ms)
            continue;
        if (selected == kMissingIndex || due_at_ms < earliest ||
            (due_at_ms == earliest && intent.serial < controller.entries[selected].serial)) {
            selected = index;
            earliest = due_at_ms;
        }
    }
    if (selected == kMissingIndex)
        return false;
    *output = controller.entries[selected];
    return true;
}

bool message_reaction_controller_admit(MessageReactionController* controller,
                                       const MessageReactionIntent& intent, u64 request_id, u64 now_ms,
                                       MessageReactionTicket* ticket) noexcept {
    if (!controller || !request_id || !ticket || controller->flight.active ||
        controller->generation == ~u64{0})
        return false;
    const u32 index = find_intent(*controller, intent.target);
    if (index == kMissingIndex || controller->entries[index].awaiting_echo ||
        controller->entries[index].serial != intent.serial ||
        !source_equal(controller->entries[index].source, intent.source) ||
        controller->entries[index].desired_mine != intent.desired_mine ||
        controller->entries[index].mine_revision != intent.mine_revision ||
        now_ms >= controller->entries[index].deadline_at_ms ||
        send_due(*controller, controller->entries[index]) > now_ms)
        return false;
    reset_value(&controller->flight);
    controller->flight.intent = controller->entries[index];
    controller->flight.actor = controller->actor;
    controller->flight.ticket = {request_id, ++controller->generation, intent.serial};
    controller->flight.active = true;
    *ticket = controller->flight.ticket;
    ++controller->revision;
    return true;
}

namespace {
// The flight stays in the controller while it is read here; the caller clears
// it afterwards. Nothing below consults controller.flight.
void complete_flight(MessageReactionOutcome* out, MessageReactionController* controller,
                     const MessageReactionFlight& flight, MessageReactionCompletion completion, u64 now_ms,
                     u64 retry_at_ms, MessageReactionRetryScope scope) noexcept {
    if (!actor_equal(controller->actor, flight.actor)) {
        make_outcome(out, *controller, flight.intent, MessageReactionOutcomeKind::retired);
        out->project = false;
        return;
    }
    if (completion == MessageReactionCompletion::retry) {
        const u64 default_retry =
            add_time(now_ms, u64{250} << (flight.intent.retries < 3 ? flight.intent.retries : 3));
        if (retry_at_ms < default_retry)
            retry_at_ms = default_retry;
        if (scope == MessageReactionRetryScope::global) {
            if (retry_at_ms > controller->global_cooldown_ms)
                controller->global_cooldown_ms = retry_at_ms;
        } else if (scope == MessageReactionRetryScope::channel)
            retain_channel_cooldown(*controller, flight.intent.target.channel_id, retry_at_ms, now_ms);
    }
    const u32 index = find_intent(*controller, flight.intent.target);
    if (flight.retired || index == kMissingIndex ||
        !source_equal(controller->entries[index].source, flight.intent.source)) {
        make_outcome(out, *controller, flight.intent, MessageReactionOutcomeKind::retired);
        out->project = false;
        return;
    }
    auto& intent = controller->entries[index];
    if (now_ms >= intent.deadline_at_ms) {
        const bool achieved = mine(intent.authority, intent.target.type) == intent.desired_mine;
        return erase_with_outcome(out, *controller, index,
                                  achieved ? MessageReactionOutcomeKind::settled
                                           : MessageReactionOutcomeKind::expired);
    }
    bool authority_conflict = false;
    if (completion == MessageReactionCompletion::success) {
        if (intent.mine_revision == flight.intent.mine_revision) {
            auto authority = intent.authority;
            set_mine(authority, intent.target.type, flight.intent.desired_mine);
            shared_authority(*controller, intent, authority, ++controller->mine_revision);
        } else
            authority_conflict = mine(intent.authority, intent.target.type) != flight.intent.desired_mine;
        intent.retries = 0;
        if (mine(intent.authority, intent.target.type) == intent.desired_mine) {
            if (flight.intent.desired_mine != intent.desired_mine) {
                intent.due_at_ms = now_ms;
                make_outcome(out, *controller, intent, MessageReactionOutcomeKind::retained);
                out->authority_conflict = authority_conflict;
                return;
            }
            intent.awaiting_echo = !intent.echo_matches;
            if (intent.awaiting_echo)
                make_outcome(out, *controller, intent, MessageReactionOutcomeKind::settled);
            else
                erase_with_outcome(out, *controller, index, MessageReactionOutcomeKind::settled);
            out->authority_conflict = authority_conflict;
            return;
        }
        intent.due_at_ms = now_ms;
        make_outcome(out, *controller, intent, MessageReactionOutcomeKind::retained);
        out->authority_conflict = authority_conflict;
        return;
    }
    if (intent.echo_matches && mine(intent.authority, intent.target.type) == intent.desired_mine)
        return erase_with_outcome(out, *controller, index, MessageReactionOutcomeKind::settled);
    if (completion == MessageReactionCompletion::retry && intent.retries < kMessageReactionMaximumRetries) {
        ++intent.retries;
        if (retry_at_ms > intent.due_at_ms)
            intent.due_at_ms = retry_at_ms;
        return make_outcome(out, *controller, intent, MessageReactionOutcomeKind::retry);
    }
    if (intent.serial != flight.intent.serial) {
        intent.due_at_ms = now_ms;
        return make_outcome(out, *controller, intent, MessageReactionOutcomeKind::retained);
    }
    erase_with_outcome(out, *controller, index, MessageReactionOutcomeKind::failed);
}
} // namespace

void message_reaction_controller_complete(MessageReactionOutcome* out, MessageReactionController* controller,
                                          MessageReactionTicket ticket,
                                                            MessageReactionCompletion completion,
                                                            u64 now_ms, u64 retry_at_ms,
                                                            MessageReactionRetryScope scope) noexcept {
    if ((completion != MessageReactionCompletion::success &&
         completion != MessageReactionCompletion::retry &&
         completion != MessageReactionCompletion::terminal) ||
        (scope != MessageReactionRetryScope::intent && scope != MessageReactionRetryScope::channel &&
         scope != MessageReactionRetryScope::global)) {
        reset_value(out);
        out->error = MessageReactionError::invalid_input;
        return;
    }
    if (!controller || !controller->flight.active || !ticket.request_id ||
        ticket.request_id != controller->flight.ticket.request_id ||
        ticket.generation != controller->flight.ticket.generation ||
        ticket.serial != controller->flight.ticket.serial)
        return reset_value(out);
    ++controller->revision;
    complete_flight(out, controller, controller->flight, completion, now_ms, retry_at_ms, scope);
    reset_value(&controller->flight);
}

bool message_reaction_controller_observe(MessageReactionController* controller,
                                         const MessageReactionTarget& target, bool add,
                                         bool self) noexcept {
    if (!controller || !target_valid(target, false))
        return false;
    u32 index = kMissingIndex;
    for (u32 candidate = 0; candidate < controller->count; ++candidate)
        if (emoji_equal(controller->entries[candidate].target, target)) {
            index = candidate;
            break;
        }
    if (index == kMissingIndex)
        return false;
    const u32 owned_index = find_intent(*controller, target);
    const bool vote = target.type == MessageReactionType::vote;
    if (self && !vote && owned_index != kMissingIndex && controller->entries[owned_index].awaiting_echo &&
        add != controller->entries[owned_index].desired_mine) {
        // A delayed contrary echo from an older request cannot erase the
        // latest HTTP-confirmed choice. Foreign count events remain admitted.
        ++controller->revision;
        return true;
    }
    bool release_echo = false;
    if (self && !vote && owned_index != kMissingIndex) {
        auto& owned = controller->entries[owned_index];
        owned.echo_matches = add == owned.desired_mine;
        release_echo = owned.awaiting_echo && owned.echo_matches && !flight_matches(*controller, owned);
    }
    auto intent = controller->entries[index];
    intent.target.type = target.type;
    auto authority = intent.authority;
    if (vote) {
        if (add) {
            if (authority.total != ~u32{0})
                ++authority.total;
        } else if (!self && u64{authority.total} > u64{authority.normal} + authority.burst) {
            --authority.total;
        }
        authority.present = authority.total != 0;
    } else if (self)
        set_mine(authority, target.type, add);
    else {
        auto& count = target.type == MessageReactionType::burst ? authority.burst : authority.normal;
        if (add) {
            if (count != ~u32{0})
                ++count;
            if (authority.total != ~u32{0})
                ++authority.total;
        } else {
            const u32 own = mine(authority, target.type) ? 1u : 0u;
            if (count > own) {
                --count;
                if (authority.total)
                    --authority.total;
            }
        }
        authority.present = authority.total != 0;
    }
    shared_authority(*controller, intent, authority,
                     self && !vote ? ++controller->mine_revision : intent.mine_revision, self && !vote);
    if (release_echo)
        erase_intent(*controller, owned_index);
    ++controller->revision;
    return true;
}

bool message_reaction_controller_observe(MessageReactionController* controller,
                                         const MessageReactionTarget& target, bool add, bool current_user,
                                         u64 now_ms) noexcept {
    if (!controller || !target_valid(target, false))
        return false;
    bool released = false;
    for (u32 index = 0; index < controller->count;) {
        const auto& intent = controller->entries[index];
        if (emoji_equal(intent.target, target) && intent.awaiting_echo && now_ms >= intent.deadline_at_ms) {
            erase_intent(*controller, index);
            released = true;
        } else {
            ++index;
        }
    }
    if (released)
        ++controller->revision;
    const bool observed = message_reaction_controller_observe(controller, target, add, current_user);
    return released || observed;
}

bool message_reaction_controller_snapshot(MessageReactionController* controller,
                                          const MessageReactionTarget& target,
                                          MessageReactionAuthority authority) noexcept {
    if (!controller || !authority_valid(authority))
        return false;
    for (u32 index = 0; index < controller->count; ++index)
        if (emoji_equal(controller->entries[index].target, target)) {
            const MessageReactionSource source = controller->entries[index].source;
            const u64 revision = ++controller->mine_revision;
            for (u32 candidate = 0; candidate < controller->count;)
                if (emoji_equal(controller->entries[candidate].target, target) &&
                    source_equal(controller->entries[candidate].source, source)) {
                    controller->entries[candidate].authority = authority;
                    controller->entries[candidate].mine_revision = revision;
                    controller->entries[candidate].echo_matches =
                        mine(authority, controller->entries[candidate].target.type) ==
                        controller->entries[candidate].desired_mine;
                    if (controller->entries[candidate].awaiting_echo)
                        erase_intent(*controller, candidate);
                    else
                        ++candidate;
                } else
                    ++candidate;
            ++controller->revision;
            return true;
        }
    return false;
}

bool message_reaction_controller_rebind_source(MessageReactionController* controller,
                                               const MessageReactionTarget& target,
                                               MessageReactionSource source) noexcept {
    if (!controller || !source_valid(source))
        return false;
    bool changed = false;
    for (u32 index = 0; index < controller->count; ++index)
        if (emoji_equal(controller->entries[index].target, target)) {
            if (flight_matches(*controller, controller->entries[index]))
                controller->flight.intent.source = source;
            controller->entries[index].source = source;
            changed = true;
        }
    if (changed)
        ++controller->revision;
    return changed;
}

bool message_reaction_controller_cancel(MessageReactionController* controller, u64 channel_id,
                                        u64 message_id, const MessageReactionTarget* emoji) noexcept {
    if (!controller)
        return false;
    bool changed = false;
    const auto matches = [channel_id, message_id, emoji](const MessageReactionTarget& target) noexcept {
        return (!channel_id || target.channel_id == channel_id) &&
               (!message_id || target.message_id == message_id) && (!emoji || emoji_equal(target, *emoji));
    };
    for (u32 index = 0; index < controller->count;)
        if (matches(controller->entries[index].target)) {
            erase_intent(*controller, index);
            changed = true;
        } else
            ++index;
    if (controller->flight.active && matches(controller->flight.intent.target)) {
        controller->flight.retired = true;
        changed = true;
    }
    if (changed)
        ++controller->revision;
    return changed;
}

bool message_reaction_controller_expire(MessageReactionController* controller, u64 now_ms,
                                        MessageReactionOutcome* output) noexcept {
    if (!controller || !output)
        return false;
    for (u32 index = 0; index < controller->count; ++index)
        if (now_ms >= controller->entries[index].deadline_at_ms) {
            const MessageReactionIntent& expired = controller->entries[index];
            if (flight_matches(*controller, expired))
                controller->flight.retired = true;
            const bool achieved = mine(expired.authority, expired.target.type) == expired.desired_mine;
            const bool settled = expired.awaiting_echo || achieved;
            erase_with_outcome(output, *controller, index,
                               settled ? MessageReactionOutcomeKind::settled
                                       : MessageReactionOutcomeKind::expired);
            ++controller->revision;
            return true;
        }
    return false;
}

void message_reaction_controller_reset(MessageReactionController* controller,
                                       MessageReactionActor actor) noexcept {
    if (!controller)
        return;
    const bool different = !actor_equal(controller->actor, actor);
    if (controller->flight.active)
        controller->flight.retired = true;
    for (u32 index = 0; index < controller->count; ++index)
        reset_value(&controller->entries[index]);
    controller->count = 0;
    controller->actor = actor;
    if (different) {
        for (auto& entry : controller->cooldowns)
            reset_value(&entry);
        controller->global_cooldown_ms = 0;
    }
    ++controller->revision;
}
MessageReactionOutcome message_reaction_controller_queue(MessageReactionController* controller,
                                                         const MessageReactionTarget& target,
                                                         MessageReactionSource source,
                                                         MessageReactionAuthority authority, bool desired,
                                                         u64 now_ms) noexcept {
    MessageReactionOutcome result;
    message_reaction_controller_queue(&result, controller, target, source, authority, desired, now_ms);
    return result;
}

MessageReactionOutcome message_reaction_controller_complete(MessageReactionController* controller,
                                                            MessageReactionTicket ticket,
                                                            MessageReactionCompletion completion,
                                                            u64 now_ms, u64 retry_at_ms,
                                                            MessageReactionRetryScope scope) noexcept {
    MessageReactionOutcome result;
    message_reaction_controller_complete(&result, controller, ticket, completion, now_ms, retry_at_ms, scope);
    return result;
}

} // namespace maicord
