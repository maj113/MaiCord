#include "application_state.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"

namespace maicord::reaction_runtime_test {
u64 monotonic_clock() noexcept;
u64 unix_clock() noexcept;
}
#define clock_monotonic_milliseconds ::maicord::reaction_runtime_test::monotonic_clock
#define clock_unix_milliseconds ::maicord::reaction_runtime_test::unix_clock
#include "../src/app/application_message_reaction.cpp"
#undef clock_unix_milliseconds
#undef clock_monotonic_milliseconds

namespace maicord::app_detail {
void application_emoji_preferences_track_reaction(ReactionEmojiRef emoji) noexcept {
    if (g_app.account_ui)
        reaction_preferences_track_emoji(&g_app.account_ui->reaction_preferences, ReactionPreferenceKind::reaction,
                                         emoji, 1);
}
} // namespace maicord::app_detail

namespace maicord::reaction_runtime_test {
using namespace app_detail;
u32 failures{}, refreshes{}, submissions{}, cancellations{};
u64 now_ms = 1000, request_serial{}, accepted_request{};
u64 unix_ms = 1'700'000'000'000;
bool transport_busy{};
MessageReactionTarget submitted_target{};
bool submitted_add{};
u8 account_marker{};
MessageRecord records[16]{};
u32 order[16]{}, heights[16]{};
u64 fenwick[16]{};
u8 text[8192]{}, rich_text[64 * 1024]{};
MessageRichRecord rich_records[16]{};
u16 rich_order[16]{};

u64 monotonic_clock() noexcept {
    return now_ms;
}
u64 unix_clock() noexcept {
    return unix_ms;
}

void write(Utf8View value) noexcept {
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), value.data, value.bytes, &written, nullptr);
}

void check(bool passed, const char* label) noexcept {
    if (passed)
        return;
    ++failures;
    u32 bytes{};
    while (label[bytes])
        ++bytes;
    write({label, bytes});
    write(utf8_literal("\n"));
}

void metric(const char* label, u64 value) noexcept {
    test_write_metric(label, value);
}

Utf8View glyph(u32 index = 0) noexcept {
    return unicode_emoji_catalog()[index].glyph();
}

RestClientSubmitResult WINAPI submit(void*, u64 channel, u64 message, u64 emoji, Utf8View label,
                                     MessageReactionType type, bool add,
                                     u64 request_id) noexcept {
    if (transport_busy)
        return {RestClientSubmitError::busy};
    ++submissions;
    accepted_request = request_id;
    submitted_target = {};
    submitted_target.channel_id = channel;
    submitted_target.message_id = message;
    submitted_target.emoji_id = emoji;
    submitted_target.name_bytes = static_cast<u16>(label.bytes);
    copy_bytes(submitted_target.name, label.data, label.bytes);
    submitted_target.type = type;
    submitted_add = add;
    return {};
}

RestClientSubmitResult WINAPI cancel(void*, AccountMessageSearchOperation operation,
                                     const MessageSearchQuery*, u64 request_id) noexcept {
    if (operation == AccountMessageSearchOperation::cancel && request_id == accepted_request)
        ++cancellations;
    return {};
}

ChannelRecord* channel(u64 id = 70) noexcept {
    return const_cast<ChannelRecord*>(
        chat_state_store_find_channel(g_app.account_ui->chat_state, id));
}

bool create_message(u64 id = 900) noexcept {
    GatewayMessageReaction baseline{};
    baseline.name = glyph();
    baseline.count = baseline.normal_count = 4;
    GatewayMessageEvent message{};
    message.id = id;
    message.channel_id = 70;
    message.author_id = 9;
    message.timestamp_ms = 1000;
    message.content = utf8_literal("Actual shipping reaction host fixture");
    message.fields = gateway_message_field_id | gateway_message_field_channel_id |
                     gateway_message_field_author_id | gateway_message_field_timestamp |
                     gateway_message_field_content | gateway_message_field_reactions;
    message.reactions = {&baseline, 1};
    return message_timeline_apply_created(&g_app.message_view.timeline, message).ok();
}

bool seed() noexcept {
    g_app.message_history_request_owner = MessageHistoryRequestOwner::none;
    g_app.primary_request_id = 0;
    reset_message_reaction();
    now_ms = 1000;
    unix_ms = 1'700'000'000'000;
    accepted_request = 0;
    transport_busy = false;
    g_app.account.instance = &account_marker;
    g_app.account.stop_failed = false;
    g_app.account.status.running = true;
    g_app.account.status.profile_id = 20;
    g_app.plugin_account_scope_serial = 10;
    g_app.login.visible = false;
    g_app.account.api.message_reaction = submit;
    g_app.account.api.message_search = cancel;
    g_app.account_ui->chat_state.self_user_id = 5;
    g_app.account_ui->active_channel_id = 70;
    chat_state_store_select_channel(&g_app.account_ui->chat_state, 70);
    const u64 channels[] = {70, 80};
    for (u64 id : channels) {
        auto* value = channel(id);
        if (!value)
            return false;
        value->type = 1;
        value->guild_id = value->parent_id = 0;
        value->thread_archived = value->thread_archived_known = false;
        value->thread_locked = value->thread_locked_known = false;
        value->actions = channel_action_view | channel_action_read_history |
                         channel_action_add_reactions | channel_action_send_messages;
    }
    g_app.message_mutation_notice = {};
    return message_store_initialize(&g_app.message_view.store, {records, 16}, {order, 16},
                                    {text, sizeof(text)}) &&
           ui_variable_list_initialize(&g_app.message_view.list, {heights, 16}, {fenwick, 16}, 0, 100) &&
           message_timeline_initialize(&g_app.message_view.timeline, &g_app.message_view.store,
                                       &g_app.message_view.list, 100, 100, true) &&
           message_rich_store_initialize(&g_app.account_ui->message_rich_store, {rich_records, 16},
                                         {rich_order, 16}, {rich_text, sizeof(rich_text)}) &&
           message_timeline_bind_rich_store(&g_app.message_view.timeline,
                                            &g_app.account_ui->message_rich_store) &&
           create_message();
}

const MessageRichReaction* reaction(u64 id = 900, u32 emoji_index = 0) noexcept {
    const auto* rich = message_rich_store_find(g_app.account_ui->message_rich_store, id);
    const auto label = glyph(emoji_index);
    for (u32 index = 0; rich && index < rich->reaction_count; ++index)
        if (utf8_equal(message_rich_store_string(g_app.account_ui->message_rich_store,
                                                 rich->reactions[index].name),
                       label))
            return &rich->reactions[index];
    return nullptr;
}

u32 reaction_index(u32 emoji_index = 0) noexcept {
    const auto* rich = message_rich_store_find(g_app.account_ui->message_rich_store, 900);
    const auto* value = reaction(900, emoji_index);
    return rich && value ? static_cast<u32>(value - rich->reactions) : ~u32{0};
}

bool send_next() noexcept {
    if (!application_message_reaction_pending(now_ms))
        return false;
    const u64 id = ++request_serial;
    if (!application_message_reaction_submit(id, now_ms))
        return false;
    g_app.message_history_request_owner = MessageHistoryRequestOwner::message_reaction;
    g_app.primary_request_id = id;
    return true;
}

bool complete(u16 status = 204, u64 retry_at = 0, bool global = false) noexcept {
    RestClientResponse response{};
    response.request_id = accepted_request;
    response.status_code = status;
    response.rate_limited = status == 429;
    response.retry_at_ms = retry_at;
    response.global_rate_limit = global;
    ++now_ms;
    const bool handled = application_message_reaction_response(response);
    if (handled) {
        g_app.message_history_request_owner = MessageHistoryRequestOwner::none;
        g_app.primary_request_id = 0;
    }
    return handled;
}

bool gateway(bool add, bool self, MessageReactionType type = MessageReactionType::normal,
             u32 emoji_index = 0) noexcept {
    GatewayMessageReactionEvent event{};
    event.channel_id = 70;
    event.message_id = 900;
    event.user_id = self ? 5 : 42;
    event.name = glyph(emoji_index);
    event.type = type;
    event.kind = add ? GatewayMessageReactionEventKind::add : GatewayMessageReactionEventKind::remove;
    const auto found = message_store_find(g_app.message_view.store, event.message_id);
    const auto* record =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    if (!record)
        return false;
    const auto applied = message_rich_store_mutate_reaction(
        &g_app.account_ui->message_rich_store, event.message_id, record->revision,
        {0, event.name, type,
         add ? MessageRichReactionMutationKind::add : MessageRichReactionMutationKind::remove, self,
         false});
    if (!applied.ok())
        return false;
    application_message_reaction_observe(event, found.handle, record->revision, true);
    return true;
}

bool clear_emoji(u32 emoji_index) noexcept {
    const auto found = message_store_find(g_app.message_view.store, 900);
    const auto* record =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    if (!record)
        return false;
    GatewayMessageReactionEvent event{};
    event.channel_id = 70;
    event.message_id = 900;
    event.name = glyph(emoji_index);
    event.kind = GatewayMessageReactionEventKind::remove_emoji;
    if (!message_rich_store_mutate_reaction(&g_app.account_ui->message_rich_store, 900, record->revision,
                                            {0, event.name, MessageReactionType::normal,
                                             MessageRichReactionMutationKind::remove_emoji, false, false})
             .ok())
        return false;
    application_message_reaction_observe(event, found.handle, record->revision, true);
    return true;
}

bool snapshot(ConstSlice<GatewayMessageReaction> reactions) noexcept {
    const auto found = message_store_find(g_app.message_view.store, 900);
    const auto* record =
        found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    if (!record)
        return false;
    GatewayMessageEvent event{};
    event.id = 900;
    event.channel_id = 70;
    event.fields = gateway_message_field_reactions;
    event.reactions = reactions;
    if (!message_rich_store_apply(&g_app.account_ui->message_rich_store, 900, record->revision, event).ok())
        return false;
    application_message_reaction_message_observe(event, found.handle, record->revision, true);
    return true;
}

bool seed_thread(u8 type = 11, bool locked = false) noexcept {
    if (!seed() ||
        !chat_state_store_upsert_guild(&g_app.account_ui->chat_state,
                                      {10, utf8_literal("Reaction guild"), GuildMutationKind::create, 99})
             .ok())
        return false;
    auto* guild = const_cast<GuildRecord*>(chat_state_store_find_guild(g_app.account_ui->chat_state, 10));
    guild->settings = {};
    guild->owner_id = 99;
    auto* parent = channel(80);
    parent->type = 0;
    parent->guild_id = 10;
    parent->permissions_resolved = true;
    parent->effective_permissions = kPermissionViewChannel | kPermissionReadMessageHistory;
    auto* value = channel();
    value->type = type;
    value->guild_id = 10;
    value->parent_id = 80;
    value->permissions_resolved = true;
    value->effective_permissions = parent->effective_permissions;
    value->thread_archived_known = value->thread_locked_known = true;
    value->thread_locked = locked;
    value->thread_self_joined_known = value->thread_self_joined = true;
    value->actions = chat_state_store_thread_actions(g_app.account_ui->chat_state, *value);
    return readable(*value);
}

bool restrict_member(bool timeout) noexcept {
    if (timeout)
        return chat_state_store_set_self_timeout(&g_app.account_ui->chat_state, 10, unix_ms / 1000 + 60);
    GuildSettings settings{};
    settings.known = guild_settings_self_quarantine;
    settings.self_member = guild_self_member_quarantined;
    return chat_state_store_set_guild_settings(&g_app.account_ui->chat_state, 10, settings);
}

void restricted_member_reaction_tests() noexcept {
    constexpr bool restrictions[]{false, true};
    for (bool timeout : restrictions) {
        check(seed_thread() && restrict_member(timeout) &&
                  application_queue_message_reaction(900, reaction_index()) ==
                      ApplicationMessageReactionEnqueueResult::permission &&
                  !owner && reaction()->count == 4 && !(reaction()->flags & 1),
              "reaction runtime: quarantine and timeout forbid existing emoji additions");
        GatewayMessageReaction mine{};
        mine.name = glyph();
        mine.count = mine.normal_count = 5;
        mine.me = true;
        check(snapshot({&mine, 1}) &&
                  application_queue_message_reaction(900, reaction_index()) ==
                      ApplicationMessageReactionEnqueueResult::accepted &&
                  send_next() && !submitted_add && complete() && reaction()->count == 4,
              "reaction runtime: restricted members can still remove their own active-thread reaction");

        check(seed_thread(), "reaction runtime: live member restriction queue fixture initializes");
        application_queue_message_reaction(900, reaction_index());
        const auto before = submissions;
        check(restrict_member(timeout) && !application_message_reaction_submit(++request_serial, now_ms) &&
                  owner && !owner->controller.count && reaction()->count == 4 &&
                  !(reaction()->flags & 1) && submissions == before,
              "reaction runtime: live member restriction cancels queued existing emoji without REST");

        check(seed_thread(), "reaction runtime: live member restriction retry fixture initializes");
        application_queue_message_reaction(900, reaction_index());
        check(send_next() && complete(429, 5000), "reaction runtime: unrestricted existing emoji queues retry");
        const auto before_retry = submissions;
        now_ms = 5000;
        check(restrict_member(timeout) && !application_message_reaction_submit(++request_serial, now_ms) &&
                  owner && !owner->controller.count && reaction()->count == 4 &&
                  !(reaction()->flags & 1) && submissions == before_retry,
              "reaction runtime: live member restriction cancels rate-limited existing emoji retry");

        check(seed_thread() && restrict_member(timeout), "reaction runtime: restricted owner fixture initializes");
        auto* guild = const_cast<GuildRecord*>(chat_state_store_find_guild(g_app.account_ui->chat_state, 10));
        guild->owner_id = 5;
        check(application_queue_message_reaction(900, reaction_index()) ==
                  ApplicationMessageReactionEnqueueResult::accepted,
              "reaction runtime: guild owner remains exempt from member reaction restrictions");

        check(seed_thread() && restrict_member(timeout), "reaction runtime: restricted administrator fixture initializes");
        auto& state = g_app.account_ui->chat_state;
        RoleRecord administrator{};
        administrator.id = 12;
        administrator.guild_id = 10;
        administrator.permissions = kPermissionAdministrator;
        state.roles = &administrator;
        state.role_count = 1;
        check(application_queue_message_reaction(900, reaction_index()) ==
                  ApplicationMessageReactionEnqueueResult::permission,
              "reaction runtime: another member's Administrator role grants no restriction exemption");
        administrator.self_assigned = true;
        check(!(channel()->effective_permissions & kPermissionAdministrator) &&
                  application_queue_message_reaction(900, reaction_index()) ==
                      ApplicationMessageReactionEnqueueResult::accepted,
              "reaction runtime: assigned Administrator exemption survives masked effective permissions");
        reset_message_reaction();
        state.roles = nullptr;
        state.role_count = 0;
    }
    check(seed_thread() && restrict_member(true), "reaction runtime: timeout expiry fixture initializes");
    unix_ms += 60'000;
    check(application_queue_message_reaction(900, reaction_index()) ==
              ApplicationMessageReactionEnqueueResult::accepted,
          "reaction runtime: timeout ending at current Unix second restores existing emoji additions");
}

void thread_reaction_tests() noexcept {
    constexpr u8 types[]{10, 11, 12};
    for (u8 type : types) {
        check(seed_thread(type, true), "reaction runtime: active locked thread fixture initializes");
        check(!(channel()->actions & channel_action_add_reactions) &&
                  application_queue_message_reaction_emoji(900, 0, glyph(1), false) ==
                      ApplicationMessageReactionEnqueueResult::permission &&
                  application_queue_message_reaction(900, reaction_index()) ==
                      ApplicationMessageReactionEnqueueResult::accepted &&
                  send_next() && submitted_add && complete() &&
                  application_queue_message_reaction(900, reaction_index()) ==
                      ApplicationMessageReactionEnqueueResult::coalesced &&
                  send_next() && !submitted_add && complete() && reaction()->count == 4 &&
                  !(reaction()->flags & 1),
              "reaction runtime: active locked threads allow existing emoji add and own removal without "
              "Add Reactions while new emoji remains forbidden");

        check(seed_thread(type), "reaction runtime: archived thread admission fixture initializes");
        const auto before = submissions;
        check(chat_state_store_set_thread_archived(&g_app.account_ui->chat_state, 70, 5, true),
              "reaction runtime: authoritative archive update applies");
        channel()->actions |= channel_action_add_reactions;
        check(application_queue_message_reaction(900, reaction_index()) ==
                      ApplicationMessageReactionEnqueueResult::permission &&
                  application_queue_message_reaction_emoji(900, 0, glyph(1), false) ==
                      ApplicationMessageReactionEnqueueResult::permission &&
                  !owner && reaction()->count == 4 && !(reaction()->flags & 1),
              "reaction runtime: archived threads reject existing and new emoji despite stale action bits");
        GatewayMessageReaction mine{};
        mine.name = glyph();
        mine.count = mine.normal_count = 5;
        mine.me = true;
        check(snapshot({&mine, 1}) &&
                  application_queue_message_reaction(900, reaction_index()) ==
                      ApplicationMessageReactionEnqueueResult::permission &&
                  reaction()->count == 5 && (reaction()->flags & 1) && !send_next() &&
                  submissions == before,
              "reaction runtime: archived threads refuse own removal without mutating aggregates or REST");
        check(chat_state_store_set_thread_archived(&g_app.account_ui->chat_state, 70, 5, false) &&
                  chat_state_store_set_thread_archived(&g_app.account_ui->chat_state, 70, 5, false, false) &&
                  application_queue_message_reaction(900, reaction_index()) ==
                      ApplicationMessageReactionEnqueueResult::permission &&
                  application_queue_message_reaction_emoji(900, 0, glyph(), false) ==
                      ApplicationMessageReactionEnqueueResult::permission && !owner,
              "reaction runtime: unknown archive authority refuses both own removal and existing emoji add");
    }

    check(seed_thread(), "reaction runtime: archive after queued existing add fixture initializes");
    application_queue_message_reaction(900, reaction_index());
    const auto before_queue = submissions;
    check(chat_state_store_set_thread_archived(&g_app.account_ui->chat_state, 70, 5, true) &&
              !application_message_reaction_submit(++request_serial, now_ms) && owner &&
              !owner->controller.count && !owner->controller.flight.active &&
              reaction()->count == 4 && !(reaction()->flags & 1) && submissions == before_queue,
          "reaction runtime: submit rechecks source archive state and restores queued existing emoji add");

    constexpr bool known_states[]{false, true};
    for (bool known : known_states) {
        check(seed_thread(), "reaction runtime: removal archive authority fixture initializes");
        GatewayMessageReaction mine{};
        mine.name = glyph();
        mine.count = mine.normal_count = 5;
        mine.me = true;
        check(snapshot({&mine, 1}) &&
                  application_queue_message_reaction(900, reaction_index()) ==
                      ApplicationMessageReactionEnqueueResult::accepted && reaction()->count == 4,
              "reaction runtime: queued removal projects exactly one own contribution");
        const auto before_remove = submissions;
        if (!known)
            g_app.account_ui->active_channel_id = 80;
        check(chat_state_store_set_thread_archived(&g_app.account_ui->chat_state, 70, 5, true, known) &&
                  !application_message_reaction_pending(now_ms) && owner && !owner->controller.count &&
                  !send_next() && submissions == before_remove,
              "reaction runtime: source thread archive authority cancels queued removal after navigation");
        check(!known || (reaction()->count == 5 && (reaction()->flags & 1)),
              "reaction runtime: archived resident removal restores exact server own contribution");
        g_app.account_ui->active_channel_id = 70;
        // Rehydrate the server's own contribution after returning to the source.
        check(snapshot({&mine, 1}) &&
                  chat_state_store_set_thread_archived(&g_app.account_ui->chat_state, 70, 5, false) &&
                  !send_next() && reaction()->count == 5 && (reaction()->flags & 1) &&
                  submissions == before_remove,
              "reaction runtime: renewed active authority cannot replay a canceled removal or inverse add");
    }

    check(seed_thread(), "reaction runtime: archived retry fixture initializes");
    application_queue_message_reaction(900, reaction_index());
    check(send_next() && complete(429, 5000), "reaction runtime: existing emoji rate limit queues retry");
    const auto before_retry = submissions;
    now_ms = 5000;
    check(chat_state_store_set_thread_archived(&g_app.account_ui->chat_state, 70, 5, true) &&
              !application_message_reaction_submit(++request_serial, now_ms) && owner &&
              !owner->controller.count && reaction()->count == 4 && !(reaction()->flags & 1) &&
              submissions == before_retry,
          "reaction runtime: archived source cancels due retry without another request or ghost contribution");

    check(seed_thread(), "reaction runtime: archive during in-flight own removal fixture initializes");
    GatewayMessageReaction mine{};
    mine.name = glyph();
    mine.count = mine.normal_count = 5;
    mine.me = true;
    check(snapshot({&mine, 1}) &&
              application_queue_message_reaction(900, reaction_index()) ==
                  ApplicationMessageReactionEnqueueResult::accepted && send_next() && !submitted_add,
          "reaction runtime: own removal holds exact transport request");
    const auto before_cancel = cancellations;
    check(chat_state_store_set_thread_archived(&g_app.account_ui->chat_state, 70, 5, true) &&
              application_message_reaction_synchronize() && owner && !owner->controller.count &&
              owner->controller.flight.retired && cancellations == before_cancel + 1 &&
              reaction()->count == 5 && (reaction()->flags & 1) && complete() &&
              !owner->controller.flight.active && !send_next() && reaction()->count == 5 &&
              (reaction()->flags & 1),
          "reaction runtime: archive retires removal flight once and late success cannot replay inverse state");
}

void run() noexcept {
    static_assert(sizeof(MessageReactionIntent) == 232);
    static_assert(sizeof(MessageReactionController) == 1408);
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.account_ui != nullptr, "reaction runtime: real account UI allocates");
    if (!g_app.account_ui)
        return;
    auto& ui = *g_app.account_ui;
    check(chat_state_store_initialize(&ui.chat_state, {ui.guild_record_storage, 2},
                                         {ui.guild_order_storage, 2}, {ui.channel_record_storage, 4},
                                         {ui.channel_order_storage, 4}, {ui.chat_state_name_storage, 4096}) &&
              chat_state_store_upsert_channel(&ui.chat_state,
                                                 {70, 0, 0, utf8_literal("Reaction DM"), 0, 0, 1})
                  .ok() &&
              chat_state_store_upsert_channel(&ui.chat_state,
                                                 {80, 0, 0, utf8_literal("Other DM"), 0, 0, 1})
                  .ok(),
          "reaction runtime: real channel metadata initializes");
    check(seed() && readable(*channel()),
          "reaction runtime: resident authoritative source and read permissions initialize");
    check(!owner, "reaction runtime: no lazy ledger exists before first action");
    check(application_queue_message_reaction_emoji(900, 0, glyph(), false) ==
              ApplicationMessageReactionEnqueueResult::accepted,
          "reaction runtime: picker selection explicitly adds current-user reaction");
    check(reaction() && reaction()->count == 5 && (reaction()->flags & 1),
          "reaction runtime: actual rich model projects one own contribution over four foreigners");
    MEMORY_BASIC_INFORMATION owner_region{}, bank_region{};
    VirtualQuery(owner, &owner_region, sizeof(owner_region));
    VirtualQuery(owner->storage.arena.base, &bank_region, sizeof(bank_region));
    metric("reaction owner sizeof=", sizeof(ReactionOwner));
    metric("reaction owner committed=", owner_region.RegionSize);
    metric("reaction initial bank committed=", bank_region.RegionSize);
    check(owner_region.State == MEM_COMMIT && owner_region.RegionSize == 4096 &&
              bank_region.State == MEM_COMMIT && bank_region.RegionSize == 4096,
          "reaction runtime: first action commits one owner page and one ledger page");
    check(send_next() && submitted_add,
          "reaction runtime: exact copied add submits once through fake Account API");
    check(application_queue_message_reaction(900, reaction_index()) ==
                  ApplicationMessageReactionEnqueueResult::coalesced &&
              reaction()->count == 4 && !(reaction()->flags & 1),
          "reaction runtime: pill toggle accepts newer remove while add is owned");
    check(application_queue_message_reaction_emoji(900, 0, glyph(1), false) ==
                  ApplicationMessageReactionEnqueueResult::accepted &&
              owner->controller.count == 2,
          "reaction runtime: distinct emoji accepted while previous REST remains pending");
    check(gateway(true, true) && reaction()->count == 4 && !(reaction()->flags & 1),
          "reaction runtime: actual old own add event applies then host reprojects latest remove");
    check(gateway(true, false) && reaction()->count == 5 && !(reaction()->flags & 1),
          "reaction runtime: foreign Gateway contribution survives newer remove projection");
    check(complete() && send_next(),
          "reaction runtime: old response drains and distinct ready target can submit");
    check(complete() && send_next() && !submitted_add,
          "reaction runtime: final remove remains queued after distinct successful target");
    check(complete() && !application_message_reaction_pending(now_ms),
          "reaction runtime: settled echo fences do not schedule duplicate REST");
    check(gateway(true, true) && reaction()->count == 5 && !(reaction()->flags & 1),
          "reaction runtime: late old add echo cannot undo HTTP-confirmed remove");
    check(gateway(false, true) && reaction()->count == 5 && !(reaction()->flags & 1),
          "reaction runtime: final matching own echo preserves all foreign contributions");

    check(seed(), "reaction runtime: failure fixture resets actual models");
    application_queue_message_reaction_emoji(900, 0, glyph(1), false);
    check(send_next() && complete(403) && !reaction(900, 1),
          "reaction runtime: terminal failure removes only unconfirmed new emoji without zero-count pill");
    bool error{};
    check(application_message_reaction_status(900, &error).bytes && error,
          "reaction runtime: exact terminal failure remains visible for retry");
    application_queue_message_reaction_emoji(900, 0, glyph(), false);
    check(send_next(), "reaction runtime: fresh action follows prior terminal failure");
    application_queue_message_reaction(900, reaction_index());
    application_queue_message_reaction(900, reaction_index());
    check(complete(403) && reaction()->count == 5 && (reaction()->flags & 1) &&
              application_message_reaction_pending(now_ms),
          "reaction runtime: older failure cannot rollback fresh add-remove-add choice");

    check(seed(), "reaction runtime: queued new emoji permission retirement fixture resets");
    application_queue_message_reaction_emoji(900, 0, glyph(), false);
    check(send_next(),
          "reaction runtime: existing emoji request owns active flight before permission change");
    const u64 existing_request = accepted_request;
    application_queue_message_reaction_emoji(900, 0, glyph(1), false);
    check(reaction(900, 1) && reaction(900, 1)->count == 1,
          "reaction runtime: new queued emoji initially owns only its optimistic contribution");
    channel()->actions &= ~channel_action_add_reactions;
    check(application_message_reaction_synchronize() && owner && owner->controller.flight.active &&
              !owner->controller.flight.retired &&
              owner->controller.flight.ticket.request_id == existing_request &&
              owner->controller.count == 1 && !reaction(900, 1) && reaction()->count == 5 &&
              (reaction()->flags & 1),
          "reaction runtime: Add reactions revocation removes only unavailable new emoji while existing "
          "flight remains eligible");
    bool permission_error{};
    const auto permission_status = application_message_reaction_status(900, &permission_error);
    const auto permission_notice = application_message_mutation_notice_text();
    check(permission_status.bytes && permission_error && permission_notice.bytes &&
              application_message_mutation_notice_is_error(),
          "reaction runtime: canceled latest new emoji owns visible scoped permission failure");
    const auto before_old_success = submissions;
    check(complete() && application_message_reaction_status(900, &permission_error).bytes &&
              permission_error && application_message_mutation_notice_text().bytes &&
              application_message_mutation_notice_is_error() &&
              !application_message_reaction_pending(now_ms) && !send_next() &&
              submissions == before_old_success && !reaction(900, 1),
          "reaction runtime: old existing emoji success cannot clear newer permission error or revive "
          "canceled new emoji");

    check(seed(), "reaction runtime: content revision permission retirement fixture resets");
    application_queue_message_reaction_emoji(900, 0, glyph(1), false);
    const auto old_source = message_store_find(g_app.message_view.store, 900);
    const auto old_revision = records[old_source.handle.slot].revision;
    channel()->actions &= ~channel_action_add_reactions;
    GatewayMessageEvent content_update{};
    content_update.id = 900;
    content_update.channel_id = 70;
    content_update.fields =
        gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_content;
    content_update.content = utf8_literal("Fresh authoritative content retained through reaction rollback");
    check(message_timeline_apply_updated(&g_app.message_view.timeline, content_update).ok(),
          "reaction runtime: content-only authoritative update advances actual resident model revision");
    const auto fresh_source = message_store_find(g_app.message_view.store, 900);
    const auto* fresh_record = message_store_resolve(g_app.message_view.store, fresh_source.handle);
    check(fresh_record && fresh_source.handle.slot == old_source.handle.slot &&
              fresh_source.handle.generation == old_source.handle.generation &&
              fresh_record->revision != old_revision,
          "reaction runtime: typed content update proves exact handle with newer source revision");
    application_message_reaction_message_observe(content_update, fresh_source.handle,
                                                 fresh_record ? fresh_record->revision : 0, true);
    check(owner && !owner->controller.count && !reaction(900, 1) && reaction()->count == 4 &&
              fresh_record &&
              utf8_equal(message_store_content(g_app.message_view.store, fresh_source.handle),
                         content_update.content),
          "reaction runtime: proved fresh content revision rolls back unavailable new emoji without ghost "
          "or content loss");

    check(seed(), "reaction runtime: API admission fixture resets");
    application_queue_message_reaction_emoji(900, 0, glyph(), false);
    transport_busy = true;
    check(!send_next() && owner && !owner->controller.flight.active && reaction()->count == 5,
          "reaction runtime: transient shared-lane busy leaves queued choice and optimistic state owned");
    transport_busy = false;
    now_ms += 49;
    check(!application_message_reaction_pending(now_ms),
          "reaction runtime: API busy uses finite wake backoff");
    ++now_ms;
    check(send_next(), "reaction runtime: queued action becomes eligible after busy backoff");
    const auto active_id = accepted_request;
    RestClientResponse stale{};
    stale.request_id = active_id + 1;
    stale.status_code = 204;
    check(!application_message_reaction_response(stale) && owner->controller.flight.active,
          "reaction runtime: unrelated response cannot drain actual submitted request ID");
    channel()->actions &= ~channel_action_read_history;
    check(
        application_message_reaction_synchronize() && owner && owner->controller.flight.retired &&
            !owner->controller.count && cancellations,
        "reaction runtime: source permission revocation cancels intent while retaining exact flight drain");
    check(complete() && !owner->controller.flight.active,
          "reaction runtime: revoked source successful response drains without resurrection");

    check(seed(), "reaction runtime: source reuse fixture resets");
    application_queue_message_reaction_emoji(900, 0, glyph(), false);
    send_next();
    check(message_timeline_remove_local(&g_app.message_view.timeline, 900).ok() && create_message() &&
              application_message_reaction_synchronize() && owner->controller.flight.retired &&
              !owner->controller.count,
          "reaction runtime: actual resident slot recycle retires old source generation");
    check(complete() && reaction()->count == 4 && !(reaction()->flags & 1),
          "reaction runtime: old successful response cannot project into recycled same message ID");

    check(seed(), "reaction runtime: navigation fixture resets");
    application_queue_message_reaction_emoji(900, 0, glyph(), false);
    send_next();
    ui.active_channel_id = 80;
    application_message_reaction_synchronize();
    check(owner && !owner->controller.flight.retired && owner->controller.count == 1,
          "reaction runtime: navigation retains authorized off-selected reaction intent");
    check(complete(), "reaction runtime: off-selected response still drains actual owner");
    ui.active_channel_id = 70;
    application_message_reaction_synchronize();
    check(reaction()->count == 5 && (reaction()->flags & 1),
          "reaction runtime: returning to source retains accepted own state");
    ++g_app.plugin_account_scope_serial;
    application_message_reaction_synchronize();
    check(!owner, "reaction runtime: actor retirement releases settled demand-backed owner");

    check(seed(), "reaction runtime: actual history reload fixture resets");
    application_queue_message_reaction_emoji(900, 0, glyph(), false);
    check(send_next(), "reaction runtime: history reload retains one actual submitted request");
    application_queue_message_reaction_emoji(900, 0, glyph(1), false);
    ui.active_channel_id = 80;
    application_message_reaction_synchronize();
    check(message_timeline_reset(&g_app.message_view.timeline),
          "reaction runtime: channel navigation clears actual message and rich history stores");
    ui.active_channel_id = 70;
    chat_state_store_select_channel(&ui.chat_state, 70);
    check(create_message() && application_message_reaction_synchronize() && owner &&
              !owner->controller.flight.retired && owner->controller.count == 2 && reaction() &&
              reaction()->count == 5 && (reaction()->flags & 1) && reaction(900, 1) &&
              reaction(900, 1)->count == 1 && (reaction(900, 1)->flags & 1),
          "reaction runtime: hydrated source receives both latest retained emoji choices before frame "
          "publication");
    const auto reload_refreshes = refreshes;
    const auto reload_committed = owner->storage.arena.committed;
    const auto reload_revision = owner->controller.revision;
    for (u32 index = 0; index < 8; ++index)
        application_message_reaction_synchronize();
    check(refreshes == reload_refreshes && owner->storage.arena.committed == reload_committed &&
              owner->controller.revision == reload_revision,
          "reaction runtime: unchanged synchronizations after hydration retain projection without "
          "rebuilding");
    check(complete() && send_next() && complete() && reaction()->count == 5 && reaction(900, 1)->count == 1,
          "reaction runtime: original flight drains against legally rebound history without losing second "
          "queued choice");

    check(seed(), "reaction runtime: settled echo plus pending snapshot fixture resets");
    application_queue_message_reaction_emoji(900, 0, glyph(), false);
    check(send_next() && complete(), "reaction runtime: first snapshot target owns a settled echo fence");
    application_queue_message_reaction_emoji(900, 0, glyph(1), false);
    GatewayMessageReaction authoritative[2]{};
    authoritative[0].name = glyph();
    authoritative[0].count = authoritative[0].normal_count = 3;
    authoritative[1].name = glyph(1);
    authoritative[1].count = authoritative[1].normal_count = 7;
    check(snapshot({authoritative, 2}) && owner && owner->controller.count == 1 && reaction()->count == 3 &&
              !(reaction()->flags & 1) && reaction(900, 1)->count == 8 && (reaction(900, 1)->flags & 1) &&
              application_message_reaction_pending(now_ms),
          "reaction runtime: full snapshot erases settled first intent then still refreshes shifted "
          "pending second intent");
    check(send_next() && complete(403) && reaction(900, 1)->count == 7 && !(reaction(900, 1)->flags & 1) &&
              reaction()->count == 3 && !(reaction()->flags & 1),
          "reaction runtime: later pending failure restores its snapshot baseline while confirmed first "
          "emoji stays intact");

    check(seed(), "reaction runtime: fresh foreign existence before permission retirement fixture resets");
    application_queue_message_reaction_emoji(900, 0, glyph(1), false);
    application_queue_message_reaction_emoji(900, 0, glyph(2), false);
    channel()->actions &= ~channel_action_add_reactions;
    check(gateway(true, false, MessageReactionType::normal, 1) && owner &&
              owner->controller.count == 1 && reaction(900, 1) && reaction(900, 1)->count == 2 &&
              (reaction(900, 1)->flags & 1) && !reaction(900, 2),
          "reaction runtime: fresh foreign add proves queued emoji existence while different absent emoji "
          "retires exactly");
    check(application_message_reaction_pending(now_ms) && send_next() && complete() &&
              reaction(900, 1)->count == 2 && (reaction(900, 1)->flags & 1) && !reaction(900, 2),
          "reaction runtime: newly existing emoji remains eligible without Add reactions and canceled "
          "neighbor never revives");

    check(seed(), "reaction runtime: full snapshot existence before permission retirement fixture resets");
    application_queue_message_reaction_emoji(900, 0, glyph(1), false);
    application_queue_message_reaction_emoji(900, 0, glyph(2), false);
    channel()->actions &= ~channel_action_add_reactions;
    GatewayMessageReaction fresh_existing[2]{};
    fresh_existing[0].name = glyph();
    fresh_existing[0].count = fresh_existing[0].normal_count = 4;
    fresh_existing[1].name = glyph(1);
    fresh_existing[1].count = fresh_existing[1].normal_count = 3;
    check(snapshot({fresh_existing, 2}) && owner && owner->controller.count == 1 && reaction(900, 1) &&
              reaction(900, 1)->count == 4 && (reaction(900, 1)->flags & 1) && !reaction(900, 2) &&
              reaction()->count == 4 && !(reaction()->flags & 1) &&
              application_message_reaction_pending(now_ms),
          "reaction runtime: complete fresh snapshot establishes existing emoji before eligibility and "
          "removes absent neighbor overlay");

    check(seed(), "reaction runtime: targeted administrative removal protection fixture resets");
    application_queue_message_reaction_emoji(900, 0, glyph(), false);
    check(send_next(), "reaction runtime: administrative removal begins with exact submitted emoji owner");
    application_queue_message_reaction_emoji(900, 0, glyph(2), false);
    channel()->actions &= ~channel_action_add_reactions;
    check(clear_emoji(0) && owner && owner->controller.flight.retired && !owner->controller.count &&
              !reaction() && !reaction(900, 2),
          "reaction runtime: remove emoji protects only its authoritative cleared target while other "
          "unavailable overlay retires");
    check(complete() && !reaction() && !reaction(900, 2) && !application_message_reaction_pending(now_ms),
          "reaction runtime: retired administrative target response cannot restore cleared emoji or "
          "canceled neighbor");

    check(seed(), "reaction runtime: expired intent fresh foreign removal fixture resets");
    application_queue_message_reaction_emoji(900, 0, glyph(), false);
    now_ms += kMessageReactionIntentLifetimeMs;
    check(gateway(false, false) && owner && !owner->controller.count && reaction() &&
              reaction()->count == 3 && reaction()->normal_count == 3 && !(reaction()->flags & 1) &&
              !application_message_reaction_pending(now_ms),
          "reaction runtime: fresh foreign remove at pending deadline retains three foreigners and clears "
          "expired own overlay");

    check(seed(), "reaction runtime: pending expiry achieved own evidence fixture resets");
    application_queue_message_reaction_emoji(900, 0, glyph(), false);
    check(send_next(), "reaction runtime: achieved expiry retains exact submitted request until drain");
    now_ms = 1000 + kMessageReactionIntentLifetimeMs;
    bool achieved_error{};
    check(gateway(true, true) && owner && !owner->controller.count && reaction() &&
              reaction()->count == 5 && reaction()->normal_count == 5 && (reaction()->flags & 1) &&
              owner->controller.flight.active && owner->controller.flight.retired &&
              !application_message_reaction_pending(now_ms) &&
              !application_message_reaction_status(900, &achieved_error).bytes && !achieved_error &&
              !application_message_mutation_notice_text().bytes &&
              !application_message_mutation_notice_is_error(),
          "reaction runtime: achieved own state at expiry settles without false failure and retains exact "
          "transport drain");
    check(complete(500) && !owner->controller.flight.active && reaction()->count == 5 &&
              (reaction()->flags & 1) && !application_message_reaction_status(900, &achieved_error).bytes &&
              !achieved_error && !application_message_reaction_pending(now_ms),
          "reaction runtime: late failed HTTP after achieved expiry drains without rollback or false retry "
          "failure");

    check(seed(), "reaction runtime: expired settled own fence live event fixture resets");
    application_queue_message_reaction_emoji(900, 0, glyph(), false);
    check(send_next() && complete(), "reaction runtime: successful choice retains settled own echo fence");
    now_ms = 1000 + kMessageReactionIntentLifetimeMs;
    check(gateway(false, true) && owner && !owner->controller.count && reaction() &&
              reaction()->count == 4 && !(reaction()->flags & 1),
          "reaction runtime: settled fence expires before contrary live own event so current Gateway state "
          "wins");

    check(seed(), "reaction runtime: exact residual vote aggregate fixture resets");
    GatewayMessageReaction vote_only{};
    vote_only.name = glyph();
    vote_only.count = 2;
    check(snapshot({&vote_only, 1}) && reaction()->count == 2 && !reaction()->normal_count &&
              !reaction()->burst_count && !(reaction()->flags & 3),
          "reaction runtime: explicit zero normal and burst counts retain exact residual vote total");
    check(application_queue_message_reaction_emoji(900, 0, glyph(), false) ==
                  ApplicationMessageReactionEnqueueResult::accepted &&
              reaction()->count == 3 && reaction()->normal_count == 1 && !reaction()->burst_count &&
              (reaction()->flags & 1),
          "reaction runtime: normal add contributes one own normal over two existing votes");
    check(send_next() && gateway(true, false, MessageReactionType::vote) && reaction()->count == 4 &&
              reaction()->normal_count == 1 && !reaction()->burst_count && (reaction()->flags & 1),
          "reaction runtime: foreign vote Gateway event survives pending normal projection without count "
          "invention");
    check(complete() && reaction()->count == 4 && reaction()->normal_count == 1 &&
              !reaction()->burst_count && (reaction()->flags & 1) && !(reaction()->flags & 2),
          "reaction runtime: normal HTTP confirmation preserves three residual votes and exact own normal "
          "membership");
    check(gateway(true, true, MessageReactionType::vote) &&
              gateway(false, true, MessageReactionType::vote) && reaction()->count == 5 &&
              reaction()->normal_count == 1 && !reaction()->burst_count && (reaction()->flags & 1) &&
              !(reaction()->flags & 2),
          "reaction runtime: own vote events follow aggregate model semantics without fabricating normal "
          "or burst ownership");

    check(seed(), "reaction runtime: bounded capacity fixture resets");
    for (u32 message = 0; message < 16; ++message) {
        if (message)
            check(create_message(900 + message),
                  "reaction runtime: bounded additional authoritative source creates");
        for (u32 emoji = 0; emoji < 4; ++emoji)
            check(application_queue_message_reaction_emoji(900 + message, 0, glyph(emoji), false) ==
                      ApplicationMessageReactionEnqueueResult::accepted,
                  "reaction runtime: each of64 distinct choices admits independently");
    }
    check(owner && owner->controller.count == 64 &&
              application_queue_message_reaction_emoji(900, 0, glyph(4), false) ==
                  ApplicationMessageReactionEnqueueResult::capacity &&
              !reaction(900, 4),
          "reaction runtime:65th action refuses without touching model or existing choices");
    VirtualQuery(owner->storage.arena.base, &bank_region, sizeof(bank_region));
    metric("reaction full bank committed=", bank_region.RegionSize);
    metric("reaction full logical intents=", owner->controller.count);
    check(bank_region.RegionSize == 16384 && bank_region.State == MEM_COMMIT,
          "reaction runtime:64 entries commit bounded16KiB ledger and no per-intent allocation");
    const auto text_refreshes = refreshes;
    const auto committed = owner->storage.arena.committed;
    const auto revision = owner->controller.revision;
    const auto rich_revision = ui.message_rich_store.updated;
    for (u32 index = 0; index < 8; ++index)
        application_message_reaction_synchronize();
    check(
        refreshes == text_refreshes && owner->storage.arena.committed == committed &&
            owner->controller.revision == revision && ui.message_rich_store.updated == rich_revision,
        "reaction runtime: warm synchronizations do not reproject model, rebuild text or grow backing");
    reset_message_reaction();
    check(!owner, "reaction runtime: actual reset releases bounded lazy owner after no transport borrow");
    thread_reaction_tests();
    restricted_member_reaction_tests();
    reset_message_reaction();
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
    g_app.account.instance = nullptr;
}
} // namespace maicord::reaction_runtime_test

namespace maicord::app_detail {
bool refresh_account_text(bool) noexcept {
    ++reaction_runtime_test::refreshes;
    return true;
}
void application_media_picker_reaction_changed() noexcept {}
} // namespace maicord::app_detail

extern "C" __declspec(noreturn) void WINAPI mainCRTStartup() noexcept {
    maicord::reaction_runtime_test::run();
    maicord::reaction_runtime_test::write(maicord::reaction_runtime_test::failures
                                              ? maicord::utf8_literal("FAIL reaction host runtime\n")
                                              : maicord::utf8_literal("PASS reaction host runtime\n"));
    ExitProcess(maicord::reaction_runtime_test::failures ? 1 : 0);
}
