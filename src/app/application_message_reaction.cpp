#include "application_message_reaction.hpp"
#include "application_account_presentation.hpp"
#include "application_message_mutation_notice.hpp"
#include "application_state.hpp"
#include "application_emoji_preferences.hpp"
#include "application_media_picker.hpp"
#include "maicord/app/message_reaction_controller.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/reaction_preferences.hpp"
#include "maicord/ui/emoji_catalog.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/app/account_message_search.hpp"
#include "maicord/model/permission_bits.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/protocol/gateway_reaction_events.hpp"

namespace maicord::app_detail {
namespace {
struct ReactionBinding {
    u64 channel{}, message{}, epoch{};
};
struct ReactionStatus {
    u64 channel{}, message{}, serial{};
    u16 generation{};
    Utf8View text{};
    bool error{};
};
struct ReactionOwner {
    MessageReactionController controller;
    StableStorage storage;
    ReactionBinding bindings[kMessageReactionMaximumIntents];
    ReactionStatus status;
    u64 resident_channel{}, resident_epoch{1}, admission_retry_at{};
    bool cancel_requested{}, closing{};
};
static_assert(sizeof(ReactionOwner) <= 4096);
ReactionOwner* owner{};
bool refreshing{};
MessageReactionActor actor() noexcept {
    if (!g_app.account_ui || !g_app.account.instance || g_app.account.stop_failed ||
        !g_app.account.status.running || g_app.login.visible || !g_app.plugin_account_scope_serial ||
        !g_app.account.status.profile_id || !g_app.account_ui->chat_state.self_user_id ||
        !g_app.account.api.message_reaction)
        return {};
    return {g_app.plugin_account_scope_serial, g_app.account.status.profile_id,
            g_app.account_ui->chat_state.self_user_id, reinterpret_cast<usize>(g_app.account.instance)};
}
bool same_actor(MessageReactionActor a, MessageReactionActor b) noexcept {
    return a.instance && a.instance == b.instance && a.account_scope == b.account_scope &&
           a.profile_id == b.profile_id && a.self_user_id == b.self_user_id;
}
bool same_handle(MessageHandle a, MessageHandle b) noexcept {
    return a.slot == b.slot && a.generation == b.generation;
}
Utf8View name(const MessageReactionTarget& target) noexcept {
    return {target.name, target.name_bytes};
}
bool same_emoji(u64 id, const Utf8View& value, const MessageReactionTarget& target) noexcept {
    return id == target.emoji_id && (id || utf8_equal(value, name(target)));
}
const MessageRecord* resident(const MessageReactionTarget& target,
                              MessageHandle* handle = nullptr) noexcept {
    if (!g_app.account_ui || g_app.account_ui->active_channel_id != target.channel_id)
        return nullptr;
    const auto found = message_store_find(g_app.message_view.store, target.message_id);
    const auto* value =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    if (!value || value->channel_id != target.channel_id ||
        value->delivery != MessageDeliveryState::authoritative)
        return nullptr;
    if (handle)
        *handle = found.handle;
    return value;
}
ReactionBinding* binding(u64 channel, u64 message, bool create = false) noexcept {
    if (!owner)
        return nullptr;
    ReactionBinding* empty = nullptr;
    for (auto& value : owner->bindings) {
        if (value.channel == channel && value.message == message)
            return &value;
        if (!value.message && !empty)
            empty = &value;
    }
    if (create && empty) {
        *empty = {channel, message, owner->resident_epoch};
        return empty;
    }
    return nullptr;
}
bool readable(const ChannelRecord& channel) noexcept {
    constexpr u32 required = channel_action_view | channel_action_read_history;
    if (!channel.occupied || !channel_is_message_capable(channel.type) ||
        (channel.flags & kChannelObfuscatedFlag) || (channel.actions & required) != required ||
        !chat_state_store_channel_visible(g_app.account_ui->chat_state, channel))
        return false;
    if (!channel.guild_id)
        return channel_is_private(channel.type);
    const auto* guild = chat_state_store_find_guild(g_app.account_ui->chat_state, channel.guild_id);
    return guild && guild->occupied && !guild->unavailable && channel.permissions_resolved;
}
bool member_reaction_add_restricted(const ChannelRecord& channel) noexcept {
    const auto& state = g_app.account_ui->chat_state;
    const auto* guild = chat_state_store_find_guild(state, channel.guild_id);
    if (!guild || guild->owner_id == state.self_user_id)
        return false;
    const auto& settings = guild->settings;
    const bool timed_out = (settings.known & guild_settings_self_timeout) &&
                           settings.self_communication_disabled_until > clock_unix_milliseconds() / 1000;
    if (!timed_out && !guild_self_quarantined(settings))
        return false;
    // Use assigned role permissions: the two-factor gate can strip the
    // effective Administrator bit without removing this exemption.
    for (u32 i = 0; i < state.role_count; ++i) {
        const auto& role = state.roles[i];
        if (role.guild_id == guild->id && (role.id == guild->id || role.self_assigned) &&
            (role.permissions & kPermissionAdministrator))
            return false;
    }
    return true;
}
bool eligible(const MessageReactionTarget& target, const MessageReactionAuthority& authority,
              bool desired) noexcept {
    const auto* channel =
        chat_state_store_find_channel(g_app.account_ui->chat_state, target.channel_id);
    if (!channel || !readable(*channel))
        return false;
    // Existing emoji and own-reaction removals bypass Add Reactions, but
    // neither can change an archived thread or one with unknown archive state.
    if (channel_is_thread(channel->type) &&
        (!channel->thread_archived_known || channel->thread_archived))
        return false;
    if (!desired)
        return true;
    if (channel->guild_id && member_reaction_add_restricted(*channel))
        return false;
    if (!authority.present && !(channel->actions & channel_action_add_reactions))
        return false;
    if (target.emoji_id && !authority.present) {
        const auto* emoji =
            chat_state_store_find_emoji(g_app.account_ui->chat_state, target.emoji_id);
        if (!emoji || !chat_state_store_emoji_usable(g_app.account_ui->chat_state, *emoji))
            return false;
    }
    return true;
}
MessageReactionAuthority authority_from(const MessageReactionTarget& target) noexcept {
    MessageReactionAuthority value{};
    value.known = true;
    const auto* record = resident(target);
    const auto* rich =
        record ? message_rich_store_find(g_app.account_ui->message_rich_store, target.message_id) : nullptr;
    if (!rich || rich->message_revision != record->revision)
        return value;
    for (u32 i = 0; i < rich->reaction_count; ++i) {
        const auto& item = rich->reactions[i];
        if (same_emoji(item.emoji_id,
                       message_rich_store_string(g_app.account_ui->message_rich_store, item.name),
                       target)) {
            value = {item.count,
                     item.normal_count,
                     item.burst_count,
                     (item.flags & 1) != 0,
                     (item.flags & 2) != 0,
                     true,
                     true};
            return value;
        }
    }
    return value;
}
bool project(const MessageReactionTarget& target, const MessageReactionProjection& p,
             bool* model_changed = nullptr) noexcept {
    if (model_changed)
        *model_changed = false;
    const auto* record = resident(target);
    if (!record)
        return true;
    const auto& a = p.authority;
    if (a.total < a.normal || a.total - a.normal < a.burst)
        return false;
    const i64 normal =
        static_cast<i64>(a.normal) + static_cast<i64>(p.desired_normal) - static_cast<i64>(a.mine_normal);
    const i64 burst =
        static_cast<i64>(a.burst) + static_cast<i64>(p.desired_burst) - static_cast<i64>(a.mine_burst);
    const i64 total = normal + burst + a.total - a.normal - a.burst;
    if (normal < 0 || burst < 0 || total < 0 || normal > ~u32{0} || burst > ~u32{0} || total > ~u32{0})
        return false;
    auto& store = g_app.account_ui->message_rich_store;
    const auto* rich = message_rich_store_find(store, target.message_id);
    if (rich && rich->message_revision != record->revision)
        return false;
    GatewayMessageReaction values[kGatewayMessageReactionMaximum]{};
    char labels[kGatewayMessageReactionMaximum][kMessageReactionMaximumEmojiNameBytes]{};
    u32 count = 0;
    bool matched = false, changed = false;
    for (u32 i = 0; rich && i < rich->reaction_count; ++i) {
        const auto& item = rich->reactions[i];
        const auto text = message_rich_store_string(store, item.name);
        const bool match = same_emoji(item.emoji_id, text, target);
        matched |= match;
        if (match && !total) {
            changed = true;
            continue;
        }
        if (text.bytes > sizeof(labels[0]) || count == kGatewayMessageReactionMaximum)
            return false;
        copy_bytes(labels[count], text.data, text.bytes);
        values[count] = {item.emoji_id,
                         {labels[count], text.bytes},
                         item.count,
                         item.normal_count,
                         item.burst_count,
                         (item.flags & 1) != 0,
                         (item.flags & 2) != 0,
                         (item.flags & 4) != 0};
        if (match) {
            auto& v = values[count];
            changed |= v.count != static_cast<u32>(total) || v.normal_count != static_cast<u32>(normal) ||
                       v.burst_count != static_cast<u32>(burst) || v.me != p.desired_normal ||
                       v.me_burst != p.desired_burst;
            v.count = static_cast<u32>(total);
            v.normal_count = static_cast<u32>(normal);
            v.burst_count = static_cast<u32>(burst);
            v.me = p.desired_normal;
            v.me_burst = p.desired_burst;
        }
        ++count;
    }
    if (!matched && total) {
        if (count == kGatewayMessageReactionMaximum)
            return false;
        copy_bytes(labels[count], target.name, target.name_bytes);
        values[count] = {target.emoji_id,         {labels[count], target.name_bytes},
                         static_cast<u32>(total), static_cast<u32>(normal),
                         static_cast<u32>(burst), p.desired_normal,
                         p.desired_burst,         target.animated};
        ++count;
        changed = true;
    }
    if (!changed)
        return true;
    GatewayMessageEvent event{};
    event.id = target.message_id;
    event.fields = gateway_message_field_reactions;
    event.reactions = {values, count};
    const bool applied = message_rich_store_apply(&store, target.message_id, record->revision, event).ok();
    if (model_changed)
        *model_changed = applied;
    return applied;
}

bool restore_current_source(const MessageReactionIntent& intent, bool* model_changed = nullptr) noexcept {
    if (model_changed)
        *model_changed = false;
    if (!owner || !same_actor(actor(), owner->controller.actor))
        return false;
    const auto* channel =
        chat_state_store_find_channel(g_app.account_ui->chat_state, intent.target.channel_id);
    MessageHandle handle{};
    const auto* record = resident(intent.target, &handle);
    if (!channel || channel->generation != intent.source.channel_generation || !record ||
        !same_handle(handle, intent.source.handle) || record->revision != intent.source.revision)
        return false;
    return project(intent.target,
                   {intent.authority, intent.authority.mine_normal, intent.authority.mine_burst, false},
                   model_changed);
}
void release_owner() noexcept {
    if (!owner)
        return;
    stable_storage_release(&owner->storage);
    VirtualFree(owner, 0, MEM_RELEASE);
    owner = nullptr;
    reset_value(&g_app.message_reaction);
}
bool ensure_owner(MessageReactionActor current) noexcept {
    if (owner)
        return same_actor(owner->controller.actor, current) && !owner->closing;
    auto* next = static_cast<ReactionOwner*>(
        VirtualAlloc(nullptr, sizeof(ReactionOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!next)
        return false;
    if (!stable_storage_initialize(&next->storage, kMessageReactionMaximumIntents, 1,
                                   sizeof(MessageReactionIntent), alignof(MessageReactionIntent)) ||
        !message_reaction_controller_initialize(
            &next->controller, {stable_storage_data<MessageReactionIntent>(next->storage), 1}, current)) {
        stable_storage_release(&next->storage);
        VirtualFree(next, 0, MEM_RELEASE);
        return false;
    }
    next->resident_channel = g_app.account_ui->active_channel_id;
    next->resident_epoch = 1;
    owner = next;
    return true;
}
void feedback(const MessageReactionTarget& target, u64 serial, Utf8View text, bool error = false) noexcept {
    if (!owner)
        return;
    if (serial < owner->status.serial)
        return;
    const auto* channel =
        chat_state_store_find_channel(g_app.account_ui->chat_state, target.channel_id);
    owner->status = {
        target.channel_id, target.message_id, serial, channel ? channel->generation : u16{0}, text, error};
}
void publish(bool model_changed = true) noexcept {
    if (refreshing)
        return;
    refreshing = true;
    if (model_changed)
        refresh_account_text(true);
    application_media_picker_reaction_changed();
    if (g_app.window)
        application_request_frame();
    refreshing = false;
}
void cancel_flight() noexcept {
    if (!owner || !owner->controller.flight.active || owner->cancel_requested)
        return;
    if (reinterpret_cast<usize>(g_app.account.instance) == owner->controller.flight.actor.instance &&
        g_app.account.api.message_search) {
        g_app.account.api.message_search(g_app.account.instance, AccountMessageSearchOperation::cancel,
                                         nullptr, owner->controller.flight.ticket.request_id);
        owner->cancel_requested = true;
    }
}
void track(u64 emoji, Utf8View glyph) noexcept {
    const u32 index = emoji ? 0 : emoji_catalog_find_glyph(glyph);
    if (emoji || index <= 0xFFFEu)
        application_emoji_preferences_track_reaction(emoji ? reaction_emoji_custom(emoji)
                                                           : reaction_emoji_unicode(static_cast<u16>(index)));
}
void mirror() noexcept {
    reset_value(&g_app.message_reaction);
    if (!owner)
        return;
    MessageReactionIntent intent{};
    const bool active = owner->controller.flight.active;
    if (active)
        intent = owner->controller.flight.intent;
    else {
        bool found = false;
        for (u32 i = 0; i < owner->controller.count; ++i)
            if (!owner->controller.entries[i].awaiting_echo) {
                intent = owner->controller.entries[i];
                found = true;
                break;
            }
        if (!found)
            return;
    }
    const auto& t = intent.target;
    auto& out = g_app.message_reaction;
    out.channel_id = t.channel_id;
    out.message_id = t.message_id;
    out.emoji_id = t.emoji_id;
    out.name_bytes = t.name_bytes;
    copy_bytes(out.name, t.name, t.name_bytes);
    out.type = t.type;
    out.add = intent.desired_mine;
    out.animated = t.animated;
    out.retries = intent.retries;
    out.retry_at_ms = intent.due_at_ms;
    out.phase = active ? MessageReactionRequestPhase::submitting
                : intent.due_at_ms > clock_monotonic_milliseconds()
                    ? MessageReactionRequestPhase::retry_wait
                    : MessageReactionRequestPhase::queued;
}
ApplicationMessageReactionEnqueueResult queue(u64 id, u64 emoji, Utf8View text, bool animated,
                                          bool toggle) noexcept {
    const auto current = actor();
    if (!current.instance)
        return ApplicationMessageReactionEnqueueResult::stale;
    if (!id || !text.data || !text.bytes || text.bytes > kMessageReactionMaximumEmojiNameBytes ||
        !utf8_validate(text).valid)
        return ApplicationMessageReactionEnqueueResult::invalid;
    for (u32 i = 0; i < text.bytes; ++i)
        if (static_cast<u8>(text.data[i]) < 0x20 || text.data[i] == 0x7F)
            return ApplicationMessageReactionEnqueueResult::invalid;
    MessageReactionTarget target{};
    target.message_id = id;
    target.emoji_id = emoji;
    target.type = MessageReactionType::normal;
    target.animated = animated;
    target.name_bytes = static_cast<u16>(text.bytes);
    copy_bytes(target.name, text.data, text.bytes);
    target.channel_id = g_app.account_ui->active_channel_id;
    MessageHandle handle{};
    const auto* record = resident(target, &handle);
    if (!record)
        return ApplicationMessageReactionEnqueueResult::stale;
    if (!application_message_reaction_synchronize())
        return ApplicationMessageReactionEnqueueResult::stale;
    if (owner && owner->closing)
        return ApplicationMessageReactionEnqueueResult::stale;
    record = resident(target, &handle);
    if (!record)
        return ApplicationMessageReactionEnqueueResult::stale;
    MessageReactionProjection previous{};
    auto a = authority_from(target);
    const bool retained =
        owner && message_reaction_controller_project(owner->controller, target, &previous);
    if (retained)
        a = previous.authority;
    const bool mine = retained ? previous.desired_normal : a.mine_normal, desired = toggle ? !mine : true;
    if (!eligible(target, a, desired))
        return ApplicationMessageReactionEnqueueResult::permission;
    const auto* rich = message_rich_store_find(g_app.account_ui->message_rich_store, id);
    if (desired && !a.present && rich && rich->reaction_count == kGatewayMessageReactionMaximum)
        return ApplicationMessageReactionEnqueueResult::capacity;
    if (!toggle && mine && !retained)
        return ApplicationMessageReactionEnqueueResult::coalesced;
    if (!ensure_owner(current))
        return ApplicationMessageReactionEnqueueResult::storage;
    if (!binding(target.channel_id, target.message_id)) {
        bool free_binding = false;
        for (const auto& source : owner->bindings)
            free_binding |= source.message == 0;
        if (!free_binding)
            return ApplicationMessageReactionEnqueueResult::capacity;
    }
    u32 prior_index = owner->controller.count;
    for (u32 i = 0; i < owner->controller.count; ++i)
        if (message_reaction_target_equal(owner->controller.entries[i].target, target))
            prior_index = i;
    const bool exists = prior_index != owner->controller.count;
    if (!exists && owner->controller.count == owner->controller.entries.count &&
        owner->controller.count < kMessageReactionMaximumIntents) {
        const u32 needed = owner->controller.count + 1;
        if (!stable_storage_reserve(&owner->storage, needed) ||
            !message_reaction_controller_grow_storage(
                &owner->controller,
                {stable_storage_data<MessageReactionIntent>(owner->storage), owner->storage.capacity}))
            return ApplicationMessageReactionEnqueueResult::storage;
    }
    const auto* channel =
        chat_state_store_find_channel(g_app.account_ui->chat_state, target.channel_id);
    // A loaded source was synchronized above. Accepting this request must not
    // replace a different source while an older request still holds its result.
    if (exists) {
        const auto& source = owner->controller.entries[prior_index].source;
        if (!same_handle(source.handle, handle) || source.revision != record->revision ||
            source.channel_generation != channel->generation)
            return ApplicationMessageReactionEnqueueResult::stale;
    }
    const auto before = owner->controller;
    const auto prior = exists ? before.entries[prior_index] : MessageReactionIntent{};
    MessageReactionOutcome outcome;
    message_reaction_controller_queue(&outcome, &owner->controller, target,
                                      {handle, record->revision, channel->generation}, a, desired,
                                      clock_monotonic_milliseconds());
    if (!outcome.ok())
        return outcome.error == MessageReactionError::capacity
                   ? ApplicationMessageReactionEnqueueResult::capacity
                   : ApplicationMessageReactionEnqueueResult::stale;
    if (outcome.project && !project(outcome.target, outcome.projection)) {
        // Rich-store commits are transactional. Restore the one modified
        // logical entry (including an erased settled entry) before refusing.
        if (exists) {
            if (owner->controller.count < before.count) {
                for (u32 i = before.count - 1; i > prior_index; --i)
                    before.entries[i] = before.entries[i - 1];
            }
            before.entries[prior_index] = prior;
        }
        owner->controller = before;
        mirror();
        return ApplicationMessageReactionEnqueueResult::storage;
    }
    binding(target.channel_id, target.message_id, true);
    feedback(target, outcome.serial, Utf8View{});
    application_message_mutation_notice_begin(ApplicationMessageMutationKind::reaction, target.channel_id,
                                              target.message_id);
    if (desired && !mine)
        track(emoji, text);
    mirror();
    publish();
    return retained ? ApplicationMessageReactionEnqueueResult::coalesced
                    : ApplicationMessageReactionEnqueueResult::accepted;
}
} // namespace

ApplicationMessageReactionEnqueueResult application_queue_message_reaction_emoji(u64 message_id, u64 emoji_id,
                                                                             Utf8View label,
                                                                             bool animated) noexcept {
    return queue(message_id, emoji_id, label, animated, false);
}

ApplicationMessageReactionEnqueueResult application_queue_message_reaction(u64 message_id, u32 index) noexcept {
    if (!actor().instance)
        return ApplicationMessageReactionEnqueueResult::stale;
    const auto* record = application_message_record(message_id);
    const auto* rich = message_rich_store_find(g_app.account_ui->message_rich_store, message_id);
    if (!record || record->channel_id != g_app.account_ui->active_channel_id || !rich ||
        rich->message_revision != record->revision || index >= rich->reaction_count)
        return ApplicationMessageReactionEnqueueResult::stale;
    const auto item = rich->reactions[index];
    Utf8View label = message_rich_store_string(g_app.account_ui->message_rich_store, item.name);
    if (!label.bytes && item.emoji_id) {
        const auto* emoji = chat_state_store_find_emoji(g_app.account_ui->chat_state, item.emoji_id);
        if (emoji)
            label = chat_state_store_emoji_name(*emoji);
        if (!label.bytes && owner) {
            for (u32 i = 0; i < owner->controller.count; ++i) {
                const auto& target = owner->controller.entries[i].target;
                if (target.channel_id == record->channel_id && target.message_id == message_id &&
                    target.emoji_id == item.emoji_id) {
                    label = name(target);
                    break;
                }
            }
        }
    }
    char copied[kMessageReactionMaximumEmojiNameBytes]{};
    if (label.bytes > sizeof(copied))
        return ApplicationMessageReactionEnqueueResult::invalid;
    copy_bytes(copied, label.data, label.bytes);
    return queue(message_id, item.emoji_id, {copied, label.bytes}, (item.flags & 4) != 0, true);
}

Utf8View application_message_reaction_admission_text(ApplicationMessageReactionEnqueueResult result) noexcept {
    switch (result) {
    case ApplicationMessageReactionEnqueueResult::accepted:
    case ApplicationMessageReactionEnqueueResult::coalesced:
        return Utf8View{};
    case ApplicationMessageReactionEnqueueResult::capacity:
        return utf8_literal("Too many reactions at once. Try again in a moment.");
    case ApplicationMessageReactionEnqueueResult::permission:
        return utf8_literal("You cannot add this reaction here.");
    case ApplicationMessageReactionEnqueueResult::stale:
        return utf8_literal("This message is no longer available. Reopen the picker.");
    case ApplicationMessageReactionEnqueueResult::invalid:
        return utf8_literal("This emoji is unavailable.");
    case ApplicationMessageReactionEnqueueResult::storage:
        return utf8_literal("Couldn't add the reaction. Try again.");
    }
    return {};
}

Utf8View application_message_reaction_status(u64 message_id, bool* error) noexcept {
    if (error)
        *error = false;
    if (!owner || !same_actor(actor(), owner->controller.actor) || !g_app.account_ui)
        return {};
    const auto& status = owner->status;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, status.channel);
    if (status.message != message_id || status.channel != g_app.account_ui->active_channel_id || !channel ||
        channel->generation != status.generation || !readable(*channel))
        return {};
    MessageReactionTarget target{};
    target.channel_id = status.channel;
    target.message_id = status.message;
    if (!resident(target))
        return {};
    if (error)
        *error = status.error;
    return status.text;
}

void reset_message_reaction() noexcept {
    if (!owner) {
        reset_value(&g_app.message_reaction);
        return;
    }
    if (same_actor(actor(), owner->controller.actor)) {
        for (u32 i = 0; i < owner->controller.count; ++i)
            restore_current_source(owner->controller.entries[i]);
    }
    const auto& flight = owner->controller.flight;
    const bool draining =
        flight.active &&
        g_app.message_history_request_owner == MessageHistoryRequestOwner::message_reaction &&
        g_app.primary_request_id == flight.ticket.request_id &&
        reinterpret_cast<usize>(g_app.account.instance) == flight.actor.instance;
    if (!draining) {
        release_owner();
        return;
    }
    message_reaction_controller_reset(&owner->controller, {});
    owner->closing = true;
    reset_value(&owner->status);
    cancel_flight();
    mirror();
}

static bool synchronize_reactions(u64 snapshot_channel, u64 snapshot_message,
                                  const MessageReactionTarget* protected_emoji = nullptr,
                                  bool authoritative_replacement = true) noexcept {
    if (!owner)
        return true;
    const auto current = actor();
    if (owner->closing || !same_actor(current, owner->controller.actor)) {
        if (!owner->closing) {
            message_reaction_controller_reset(&owner->controller, {});
            owner->closing = true;
            reset_value(&owner->status);
            cancel_flight();
            publish(false);
        }
        if (!owner->controller.flight.active)
            release_owner();
        return true;
    }
    auto& controller = owner->controller;
    const auto protected_target = [&](const MessageReactionTarget& target) noexcept {
        return target.channel_id == snapshot_channel && target.message_id == snapshot_message &&
               (!protected_emoji || same_emoji(target.emoji_id, name(target), *protected_emoji));
    };
    const auto revision = controller.revision;
    if (owner->resident_channel != g_app.account_ui->active_channel_id) {
        owner->resident_channel = g_app.account_ui->active_channel_id;
        ++owner->resident_epoch;
    }
    bool changed = false;
    for (u32 i = 0; i < controller.count;) {
        const auto intent = controller.entries[i];
        const auto* channel =
            chat_state_store_find_channel(g_app.account_ui->chat_state, intent.target.channel_id);
        const bool current_channel = channel && channel->generation == intent.source.channel_generation;
        const bool allowed =
            current_channel && eligible(intent.target, intent.authority, intent.desired_mine);
        bool valid = allowed;
        MessageHandle handle{};
        const auto* record = resident(intent.target, &handle);
        auto* source = binding(intent.target.channel_id, intent.target.message_id);
        if (valid && intent.target.channel_id == owner->resident_channel) {
            const bool same_epoch = source && source->epoch == owner->resident_epoch;
            if (same_epoch && (!record || !same_handle(handle, intent.source.handle)))
                valid = false;
            else if (record) {
                const bool reproject = !same_epoch || !same_handle(handle, intent.source.handle) ||
                                       record->revision != intent.source.revision;
                if (!same_handle(handle, intent.source.handle) ||
                    record->revision != intent.source.revision) {
                    message_reaction_controller_rebind_source(
                        &controller, intent.target, {handle, record->revision, channel->generation});
                }
                if (reproject && !protected_target(intent.target)) {
                    MessageReactionProjection projection{};
                    bool model_changed = false;
                    if (message_reaction_controller_project(controller, intent.target, &projection))
                        project(intent.target, projection, &model_changed);
                    changed |= model_changed;
                }
            }
        }
        if (!valid) {
            const bool permission_lost = current_channel && !allowed;
            bool restored = false;
            if (!protected_target(intent.target) || !authoritative_replacement)
                restored = restore_current_source(intent);
            message_reaction_controller_cancel(&controller, intent.target.channel_id,
                                               intent.target.message_id, &intent.target);
            if (owner->status.serial <= intent.serial) {
                feedback(intent.target, intent.serial,
                         application_message_reaction_admission_text(
                             permission_lost ? ApplicationMessageReactionEnqueueResult::permission
                                             : ApplicationMessageReactionEnqueueResult::stale),
                         true);
                if (permission_lost && restored) {
                    RestClientResponse refusal{};
                    refusal.status_code = 403;
                    application_message_mutation_notice_failed(ApplicationMessageMutationKind::reaction,
                                                               intent.target.channel_id,
                                                               intent.target.message_id, refusal);
                }
            }
            changed = true;
            continue;
        }
        ++i;
    }
    MessageReactionOutcome expired{};
    while (message_reaction_controller_expire(&controller, clock_monotonic_milliseconds(), &expired)) {
        if (expired.project && (!protected_target(expired.target) || !authoritative_replacement))
            project(expired.target, expired.projection);
        if (expired.kind == MessageReactionOutcomeKind::expired)
            feedback(expired.target, expired.serial,
                     utf8_literal("Reaction failed. Try again."), true);
        else if (expired.kind == MessageReactionOutcomeKind::settled &&
                 owner->status.serial <= expired.serial) {
            feedback(expired.target, expired.serial, {});
            application_message_mutation_notice_succeeded(ApplicationMessageMutationKind::reaction,
                                                          expired.target.channel_id,
                                                          expired.target.message_id);
        }
        changed = true;
    }
    if (controller.flight.retired)
        cancel_flight();
    for (auto& source : owner->bindings) {
        if (!source.message)
            continue;
        bool used = controller.flight.active && !controller.flight.retired &&
                    controller.flight.intent.target.channel_id == source.channel &&
                    controller.flight.intent.target.message_id == source.message;
        for (u32 i = 0; i < controller.count && !used; ++i)
            used = controller.entries[i].target.channel_id == source.channel &&
                   controller.entries[i].target.message_id == source.message;
        if (!used)
            reset_value(&source);
        else if (source.channel == owner->resident_channel) {
            MessageReactionTarget target{};
            target.channel_id = source.channel;
            target.message_id = source.message;
            if (resident(target))
                source.epoch = owner->resident_epoch;
        }
    }
    mirror();
    if (changed || revision != controller.revision)
        publish(changed);
    return true;
}

bool application_message_reaction_synchronize() noexcept {
    return synchronize_reactions(0, 0);
}

bool application_message_reaction_pending(u64 now_ms) noexcept {
    if (!application_message_reaction_synchronize() || !owner || owner->closing)
        return false;
    if (now_ms < owner->admission_retry_at)
        return false;
    MessageReactionIntent intent{};
    return message_reaction_controller_peek_due(owner->controller, now_ms, &intent);
}

bool application_message_reaction_submit(u64 request_id, u64 now_ms) noexcept {
    if (!request_id || !application_message_reaction_synchronize() || !owner || owner->closing)
        return false;
    if (now_ms < owner->admission_retry_at)
        return false;
    MessageReactionIntent intent{};
    if (!message_reaction_controller_peek_due(owner->controller, now_ms, &intent))
        return false;
    const auto& target = intent.target;
    const auto result = g_app.account.api.message_reaction(g_app.account.instance, target.channel_id,
                                                           target.message_id, target.emoji_id, name(target),
                                                           target.type, intent.desired_mine, request_id);
    if (!result.ok()) {
        if (result.error == RestClientSubmitError::busy) {
            owner->admission_retry_at = now_ms + 50;
            return false;
        }
        MessageReactionProjection baseline{intent.authority, intent.authority.mine_normal,
                                           intent.authority.mine_burst, false};
        message_reaction_controller_cancel(&owner->controller, target.channel_id, target.message_id,
                                           &target);
        project(target, baseline);
        feedback(target, intent.serial,
                 utf8_literal("Couldn't add the reaction. Try again."), true);
        mirror();
        publish();
        return false;
    }
    MessageReactionTicket ticket{};
    owner->admission_retry_at = 0;
    if (!message_reaction_controller_admit(&owner->controller, intent, request_id, now_ms, &ticket)) {
        // The UI thread cannot change the request between peek and the API call.
        // Keep the transport's request handle intact even if an adapter violates that rule.
        owner->controller.flight = {intent,
                                    owner->controller.actor,
                                    {request_id, owner->controller.generation, intent.serial},
                                    true,
                                    true};
        cancel_flight();
    }
    owner->cancel_requested = false;
    feedback(target, intent.serial, {});
    mirror();
    publish(false);
    return true;
}

bool application_message_reaction_response(const RestClientResponse& response) noexcept {
    if (!owner || !owner->controller.flight.active ||
        owner->controller.flight.ticket.request_id != response.request_id)
        return false;
    application_message_reaction_synchronize();
    if (!owner)
        return false;
    const auto flight = owner->controller.flight;
    const u64 now = clock_monotonic_milliseconds();
    const bool success =
        response.transport_ok() && response.status_code >= 200 && response.status_code < 300;
    const bool rate = response.transport_ok() && response.status_code == 429 && response.rate_limited &&
                      response.retry_at_ms && !api_rejection_requires_action(response.rejection);
    const bool server = response.transport_ok() && response.status_code >= 500 &&
                        response.status_code < 600 &&
                        !api_rejection_requires_action(response.rejection);
    const auto completion = success          ? MessageReactionCompletion::success
                            : rate || server ? MessageReactionCompletion::retry
                                             : MessageReactionCompletion::terminal;
    const u64 retry = rate ? response.retry_at_ms : now + 500 * (flight.intent.retries + 1);
    MessageReactionOutcome outcome;
    message_reaction_controller_complete(
        &outcome, &owner->controller, flight.ticket, completion, now, retry,
        rate ? response.global_rate_limit ? MessageReactionRetryScope::global
                                          : MessageReactionRetryScope::channel
             : MessageReactionRetryScope::intent);
    owner->cancel_requested = false;
    if (owner->closing) {
        release_owner();
        return true;
    }
    if (outcome.project)
        project(outcome.target, outcome.projection);
    const bool latest_feedback = owner->status.serial <= outcome.serial;
    if (outcome.kind == MessageReactionOutcomeKind::failed ||
        outcome.kind == MessageReactionOutcomeKind::expired) {
        if (latest_feedback) {
            feedback(outcome.target, outcome.serial,
                     utf8_literal("Reaction failed. Try again."), true);
            application_message_mutation_notice_failed(ApplicationMessageMutationKind::reaction,
                                                       outcome.target.channel_id, outcome.target.message_id,
                                                       response);
        }
    } else if (outcome.kind == MessageReactionOutcomeKind::retry) {
        if (latest_feedback)
            feedback(outcome.target, outcome.serial, utf8_literal("Retrying reaction…"));
    } else if (outcome.kind == MessageReactionOutcomeKind::settled && latest_feedback) {
        feedback(outcome.target, outcome.serial, {});
        application_message_mutation_notice_succeeded(ApplicationMessageMutationKind::reaction,
                                                      outcome.target.channel_id, outcome.target.message_id);
    }
    mirror();
    publish(outcome.project);
    return true;
}

bool application_message_reaction_observation_needed() noexcept {
    return owner && (owner->controller.count || owner->controller.flight.active);
}

ApplicationMessageReactionMetrics application_message_reaction_metrics() noexcept {
    ApplicationMessageReactionMetrics result{};
    if (!owner)
        return result;
    Win32MemoryInformation memory{};
    VirtualQuery(owner, memory.native(), sizeof(memory));
    result.owner_bytes = sizeof(ReactionOwner);
    result.owner_committed_bytes = memory.State == MEM_COMMIT ? static_cast<u32>(memory.RegionSize) : 0;
    result.intent_bytes = kMessageReactionMaximumIntents * sizeof(MessageReactionIntent);
    result.intent_committed_bytes = static_cast<u32>(owner->storage.arena.committed);
    result.intent_count = owner->controller.count;
    result.backed_slots = owner->controller.entries.count;
    result.revision = owner->controller.revision;
    result.active = true;
    result.flight_active = owner->controller.flight.active;
    return result;
}

u64 application_message_reaction_next_wake() noexcept {
    if (!owner || owner->closing)
        return 0;
    u64 wake{};
    const u64 now = clock_monotonic_milliseconds();
    if (g_app.message_history_request_owner == MessageHistoryRequestOwner::none &&
        !owner->controller.flight.active) {
        message_reaction_controller_next_due(owner->controller, &wake);
        if (wake && wake < owner->admission_retry_at)
            wake = owner->admission_retry_at;
        if (wake && wake <= now)
            wake = 1;
    }
    for (u32 i = 0; i < owner->controller.count; ++i) {
        const auto deadline = owner->controller.entries[i].deadline_at_ms;
        if (!wake || deadline < wake)
            wake = deadline;
    }
    return wake;
}

static bool observed_source_current(const MessageReactionIntent& intent, MessageHandle observed_handle,
                                    u32 observed_revision, bool applied) noexcept {
    const auto* channel =
        chat_state_store_find_channel(g_app.account_ui->chat_state, intent.target.channel_id);
    if (!channel || channel->generation != intent.source.channel_generation)
        return false;
    MessageHandle current_handle{};
    const auto* record = resident(intent.target, &current_handle);
    const auto* source = binding(intent.target.channel_id, intent.target.message_id);
    const bool navigation_rebind = owner->resident_channel != g_app.account_ui->active_channel_id ||
                                   (source && source->epoch != owner->resident_epoch);
    if (!record)
        return intent.target.channel_id != g_app.account_ui->active_channel_id || navigation_rebind;
    if (!applied || !same_handle(observed_handle, current_handle) ||
        record->revision != observed_revision ||
        (!navigation_rebind && !same_handle(intent.source.handle, current_handle)))
        return false;
    if (!same_handle(intent.source.handle, current_handle) || intent.source.revision != record->revision)
        message_reaction_controller_rebind_source(&owner->controller, intent.target,
                                                  {current_handle, record->revision, channel->generation});
    return true;
}

void application_message_reaction_observe(const GatewayMessageReactionEvent& event,
                                          MessageHandle observed_handle, u32 observed_revision,
                                          bool applied) noexcept {
    if (!owner)
        return;
    if (owner->closing || !same_actor(actor(), owner->controller.actor)) {
        application_message_reaction_synchronize();
        return;
    }
    MessageReactionTarget target{};
    target.channel_id = event.channel_id;
    target.message_id = event.message_id;
    target.emoji_id = event.emoji_id;
    target.type = event.type;
    if (event.name.bytes > sizeof(target.name))
        return;
    target.name_bytes = static_cast<u16>(event.name.bytes);
    copy_bytes(target.name, event.name.data, event.name.bytes);
    if (event.kind == GatewayMessageReactionEventKind::remove_all ||
        event.kind == GatewayMessageReactionEventKind::remove_emoji) {
        // Only the replaced aggregates are protected from baseline restoration.
        const auto* removed = event.kind == GatewayMessageReactionEventKind::remove_all ? nullptr : &target;
        synchronize_reactions(target.channel_id, target.message_id, removed);
        if (!owner || owner->closing)
            return;
        const bool changed = message_reaction_controller_cancel(&owner->controller, target.channel_id,
                                                                target.message_id, removed);
        if (owner->controller.flight.retired)
            cancel_flight();
        mirror();
        if (changed)
            publish();
        return;
    }
    for (u32 i = 0; i < owner->controller.count; ++i) {
        const auto intent = owner->controller.entries[i];
        if (intent.target.channel_id != target.channel_id ||
            intent.target.message_id != target.message_id ||
            !same_emoji(intent.target.emoji_id, name(intent.target), target))
            continue;
        if (!observed_source_current(intent, observed_handle, observed_revision, applied)) {
            synchronize_reactions(target.channel_id, target.message_id, &target, false);
            return;
        }
    }
    // The validated event establishes emoji existence before eligibility checks.
    // Other emoji still have their source request canceled and their baseline restored.
    const bool observed = message_reaction_controller_observe(
        &owner->controller, target, event.kind == GatewayMessageReactionEventKind::add,
        event.user_id == owner->controller.actor.self_user_id, clock_monotonic_milliseconds());
    synchronize_reactions(target.channel_id, target.message_id, &target, false);
    if (!owner || owner->closing)
        return;
    MessageReactionProjection projection{};
    const bool retained = message_reaction_controller_project(owner->controller, target, &projection);
    if (retained)
        project(target, projection);
    mirror();
    if (observed || retained)
        publish();
}

void application_message_reaction_message_observe(const GatewayMessageEvent& event, MessageHandle handle,
                                                  u32 revision, bool applied) noexcept {
    if (!owner)
        return;
    if (owner->closing || !same_actor(actor(), owner->controller.actor)) {
        application_message_reaction_synchronize();
        return;
    }
    if (!(event.fields & gateway_message_field_reactions)) {
        for (u32 i = 0; i < owner->controller.count; ++i) {
            const auto intent = owner->controller.entries[i];
            if (intent.target.channel_id == event.channel_id && intent.target.message_id == event.id)
                observed_source_current(intent, handle, revision, applied);
        }
        application_message_reaction_synchronize();
        return;
    }
    // A full snapshot supplies every aggregate. Apply it as the server-confirmed
    // state before checking eligibility, then overlay only the surviving desired states.
    bool changed = false;
    for (u32 i = 0; i < owner->controller.count;) {
        const auto intent = owner->controller.entries[i];
        if (intent.target.channel_id != event.channel_id || intent.target.message_id != event.id ||
            !observed_source_current(intent, handle, revision, applied)) {
            ++i;
            continue;
        }
        MessageReactionAuthority authority{};
        authority.known = true;
        for (u32 r = 0; r < event.reactions.count; ++r) {
            const auto& item = event.reactions[r];
            if (same_emoji(item.emoji_id, item.name, intent.target)) {
                authority = {item.count, item.normal_count, item.burst_count, item.me, item.me_burst, true,
                             true};
                break;
            }
        }
        const auto count = owner->controller.count;
        message_reaction_controller_snapshot(&owner->controller, intent.target, authority);
        changed = true;
        if (owner->controller.count == count)
            ++i;
    }
    synchronize_reactions(event.channel_id, event.id);
    if (!owner || owner->closing)
        return;
    for (u32 i = 0; i < owner->controller.count; ++i) {
        const auto& intent = owner->controller.entries[i];
        if (intent.target.channel_id != event.channel_id || intent.target.message_id != event.id)
            continue;
        MessageReactionProjection projection{};
        if (message_reaction_controller_project(owner->controller, intent.target, &projection))
            project(intent.target, projection);
    }
    mirror();
    if (changed)
        publish();
}
void application_message_reaction_message_deleted(u64 channel_id, u64 message_id) noexcept {
    if (!owner)
        return;
    const bool changed = message_reaction_controller_cancel(&owner->controller, channel_id, message_id);
    if (owner->controller.flight.retired)
        cancel_flight();
    if (owner->status.channel == channel_id && owner->status.message == message_id)
        reset_value(&owner->status);
    mirror();
    if (changed)
        publish();
}

bool queue_message_reaction(u64 message_id, u32 index) noexcept {
    const auto result = application_queue_message_reaction(message_id, index);
    return result == ApplicationMessageReactionEnqueueResult::accepted ||
           result == ApplicationMessageReactionEnqueueResult::coalesced;
}
bool queue_message_reaction_emoji(u64 message_id, u64 emoji_id, Utf8View label, bool animated) noexcept {
    const auto result = application_queue_message_reaction_emoji(message_id, emoji_id, label, animated);
    return result == ApplicationMessageReactionEnqueueResult::accepted ||
           result == ApplicationMessageReactionEnqueueResult::coalesced;
}
} // namespace maicord::app_detail
