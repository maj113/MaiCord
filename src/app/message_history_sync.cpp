#include "maicord/app/message_history_sync.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {
namespace {

static_assert(sizeof(MessageHistoryCatchup) == 64);
static_assert(sizeof(MessageHistoryOlder) == 64);
static_assert(sizeof(AttachmentRefresh) == 48);

bool history_transient_failure(const RestClientResponse& response) noexcept {
    if (api_rejection_requires_action(response.rejection))
        return false;
    switch (response.error) {
    case RestClientError::resolve:
    case RestClientError::connect:
    case RestClientError::tls:
    case RestClientError::send:
    case RestClientError::receive:
    case RestClientError::timeout:
        return true;
    case RestClientError::none:
        return response.status_code == 408 || response.status_code == 500 ||
               response.status_code == 502 || response.status_code == 503 || response.status_code == 504;
    default:
        return false;
    }
}

u64 history_retry_deadline(u8* retries, const RestClientResponse& response, u64 now_ms) noexcept {
    // Read-only pages are safe to replay. Keep the cursor and resident window,
    // let the failed transport reconnect, and cap offline polling at once/30s.
    const u64 delay_ms = minimum(500ull << minimum(static_cast<u32>(*retries), 6u), 30'000ull);
    if (*retries < 6)
        ++*retries;
    const u64 local = deadline_after(now_ms, delay_ms);
    // Only a completed HTTP response can supply a trusted server wait.
    return response.transport_ok() ? maximum(local, response.retry_at_ms) : local;
}

inline constexpr u32 kHistoryRichFieldMask =
    gateway_message_field_message_reference | gateway_message_field_referenced_message |
    gateway_message_field_reactions | gateway_message_field_attachments | gateway_message_field_embeds |
    gateway_message_field_sticker_items | gateway_message_field_components | gateway_message_field_interaction |
    gateway_message_field_message_snapshots | gateway_message_field_thread;

MessageHistoryApplyResult apply_error(MessageHistoryApplyError error, bool destructive = false) noexcept {
    MessageHistoryApplyResult result{};
    result.error = error;
    result.destructive_failure = destructive;
    return result;
}

constexpr bool valid_content_availability(MessageContentAvailability availability) noexcept {
    return availability == MessageContentAvailability::unknown ||
           availability == MessageContentAvailability::available ||
           availability == MessageContentAvailability::restricted;
}

UiScrollAnchor capture_anchor(const MessageTimeline& timeline) noexcept {
    UiScrollAnchor anchor{};
    if (!timeline.follow_bottom && timeline.list->count != 0) {
        ui_variable_list_capture_anchor(*timeline.list, timeline.scroll_units, &anchor);
    }
    return anchor;
}

void restore_scroll(MessageTimeline* timeline, UiScrollAnchor* anchor, u64 previous_scroll) noexcept {
    u64 restored = previous_scroll;
    if (timeline->follow_bottom) {
        message_timeline_restore_scroll(timeline, message_timeline_maximum_scroll(*timeline), true);
    } else if (anchor->valid &&
               ui_variable_list_restore_anchor(*timeline->list, *anchor, &restored)) {
        message_timeline_restore_scroll(timeline, restored, false);
    } else {
        message_timeline_restore_scroll(timeline, previous_scroll, false);
    }
}

bool content_overlaps_store(const MessageStore& store, Utf8View content) noexcept {
    if (!content.data || content.bytes == 0) {
        return false;
    }
    const u64 content_begin = reinterpret_cast<u64>(content.data);
    const u64 text_begin = reinterpret_cast<u64>(store.text);
    return content_begin < text_begin ? text_begin - content_begin < content.bytes
                                      : content_begin - text_begin < store.text_capacity;
}

const MessageRecord* ordered_record(const MessageStore& store, u32 index) noexcept {
    return message_store_resolve(store, message_store_ordered_handle(store, index));
}
const MessageRecord* authoritative_edge(const MessageStore& store, bool newest) noexcept {
    for (u32 i = 0; i < store.count; ++i) {
        const auto* record = ordered_record(store, newest ? store.count - 1 - i : i);
        if (record && record->delivery == MessageDeliveryState::authoritative) return record;
    }
    return nullptr;
}

MessageContentAvailability resolved_availability(const GatewayMessageEvent& message,
                                                 MessageContentAvailability empty_availability) noexcept {
    // REST message objects distinguish an absent content field from a present
    // empty string. The latter is normal for rich-only messages.
    return (message.fields & gateway_message_field_content) != 0 ? MessageContentAvailability::available
                                                                 : empty_availability;
}

bool page_matches_summary(const MessageHistoryPage& page,
                          ConstSlice<GatewayMessageEvent> messages) noexcept {
    if (page.count != messages.count) {
        return false;
    }
    if (messages.count == 0) {
        return page.channel_id == 0 && page.newest_id == 0 && page.oldest_id == 0 &&
               page.content_bytes == 0;
    }
    u64 content_bytes = 0;
    for (u32 index = 0; index < messages.count; ++index) {
        content_bytes += messages[index].content.bytes;
    }
    return page.channel_id == messages[0].channel_id && page.newest_id == messages[0].id &&
           page.oldest_id == messages[messages.count - 1].id && content_bytes == page.content_bytes;
}

bool observe_history_users(UserStore* users, ConstSlice<GatewayMessageEvent> messages) noexcept {
    if (!users) {
        return true;
    }
    for (u32 message_index = 0; message_index < messages.count; ++message_index) {
        const GatewayMessageEvent& message = messages[message_index];
        if (message.interaction.user_name.bytes != 0) {
            const auto& interaction = message.interaction;
            if (!user_store_upsert(users, interaction.user_id, interaction.user_name).ok() ||
                (interaction.user_avatar_hash.bytes != 0 &&
                 !user_store_set_avatar(users, interaction.user_id, interaction.user_avatar_hash))) {
                return false;
            }
        }
        if (message.author_name.bytes != 0 &&
            (message.author_id == 0 ||
             !user_store_upsert(users, message.author_id, message.author_name).ok())) {
            return false;
        }
        if (message.author_avatar_hash.bytes != 0 &&
            (message.author_id == 0 || !user_store_find(*users, message.author_id) ||
             !user_store_set_avatar(users, message.author_id, message.author_avatar_hash))) {
            return false;
        }
        if (message.reference.author_name.bytes != 0 &&
            (message.reference.author_id == 0 ||
             !user_store_upsert(users, message.reference.author_id, message.reference.author_name)
                  .ok())) {
            return false;
        }
        if (message.reference.author_avatar_hash.bytes != 0 &&
            (message.reference.author_id == 0 ||
             !user_store_find(*users, message.reference.author_id) ||
             !user_store_set_avatar(users, message.reference.author_id,
                                            message.reference.author_avatar_hash))) {
            return false;
        }
        for (u32 user_index = 0; user_index < message.mention_users.count; ++user_index) {
            const GatewayMessageNamedUser& user = message.mention_users[user_index];
            if (!user_store_upsert(users, user.id, user.name).ok()) {
                return false;
            }
        }
        // Quoted mentions supply display names, but do not participate in the
        // outer message's unread/notification mention policy.
        for (u32 user_index = 0; user_index < message.snapshot.mention_users.count; ++user_index) {
            const auto& user = message.snapshot.mention_users[user_index];
            if (!user_store_upsert(users, user.id, user.name).ok()) return false;
        }
    }
    return true;
}

bool observe_history_members(GuildMemberStore* members, ChatStateStore* state,
                             ConstSlice<GatewayMessageEvent> messages) noexcept {
    if (!members && !state) {
        return true;
    }
    if (!members || !state) {
        return false;
    }
    for (u32 index = 0; index < messages.count; ++index) {
        const GatewayMessageEvent& message = messages[index];
        if ((message.fields & gateway_message_field_member) == 0) {
            continue;
        }
        const ChannelRecord* channel = chat_state_store_find_channel(*state, message.channel_id);
        if (!channel || channel->guild_id == 0 || message.author_id == 0 ||
            !guild_member_store_upsert(members, *state, channel->guild_id, message.author_id,
                                               message.member_nick, message.member_roles, true,
                                               GuildMemberStatus::unknown)
                 .ok()) {
            return false;
        }
    }
    return true;
}

bool decimal_id(Utf8View value, u32 first, u32 last, u64* id) noexcept {
    if (!id || first >= last || last > value.bytes) {
        return false;
    }
    u64 parsed = 0;
    for (u32 index = first; index < last; ++index) {
        const char character = value.data[index];
        if (character < '0' || character > '9') {
            return false;
        }
        const u64 digit = static_cast<u64>(character - '0');
        if (parsed > (~0ull - digit) / 10ull) {
            return false;
        }
        parsed = parsed * 10ull + digit;
    }
    *id = parsed;
    return parsed != 0;
}

bool history_message_mentions_current_member(const ChatStateStore& state,
                                             const GatewayMessageEvent& message) noexcept {
    if (message.mention_everyone || message.mentions_current_user) {
        return true;
    }
    for (u32 index = 0; index < message.mention_users.count; ++index) {
        if (message.mention_users[index].id == state.self_user_id) {
            return true;
        }
    }
    const ChannelRecord* channel = chat_state_store_find_channel(state, message.channel_id);
    const Utf8View content = message.content;
    for (u32 offset = 0; offset + 4u <= content.bytes; ++offset) {
        if (content.data[offset] != '<' || content.data[offset + 1u] != '@') {
            continue;
        }
        u32 cursor = offset + 2u;
        bool role = false;
        if (cursor < content.bytes && content.data[cursor] == '!') {
            ++cursor;
        } else if (cursor < content.bytes && content.data[cursor] == '&') {
            role = true;
            ++cursor;
        }
        const u32 digits = cursor;
        while (cursor < content.bytes && content.data[cursor] >= '0' && content.data[cursor] <= '9') {
            ++cursor;
        }
        u64 id = 0;
        if (cursor >= content.bytes || content.data[cursor] != '>' ||
            !decimal_id(content, digits, cursor, &id)) {
            continue;
        }
        if ((!role && id == state.self_user_id) ||
            (role && channel && channel->guild_id != 0 &&
             chat_state_store_self_has_role(state, channel->guild_id, id))) {
            return true;
        }
        offset = cursor;
    }
    return false;
}

MessageHistoryDecodeResult decode_page(Utf8View body, const MessageHistoryDecodeScratch& scratch) noexcept {
    return decode_message_history_page(body, scratch.page_structurals, scratch.message_structurals,
                                       scratch.maximum_depth, scratch.content, scratch.messages,
                                       scratch.mention_users, scratch.rich);
}

// Retry, tick and fail serve both paged loads: each is a state machine over awaiting_page,
// waiting_retry, request_required and failed.
template <class State, class Machine>
bool page_retry(Machine* machine, MessageHistoryCatchupFailure failure, u64 retry_at_ms) noexcept {
    if (!machine || machine->state != State::awaiting_page || failure == MessageHistoryCatchupFailure::none ||
        failure == MessageHistoryCatchupFailure::apply || retry_at_ms == 0) {
        return false;
    }
    machine->last_failure = failure;
    machine->retry_at_ms = retry_at_ms;
    machine->state = State::waiting_retry;
    ++machine->failures;
    return true;
}

template <class State, class Machine>
bool page_tick(Machine* machine, u64 now_ms) noexcept {
    if (!machine || machine->state != State::waiting_retry || now_ms < machine->retry_at_ms) {
        return false;
    }
    machine->retry_at_ms = 0;
    machine->state = State::request_required;
    return true;
}

template <class State, class Machine>
bool page_fail(Machine* machine, MessageHistoryCatchupFailure failure) noexcept {
    if (!machine || machine->state != State::awaiting_page || failure == MessageHistoryCatchupFailure::none) {
        return false;
    }
    machine->last_failure = failure;
    machine->state = State::failed;
    ++machine->failures;
    return true;
}

// Shared REST completion of a catch-up or older page: transient failures retry, other failures
// end the load, and a decoded page goes through apply_page.
template <class State, class Machine, class Apply>
MessageHistoryRestResult accept_page_rest(Machine* machine, const RestClientResponse& response,
                                          const MessageHistoryDecodeScratch& scratch, u64 now_ms,
                                          UserStore* users, GuildMemberStore* members, ChatStateStore* state,
                                          MessageTimeline* timeline, Apply apply_page, Utf8View body) noexcept {
    MessageHistoryRestResult result{};
    result.disposition = MessageHistoryRestDisposition::failed;
    if (history_transient_failure(response)) {
        page_retry<State>(machine,
                          response.transport_ok() ? MessageHistoryCatchupFailure::http_status
                                                  : MessageHistoryCatchupFailure::transport,
                          history_retry_deadline(&machine->transient_retries, response, now_ms));
        result.disposition = MessageHistoryRestDisposition::retry_scheduled;
        return result;
    }
    if (!response.transport_ok()) {
        page_fail<State>(machine, MessageHistoryCatchupFailure::transport);
        result.error = MessageHistoryRestError::transport;
        return result;
    }
    if (response.status_code == 429) {
        if (api_rejection_requires_action(response.rejection) || !response.rate_limited ||
            response.retry_at_ms == 0 ||
            !page_retry<State>(machine, MessageHistoryCatchupFailure::rate_limited, response.retry_at_ms)) {
            page_fail<State>(machine, MessageHistoryCatchupFailure::rate_limited);
            result.error = MessageHistoryRestError::rate_limit;
            return result;
        }
        result.disposition = MessageHistoryRestDisposition::retry_scheduled;
        return result;
    }
    if (response.status_code != 200) {
        page_fail<State>(machine, MessageHistoryCatchupFailure::http_status);
        result.error = MessageHistoryRestError::http_status;
        return result;
    }
    result.decode = decode_page(body, scratch);
    const ConstSlice<GatewayMessageEvent> decoded{scratch.messages.data, result.decode.page.count};
    if (!result.decode.ok() || !observe_history_users(users, decoded) ||
        !observe_history_members(members, state, decoded)) {
        page_fail<State>(machine, MessageHistoryCatchupFailure::decode);
        result.error = MessageHistoryRestError::decode;
        return result;
    }
    result.apply = apply_page(result.decode.page, decoded);
    if (!result.apply.ok() || !message_history_apply_mention_flags(timeline, state, decoded)) {
        result.error = MessageHistoryRestError::apply;
        return result;
    }
    result.disposition = MessageHistoryRestDisposition::applied;
    return result;
}

} // namespace

bool message_history_observe_page(UserStore* users, GuildMemberStore* members,
                                  ChatStateStore* state,
                                  ConstSlice<GatewayMessageEvent> messages) noexcept {
    return observe_history_users(users, messages) && observe_history_members(members, state, messages);
}

bool message_history_apply_mention_flags(MessageTimeline* timeline, const ChatStateStore* state,
                                         ConstSlice<GatewayMessageEvent> messages) noexcept {
    if (!timeline || !timeline->store) {
        return false;
    }
    if (!state) {
        return true;
    }
    for (u32 index = 0; index < messages.count; ++index) {
        const MessageStoreResult found = message_store_find(*timeline->store, messages[index].id);
        if (found.error == MessageStoreError::not_found) {
            continue;
        }
        const auto* record = found.ok() ? message_store_resolve(*timeline->store, found.handle) : nullptr;
        if (!record || !message_store_set_presentation_flags(
                               timeline->store, messages[index].id,
                               (record->presentation_flags & ~message_presentation_mentions_current_user) |
                               (history_message_mentions_current_member(*state, messages[index])
                                   ? message_presentation_mentions_current_user : 0u))) {
            return false;
        }
    }
    return true;
}

MessageHistoryApplyResult message_timeline_apply_history_page(
    MessageTimeline* timeline, ConstSlice<GatewayMessageEvent> messages, MessageHistoryDirection direction,
    MessageContentAvailability empty_content_availability) noexcept {
    if (!timeline || !timeline->store || !timeline->list || !timeline->store->records ||
        !timeline->store->order || !timeline->store->text || !timeline->list->item_units ||
        !timeline->list->fenwick || timeline->store->count != timeline->list->count ||
        (!messages.data && messages.count != 0) || messages.count > kMessageHistoryMaximumMessages ||
        !valid_content_availability(empty_content_availability) ||
        (direction != MessageHistoryDirection::older && direction != MessageHistoryDirection::newer)) {
        return apply_error(timeline && timeline->store && timeline->list &&
                                   timeline->store->count != timeline->list->count
                               ? MessageHistoryApplyError::count_mismatch
                               : MessageHistoryApplyError::invalid_input);
    }
    if (messages.count == 0) {
        return MessageHistoryApplyResult{};
    }

    MessageStore& store = *timeline->store;
    const u64 channel_id = messages[0].channel_id;
    u64 previous_id = ~0ull;
    // Page structure (identity, order, storage aliasing) is validated as a
    // whole. A single message the store cannot admit is left out instead: one
    // unexpected field must never make a channel's history unloadable.
    u8 kept[kMessageHistoryMaximumMessages];
    u32 kept_count = 0;
    for (u32 index = 0; index < messages.count; ++index) {
        const GatewayMessageEvent& message = messages[index];
        if (message.channel_id != channel_id) {
            return apply_error(MessageHistoryApplyError::channel_mismatch);
        }
        if (message.id >= previous_id || content_overlaps_store(store, message.content)) {
            return apply_error(MessageHistoryApplyError::invalid_page);
        }
        previous_id = message.id;
        const MessageReconcileResult validation = message_reconcile_validate_create(
            &store, message, resolved_availability(message, empty_content_availability));
        if (validation.ok()) {
            kept[kept_count++] = static_cast<u8>(index);
        }
    }
    const u32 skipped = messages.count - kept_count;
    const auto page_message = [&](u32 index) noexcept -> const GatewayMessageEvent& {
        return messages[kept[index]];
    };
    for (u32 index = 0; index < store.count; ++index) {
        const MessageRecord* record = ordered_record(store, index);
        if (!record || record->channel_id != channel_id) {
            return apply_error(MessageHistoryApplyError::channel_mismatch);
        }
    }

    u32 selected_begin = 0;
    u32 selected_count = kept_count;
    if (const MessageRecord* first = authoritative_edge(store, false)) {
        const MessageRecord* last = authoritative_edge(store, true);
        if (!last) {
            return apply_error(MessageHistoryApplyError::reconcile);
        }
        if (direction == MessageHistoryDirection::newer) {
            selected_count = 0;
            while (selected_count < kept_count && page_message(selected_count).id > last->id) {
                ++selected_count;
            }
        } else {
            while (selected_begin < kept_count && page_message(selected_begin).id >= first->id) {
                ++selected_begin;
            }
            selected_count = kept_count - selected_begin;
        }
    }
    if (selected_count == 0) {
        MessageHistoryApplyResult result{};
        result.skipped = skipped;
        ++timeline->history_pages;
        return result;
    }
    const u32 resident_capacity = minimum(store.capacity, timeline->list->capacity);
    if (resident_capacity == 0 || selected_count > resident_capacity) {
        return apply_error(MessageHistoryApplyError::capacity);
    }
    u64 incoming_bytes = 0;
    for (u32 index = 0; index < selected_count; ++index) {
        incoming_bytes += page_message(selected_begin + index).content.bytes;
        if (incoming_bytes > store.text_capacity) {
            return apply_error(MessageHistoryApplyError::capacity);
        }
    }

    // Local sends are a derived outbox projection, not server history anchors.
    // The controller restores them after this validated, contiguous merge.
    const u32 before_local_removal = store.count;
    const u64 before_local_scroll = timeline->scroll_units;
    u32 removed_locals = 0;
    for (u32 i = store.count; i != 0; --i) {
        const auto* record = ordered_record(store, i - 1);
        if (record && record->delivery != MessageDeliveryState::authoritative) {
            if (!message_timeline_remove_local(timeline, record->id).ok()) return apply_error(MessageHistoryApplyError::reconcile, true);
            ++removed_locals;
        }
    }
    u32 eviction_count = store.count + selected_count > resident_capacity
                             ? store.count + selected_count - resident_capacity
                             : 0;
    u64 live_after = store.text_used - store.text_wasted;
    for (u32 index = 0; index < eviction_count; ++index) {
        const u32 victim = direction == MessageHistoryDirection::newer ? index : store.count - 1 - index;
        const MessageRecord* record = ordered_record(store, victim);
        if (!record || record->content_bytes > live_after) {
            return apply_error(MessageHistoryApplyError::reconcile);
        }
        live_after -= record->content_bytes;
    }
    while (incoming_bytes > store.text_capacity - live_after) {
        if (eviction_count >= store.count) {
            return apply_error(MessageHistoryApplyError::capacity);
        }
        const u32 victim =
            direction == MessageHistoryDirection::newer ? eviction_count : store.count - 1 - eviction_count;
        const MessageRecord* record = ordered_record(store, victim);
        if (!record || record->content_bytes > live_after) {
            return apply_error(MessageHistoryApplyError::reconcile);
        }
        live_after -= record->content_bytes;
        ++eviction_count;
    }

    const u32 old_count = timeline->list->count;
    const u64 previous_scroll = timeline->scroll_units;
    UiScrollAnchor anchor = capture_anchor(*timeline);
    const u32 previous_compactions = store.compactions;
    u32 rich_skipped = 0;
    for (u32 index = 0; index < eviction_count; ++index) {
        const u32 victim = direction == MessageHistoryDirection::newer ? 0 : store.count - 1;
        const MessageRecord* record = ordered_record(store, victim);
        if (!record) {
            return apply_error(MessageHistoryApplyError::reconcile, true);
        }
        const u64 evicted_id = record->id;
        if (!message_store_delete(&store, evicted_id) ||
            (timeline->rich_store && !message_rich_store_delete(timeline->rich_store, evicted_id))) {
            return apply_error(MessageHistoryApplyError::reconcile, true);
        }
    }
    for (u32 index = selected_count; index != 0; --index) {
        const GatewayMessageEvent& message = page_message(selected_begin + index - 1);
        const MessageReconcileResult inserted = message_reconcile_create(
            &store, message, resolved_availability(message, empty_content_availability));
        if (!inserted.ok() || !inserted.inserted) {
            return apply_error(MessageHistoryApplyError::reconcile, true);
        }
        if (timeline->rich_store &&
            (((message.fields & kHistoryRichFieldMask) != 0) || message.rich_truncations != 0)) {
            const MessageRecord* resident = message_store_resolve(store, inserted.handle);
            if (!resident) {
                return apply_error(MessageHistoryApplyError::reconcile, true);
            }
            // The message itself is resident; rich data the bounded rich store
            // rejects only degrades to the plain message, never fails the page.
            if (!message_rich_store_apply(timeline->rich_store, message.id, resident->revision, message).ok()) {
                ++rich_skipped;
            }
        }
    }

    const u32 removal_index =
        direction == MessageHistoryDirection::newer ? 0 : timeline->list->count - eviction_count;
    if (eviction_count != 0 &&
        !ui_variable_list_remove(timeline->list, removal_index, eviction_count, &anchor)) {
        return apply_error(MessageHistoryApplyError::ui_mutation, true);
    }
    const u32 insertion_index = direction == MessageHistoryDirection::older ? 0 : timeline->list->count;
    if (!ui_variable_list_insert(timeline->list, insertion_index, selected_count,
                                 timeline->default_item_units, &anchor)) {
        return apply_error(MessageHistoryApplyError::ui_mutation, true);
    }
    if (store.count != timeline->list->count) {
        return apply_error(MessageHistoryApplyError::count_mismatch, true);
    }
    restore_scroll(timeline, &anchor, previous_scroll);

    MessageHistoryApplyResult result{};
    result.inserted = selected_count;
    result.skipped = skipped;
    result.rich_skipped = rich_skipped;
    result.evicted = eviction_count + removed_locals;
    result.compactions = store.compactions - previous_compactions;
    const u32 first_changed =
        direction == MessageHistoryDirection::older || eviction_count != 0 || removed_locals ? 0 : old_count;
    const u32 changed_end = maximum(maximum(old_count, before_local_removal), timeline->list->count);
    result.invalidation = MessageTimelineInvalidation{
        first_changed, first_changed < changed_end ? changed_end - first_changed : 1, true,
        timeline->scroll_units != previous_scroll || timeline->scroll_units != before_local_scroll};
    timeline->evicted += eviction_count + removed_locals;
    timeline->compactions += result.compactions;
    ++timeline->history_pages;
    timeline->history_inserted += selected_count;
    return result;
}

MessageHistoryApplyResult message_history_refresh_attachment(
    MessageTimeline* timeline, const GatewayMessageEvent& message, u64 expected_message_id,
    u64 expected_channel_id, u32 expected_message_revision, u64 expected_attachment_id,
    MessageContentAvailability empty_content_availability) noexcept {
    if (!timeline || !timeline->store || !timeline->rich_store || !timeline->list ||
        timeline->store->count != timeline->list->count || expected_message_id == 0 ||
        expected_channel_id == 0 || expected_message_revision == 0 || expected_attachment_id == 0 ||
        !valid_content_availability(empty_content_availability) || message.id != expected_message_id ||
        message.channel_id != expected_channel_id ||
        (message.fields & gateway_message_field_attachments) == 0) {
        return apply_error(MessageHistoryApplyError::invalid_input);
    }
    const MessageStoreResult found = message_store_find(*timeline->store, expected_message_id);
    if (found.error == MessageStoreError::not_found) {
        return apply_error(MessageHistoryApplyError::not_found);
    }
    const MessageRecord* existing =
        found.ok() ? message_store_resolve(*timeline->store, found.handle) : nullptr;
    if (!found.ok() || !existing || existing->channel_id != expected_channel_id) {
        return apply_error(MessageHistoryApplyError::reconcile);
    }
    if (existing->revision != expected_message_revision) {
        return apply_error(MessageHistoryApplyError::stale_revision);
    }
    bool attachment_present = false;
    for (u32 index = 0; index < message.attachments.count; ++index) {
        attachment_present = attachment_present || message.attachments[index].id == expected_attachment_id;
    }
    if (!attachment_present) {
        return apply_error(MessageHistoryApplyError::attachment_missing);
    }
    const MessageReconcileResult validation = message_reconcile_validate_create(
        timeline->store, message, resolved_availability(message, empty_content_availability));
    if (!validation.ok()) {
        return apply_error(MessageHistoryApplyError::reconcile);
    }
    const u32 previous_compactions = timeline->store->compactions;
    const MessageReconcileResult updated = message_reconcile_create(
        timeline->store, message, resolved_availability(message, empty_content_availability));
    if (!updated.ok() || updated.inserted) {
        return apply_error(MessageHistoryApplyError::reconcile, true);
    }
    const MessageRecord* resident = message_store_resolve(*timeline->store, updated.handle);
    if (!resident ||
        !message_rich_store_apply(timeline->rich_store, message.id, resident->revision, message).ok()) {
        return apply_error(MessageHistoryApplyError::rich_store, true);
    }
    MessageHistoryApplyResult result{};
    result.compactions = timeline->store->compactions - previous_compactions;
    result.invalidation = MessageTimelineInvalidation{updated.order_index, 1, false, false};
    ++timeline->applied;
    return result;
}

void message_history_attachment_reset(AttachmentRefresh* refresh) noexcept {
    if (refresh) {
        *refresh = AttachmentRefresh{};
    }
}

bool message_history_attachment_tick(AttachmentRefresh* refresh, u64 now_ms) noexcept {
    if (!refresh) {
        return false;
    }
    if (refresh->state == AttachmentRefreshState::waiting_retry && now_ms >= refresh->retry_at_ms) {
        refresh->retry_at_ms = 0;
        refresh->state = AttachmentRefreshState::request_required;
    }
    return true;
}

bool message_history_attachment_queue(AttachmentRefresh* refresh, u64 channel_id, u64 message_id,
                                      u32 message_revision, u64 attachment_id) noexcept {
    if (!refresh || channel_id == 0 || message_id == 0 || message_revision == 0 || attachment_id == 0) {
        return false;
    }
    if (refresh->state != AttachmentRefreshState::idle) {
        return refresh->channel_id == channel_id && refresh->message_id == message_id &&
               refresh->attachment_id == attachment_id;
    }
    refresh->channel_id = channel_id;
    refresh->message_id = message_id;
    refresh->attachment_id = attachment_id;
    refresh->source_message_revision = message_revision;
    refresh->state = AttachmentRefreshState::request_required;
    return true;
}

MessageHistoryRestResult message_history_jump_accept_rest(
    MessageTimeline* timeline, UserStore* users, GuildMemberStore* members,
    ChatStateStore* state, const RestClientResponse& response, Utf8View body,
    u64 target_channel_id, u64 target_message_id, const MessageHistoryDecodeScratch& scratch,
    MessageContentAvailability empty_content_availability) noexcept {
    MessageHistoryRestResult result{};
    result.disposition = MessageHistoryRestDisposition::failed;
    if (!timeline || !users || !members || !state || target_channel_id == 0 || target_message_id == 0 ||
        (!body.data && body.bytes != 0) || response.body_bytes != body.bytes ||
        !valid_content_availability(empty_content_availability)) {
        result.error = MessageHistoryRestError::invalid_input;
        return result;
    }
    if (!response.transport_ok()) {
        result.error = MessageHistoryRestError::transport;
        return result;
    }
    if (response.status_code == 429) {
        result.error = MessageHistoryRestError::rate_limit;
        return result;
    }
    if (response.status_code != 200) {
        result.error = MessageHistoryRestError::http_status;
        return result;
    }
    result.decode = decode_page(body, scratch);
    const ConstSlice<GatewayMessageEvent> decoded{scratch.messages.data, result.decode.page.count};
    bool target_present = false;
    for (u32 index = 0; result.decode.ok() && index < decoded.count; ++index) {
        target_present = target_present || (decoded[index].id == target_message_id &&
                                            decoded[index].channel_id == target_channel_id);
    }
    if (!result.decode.ok() || !target_present) {
        result.error = MessageHistoryRestError::decode;
        return result;
    }
    if (!message_history_observe_page(users, members, state, decoded)) {
        result.error = MessageHistoryRestError::apply;
        return result;
    }
    if (!message_timeline_reset(timeline)) {
        result.apply = apply_error(MessageHistoryApplyError::ui_mutation, true);
        result.error = MessageHistoryRestError::apply;
        return result;
    }
    result.apply = message_timeline_apply_history_page(timeline, decoded, MessageHistoryDirection::newer,
                                                       empty_content_availability);
    if (!result.apply.ok() || !message_history_apply_mention_flags(timeline, state, decoded)) {
        if (result.apply.ok()) {
            result.apply = apply_error(MessageHistoryApplyError::ui_mutation, true);
        }
        result.error = MessageHistoryRestError::apply;
        return result;
    }
    result.disposition = MessageHistoryRestDisposition::applied;
    return result;
}

MessageHistoryJumpEdges message_history_jump_edges(ConstSlice<GatewayMessageEvent> page, u64 target_message_id,
                                                   u32 request_limit, u64 channel_last_message_id) noexcept {
    MessageHistoryJumpEdges edges{true, true};
    if (!page.data || page.count == 0 || target_message_id == 0 || request_limit == 0)
        return edges;
    // Pages are newest first. The official client sorts [target, ...page] ascending and
    // takes the target's index: the rows strictly older than it.
    u32 older = 0;
    for (u32 index = 0; index < page.count; ++index)
        older += page[index].id < target_message_id ? 1u : 0u;
    const u32 after_half = request_limit / 2u;
    const u32 before_half = after_half + request_limit % 2u;
    if (older + 1u < before_half)
        edges.more_before = false;
    if (page.count - older < after_half)
        edges.more_after = false;
    const u64 newest = page[0].id;
    if (edges.more_after && channel_last_message_id != 0 && newest == channel_last_message_id)
        edges.more_after = false;
    // A create observed after the server built this page is newer than its
    // newest row; reaching the present must still fetch it.
    if (!edges.more_after && channel_last_message_id > newest)
        edges.more_after = true;
    return edges;
}

bool message_history_jump_settle(MessageHistoryCatchup* catchup, MessageHistoryOlder* older,
                                 MessageEventPump* pump, u64 channel_id,
                                 ConstSlice<GatewayMessageEvent> page, MessageHistoryJumpEdges edges) noexcept {
    if (!catchup || !older || !pump || !pump->timeline || channel_id == 0 || (!page.data && page.count))
        return false;
    MessageTimeline& timeline = *pump->timeline;
    // Browsing stays ready (older paging, attachment and reply refreshes): a
    // detached window waits paused for the reader to approach its bottom.
    *catchup = MessageHistoryCatchup{};
    catchup->channel_id = channel_id;
    catchup->deferred_mark = timeline.deferred;
    catchup->incremental = edges.more_after;
    catchup->state = edges.more_after ? MessageHistoryCatchupState::paused : MessageHistoryCatchupState::complete;
    *older = MessageHistoryOlder{};
    older->channel_id = channel_id;
    if (!edges.more_before && page.count) {
        older->cursor_before = page[page.count - 1].id;
        older->state = MessageHistoryOlderState::exhausted;
    }
    timeline.newer_refetch_required = edges.more_after;
    pump->catchup_required = false;
    if (edges.more_after)
        timeline.follow_bottom = false;
    return true;
}

bool message_history_jump_to_present(MessageHistoryCatchup* catchup, MessageHistoryOlder* older,
                                     MessageEventPump* pump, u64 channel_id, u32 request_limit) noexcept {
    if (!catchup || !older || !pump || !pump->timeline || !pump->timeline->store || channel_id == 0 ||
        request_limit == 0)
        return false;
    MessageTimeline& timeline = *pump->timeline;
    if (!timeline.newer_refetch_required || timeline.store->count == 0 ||
        !(catchup->incremental || message_history_browsing_ready(catchup->state)))
        return false;
    // An in-flight page for the old window no longer matches its owner state
    // (catch-up request_required, older idle) and is discarded as stale.
    if (!message_timeline_reset(&timeline) || !message_timeline_set_scroll(&timeline, 0, true))
        return false;
    *older = MessageHistoryOlder{};
    pump->catchup_required = true;
    timeline.newer_refetch_required = true;
    return message_history_catchup_begin(catchup, pump, channel_id, request_limit);
}

MessageHistoryRestResult message_history_attachment_accept_rest(
    AttachmentRefresh* refresh, MessageTimeline* timeline, const RestClientResponse& response,
    Utf8View body, const MessageHistoryDecodeScratch& scratch,
    MessageContentAvailability empty_content_availability) noexcept {
    MessageHistoryRestResult result{};
    result.disposition = MessageHistoryRestDisposition::failed;
    if (!refresh || !timeline || refresh->state != AttachmentRefreshState::awaiting_response ||
        (!body.data && body.bytes != 0) || response.body_bytes != body.bytes ||
        !valid_content_availability(empty_content_availability)) {
        result.error = MessageHistoryRestError::invalid_input;
        return result;
    }
    if (!response.transport_ok()) {
        result.error = MessageHistoryRestError::transport;
        message_history_attachment_reset(refresh);
        return result;
    }
    if (response.status_code == 429) {
        if (response.rate_limited && response.retry_at_ms != 0) {
            refresh->retry_at_ms = response.retry_at_ms;
            refresh->state = AttachmentRefreshState::waiting_retry;
            ++refresh->failures;
            result.disposition = MessageHistoryRestDisposition::retry_scheduled;
            return result;
        }
        result.error = MessageHistoryRestError::rate_limit;
        message_history_attachment_reset(refresh);
        return result;
    }
    if (response.status_code != 200) {
        result.error = MessageHistoryRestError::http_status;
        message_history_attachment_reset(refresh);
        return result;
    }
    result.decode = decode_page(body, scratch);
    if (!result.decode.ok() || result.decode.page.count != 1) {
        result.error = MessageHistoryRestError::decode;
        message_history_attachment_reset(refresh);
        return result;
    }
    result.apply = message_history_refresh_attachment(timeline, scratch.messages[0], refresh->message_id,
                                                      refresh->channel_id, refresh->source_message_revision,
                                                      refresh->attachment_id, empty_content_availability);
    const bool superseded = result.apply.error == MessageHistoryApplyError::not_found ||
                            result.apply.error == MessageHistoryApplyError::stale_revision ||
                            result.apply.error == MessageHistoryApplyError::attachment_missing;
    if (!result.apply.ok() && !superseded) {
        result.error = MessageHistoryRestError::apply;
    } else {
        result.disposition = MessageHistoryRestDisposition::applied;
    }
    message_history_attachment_reset(refresh);
    return result;
}

bool message_history_reply_queue(ReplyReferenceRefresh* refresh, const MessageTimeline& timeline,
                                 u64 channel_id, u64 now_ms) noexcept {
    if (!refresh || !channel_id || !timeline.store || !timeline.rich_store) return false;
    if (refresh->channel_id != channel_id) *refresh = {channel_id};
    if (refresh->state == AttachmentRefreshState::waiting_retry && now_ms >= refresh->retry_at_ms) {
        refresh->state = AttachmentRefreshState::request_required;
        refresh->retry_at_ms = 0;
    }
    if (refresh->state != AttachmentRefreshState::idle) return true;
    for (u32 i = 0; i < timeline.store->count; ++i) {
        const auto* record = ordered_record(*timeline.store, i);
        if (!record || record->channel_id != channel_id || record->id <= refresh->completed_through_id ||
            record->content_availability != MessageContentAvailability::available ||
            record->delivery != MessageDeliveryState::authoritative) continue;
        const auto* rich = message_rich_store_find(*timeline.rich_store, record->id);
        if (!rich || rich->message_revision != record->revision ||
            rich->reference.state != GatewayMessageReferenceState::unknown || !rich->reference.message_id) continue;
        refresh->message_id = record->id;
        refresh->reference_id = rich->reference.message_id;
        refresh->state = AttachmentRefreshState::request_required;
        break;
    }
    return true;
}

MessageHistoryRestResult message_history_reply_accept_rest(
    ReplyReferenceRefresh* refresh, MessageTimeline* timeline, UserStore* users,
    const RestClientResponse& response, Utf8View body,
    const MessageHistoryDecodeScratch& scratch) noexcept {
    MessageHistoryRestResult result{};
    result.disposition = MessageHistoryRestDisposition::applied;
    if (!refresh || !timeline || !timeline->store || !timeline->rich_store ||
        refresh->state != AttachmentRefreshState::awaiting_response || response.body_bytes != body.bytes) {
        result.error = MessageHistoryRestError::invalid_input;
        return result;
    }
    if (response.status_code == 429 && response.rate_limited && response.retry_at_ms) {
        refresh->state = AttachmentRefreshState::waiting_retry;
        refresh->retry_at_ms = response.retry_at_ms;
        result.disposition = MessageHistoryRestDisposition::retry_scheduled;
        return result;
    }
    // A failed passive quote refresh must not reset the channel or loop forever.
    refresh->state = AttachmentRefreshState::idle;
    refresh->completed_through_id = maximum(refresh->completed_through_id, refresh->message_id);
    if (!response.transport_ok() || response.status_code != 200) return result;
    result.decode = decode_page(body, scratch);
    if (!result.decode.ok()) { result.error = MessageHistoryRestError::decode; return result; }
    for (u32 i = 0; i < result.decode.page.count; ++i) {
        const auto& source = scratch.messages[i];
        if (source.id != refresh->message_id || source.channel_id != refresh->channel_id ||
            source.reference.message_id != refresh->reference_id ||
            !(source.fields & gateway_message_field_referenced_message)) continue;
        const auto found = message_store_find(*timeline->store, source.id);
        const auto* record = found.ok() ? message_store_resolve(*timeline->store, found.handle) : nullptr;
        const auto* rich = message_rich_store_find(*timeline->rich_store, source.id);
        if (!record || record->channel_id != refresh->channel_id || !rich ||
            rich->message_revision != record->revision || rich->reference.message_id != refresh->reference_id ||
            rich->reference.state != GatewayMessageReferenceState::unknown) break;
        GatewayMessageEvent quote{};
        quote.id = source.id;
        quote.fields = gateway_message_field_message_reference | gateway_message_field_referenced_message;
        quote.reference = source.reference;
        if (!observe_history_users(users, {&quote, 1}) ||
            !message_rich_store_apply(timeline->rich_store, source.id, record->revision, quote).ok()) {
            result.error = MessageHistoryRestError::apply;
            break;
        }
        result.apply.invalidation = {found.order_index, 1, false, false};
        break;
    }
    return result;
}

bool message_history_catchup_begin(MessageHistoryCatchup* catchup, MessageEventPump* pump, u64 channel_id,
                                   u32 request_limit) noexcept {
    if (!catchup || !pump || !pump->timeline || !pump->timeline->store || channel_id == 0 ||
        request_limit == 0 || request_limit > kMessageHistoryMaximumMessages ||
        (!pump->timeline->follow_bottom && !message_history_newer_prefetch_needed(*pump->timeline)) ||
        (!pump->catchup_required && !pump->timeline->newer_refetch_required)) {
        return false;
    }
    *catchup = MessageHistoryCatchup{};
    catchup->incremental = !pump->timeline->follow_bottom;
    catchup->channel_id = channel_id;
    catchup->deferred_mark = pump->timeline->deferred;
    const MessageStore& store = *pump->timeline->store;
    const u32 resident_capacity = minimum(store.capacity, pump->timeline->list->capacity);
    if (resident_capacity == 0) {
        return false;
    }
    catchup->request_limit = minimum(request_limit, resident_capacity);
    for (u32 i = store.count; i != 0; --i) {
        const MessageRecord* last = ordered_record(store, i - 1);
        if (!last || last->channel_id != channel_id) {
            catchup->state = MessageHistoryCatchupState::failed;
            catchup->last_failure = MessageHistoryCatchupFailure::apply;
            return false;
        }
        if (last->delivery != MessageDeliveryState::authoritative) continue;
        catchup->cursor_after = last->id;
        break;
    }
    pump->catchup_required = true;
    pump->timeline->newer_refetch_required = true;
    catchup->state = MessageHistoryCatchupState::request_required;
    return true;
}

bool message_history_catchup_take_query(MessageHistoryCatchup* catchup,
                                        MessageHistoryQuery* query) noexcept {
    if (!catchup || !query || catchup->state != MessageHistoryCatchupState::request_required) {
        return false;
    }
    *query = MessageHistoryQuery{catchup->cursor_after, catchup->request_limit,
                                 catchup->cursor_after == 0 ? MessageHistoryPosition::latest
                                                                   : MessageHistoryPosition::after};
    catchup->state = MessageHistoryCatchupState::awaiting_page;
    ++catchup->requests;
    return true;
}

bool message_history_catchup_retry(MessageHistoryCatchup* catchup, MessageHistoryCatchupFailure failure,
                                   u64 retry_at_ms) noexcept {
    return page_retry<MessageHistoryCatchupState>(catchup, failure, retry_at_ms);
}

bool message_history_catchup_tick(MessageHistoryCatchup* catchup, u64 now_ms) noexcept {
    return page_tick<MessageHistoryCatchupState>(catchup, now_ms);
}

bool message_history_catchup_fail(MessageHistoryCatchup* catchup,
                                  MessageHistoryCatchupFailure failure) noexcept {
    return page_fail<MessageHistoryCatchupState>(catchup, failure);
}

MessageHistoryApplyResult message_history_catchup_apply_page(
    MessageHistoryCatchup* catchup, MessageEventPump* pump, const MessageHistoryPage& page,
    ConstSlice<GatewayMessageEvent> messages,
    MessageContentAvailability empty_content_availability) noexcept {
    if (!catchup || !pump || !pump->timeline ||
        catchup->state != MessageHistoryCatchupState::awaiting_page ||
        !page_matches_summary(page, messages) ||
        (messages.count != 0 && page.channel_id != catchup->channel_id)) {
        page_fail<MessageHistoryCatchupState>(catchup, MessageHistoryCatchupFailure::decode);
        return apply_error(MessageHistoryApplyError::invalid_page);
    }
    // Browsing forward extends the loaded window without jumping to its new
    // bottom, even if a wheel gesture reached the old edge during the request.
    if (catchup->incremental) pump->timeline->follow_bottom = false;
    catchup->latest_short = catchup->cursor_after == 0 && messages.count != 0 &&
                            messages.count < catchup->request_limit;
    const MessageHistoryApplyResult applied = message_timeline_apply_history_page(
        pump->timeline, messages, MessageHistoryDirection::newer, empty_content_availability);
    if (!applied.ok()) {
        page_fail<MessageHistoryCatchupState>(catchup, MessageHistoryCatchupFailure::apply);
        if (applied.destructive_failure) {
            pump->resync_required = true;
        }
        return applied;
    }
    ++catchup->pages;
    catchup->transient_retries = 0;
    catchup->received += messages.count;
    catchup->inserted += applied.inserted;
    catchup->evicted += applied.evicted;
    const bool deferred_since_basis = pump->timeline->deferred != catchup->deferred_mark;
    catchup->deferred_mark = pump->timeline->deferred;
    if ((messages.count == 0 || messages.count < catchup->request_limit) && deferred_since_basis) {
        // The tail moved while this page was in flight: its deferred rows are
        // newer than the page, so the gap is not closed yet.
        if (messages.count) catchup->cursor_after = page.newest_id;
        catchup->state = MessageHistoryCatchupState::request_required;
    } else if (messages.count == 0 || messages.count < catchup->request_limit) {
        catchup->state = MessageHistoryCatchupState::complete;
        pump->catchup_required = false;
        pump->resync_required = false;
        pump->timeline->newer_refetch_required = false;
    } else {
        catchup->cursor_after = page.newest_id;
        catchup->state = catchup->incremental ? MessageHistoryCatchupState::paused : MessageHistoryCatchupState::request_required;
        if (catchup->incremental) pump->catchup_required = false;
    }
    return applied;
}

bool message_history_older_prefetch_needed(const MessageTimeline& timeline) noexcept {
    return timeline.list && timeline.viewport_units != 0 && timeline.list->count != 0 &&
           timeline.list->total_units > timeline.viewport_units &&
           timeline.scroll_units <= ui_extent_units(500.0f);
}

bool message_history_newer_prefetch_needed(const MessageTimeline& timeline) noexcept {
    if (!timeline.newer_refetch_required || !timeline.list || !timeline.list->count || !timeline.viewport_units) return false;
    const u64 maximum = timeline.list->total_units > timeline.viewport_units ? timeline.list->total_units - timeline.viewport_units : 0;
    return maximum - minimum(maximum, timeline.scroll_units) <= ui_extent_units(500.0f);
}

bool message_history_older_demand(MessageHistoryOlder* older, const MessageTimeline* timeline,
                                  u64 channel_id, u32 request_limit) noexcept {
    if (!older || !timeline || !timeline->store || !timeline->list || channel_id == 0 ||
        request_limit == 0 || request_limit > kMessageHistoryMaximumMessages ||
        (older->channel_id != 0 && older->channel_id != channel_id)) {
        return false;
    }
    if (older->state == MessageHistoryOlderState::exhausted) {
        const auto* first = authoritative_edge(*timeline->store, false);
        if (first && older->cursor_before && first->id > older->cursor_before) older->state = MessageHistoryOlderState::idle;
    }
    if (older->state != MessageHistoryOlderState::idle) {
        return true;
    }
    const MessageStore& store = *timeline->store;
    const u32 resident_capacity = minimum(store.capacity, timeline->list->capacity);
    if (resident_capacity == 0 || store.count != timeline->list->count) {
        return false;
    }
    older->channel_id = channel_id;
    older->request_limit = minimum(request_limit, resident_capacity);
    ++older->demands;
    if (store.count == 0) {
        older->state = MessageHistoryOlderState::exhausted;
        return true;
    }
    const MessageRecord* first = authoritative_edge(store, false);
    if (!first) { older->state = MessageHistoryOlderState::idle; older->cursor_before = 0; return true; }
    if (first->channel_id != channel_id || first->id == 0) {
        older->state = MessageHistoryOlderState::failed;
        older->last_failure = MessageHistoryCatchupFailure::apply;
        ++older->failures;
        return false;
    }
    older->cursor_before = first->id;
    older->state = MessageHistoryOlderState::request_required;
    return true;
}

bool message_history_older_take_query(MessageHistoryOlder* older,
                                      MessageHistoryQuery* query) noexcept {
    if (!older || !query || older->cursor_before == 0 ||
        older->state != MessageHistoryOlderState::request_required) {
        return false;
    }
    *query = MessageHistoryQuery{older->cursor_before, older->request_limit,
                                 MessageHistoryPosition::before};
    older->state = MessageHistoryOlderState::awaiting_page;
    ++older->requests;
    return true;
}

bool message_history_older_retry(MessageHistoryOlder* older, MessageHistoryCatchupFailure failure,
                                 u64 retry_at_ms) noexcept {
    return page_retry<MessageHistoryOlderState>(older, failure, retry_at_ms);
}

bool message_history_older_tick(MessageHistoryOlder* older, u64 now_ms) noexcept {
    return page_tick<MessageHistoryOlderState>(older, now_ms);
}

bool message_history_older_fail(MessageHistoryOlder* older, MessageHistoryCatchupFailure failure) noexcept {
    return page_fail<MessageHistoryOlderState>(older, failure);
}

MessageHistoryApplyResult message_history_older_apply_page(
    MessageHistoryOlder* older, MessageTimeline* timeline, const MessageHistoryPage& page,
    ConstSlice<GatewayMessageEvent> messages,
    MessageContentAvailability empty_content_availability) noexcept {
    if (!older || !timeline || older->state != MessageHistoryOlderState::awaiting_page ||
        !page_matches_summary(page, messages) || messages.count > older->request_limit ||
        (messages.count != 0 &&
         (page.channel_id != older->channel_id || page.newest_id >= older->cursor_before))) {
        page_fail<MessageHistoryOlderState>(older, MessageHistoryCatchupFailure::decode);
        return apply_error(MessageHistoryApplyError::invalid_page);
    }
    const MessageHistoryApplyResult applied = message_timeline_apply_history_page(
        timeline, messages, MessageHistoryDirection::older, empty_content_availability);
    if (!applied.ok()) {
        page_fail<MessageHistoryOlderState>(older, MessageHistoryCatchupFailure::apply);
        return applied;
    }
    ++older->pages;
    older->transient_retries = 0;
    older->received += messages.count;
    older->inserted += applied.inserted;
    older->evicted += applied.evicted;
    // Older paging is a sliding resident window. If the tail was evicted,
    // remember that the resident newest edge is incomplete. The catch-up
    // scheduler waits until the user returns to the bottom before refilling
    // that edge, so scrolling backward never permanently loses newer rows.
    if (applied.evicted != 0) {
        timeline->newer_refetch_required = true;
    }
    if (messages.count == 0 || messages.count < older->request_limit) {
        if (messages.count) older->cursor_before = page.oldest_id;
        older->state = MessageHistoryOlderState::exhausted;
    } else {
        older->cursor_before = page.oldest_id;
        older->state = MessageHistoryOlderState::idle;
    }
    return applied;
}

MessageHistoryRestResult message_history_catchup_accept_rest(
    MessageHistoryCatchup* catchup, MessageEventPump* pump, const RestClientResponse& response,
    Utf8View body, const MessageHistoryDecodeScratch& scratch,
    MessageContentAvailability empty_content_availability, u64 now_ms) noexcept {
    if (!catchup || !pump || !pump->timeline ||
        catchup->state != MessageHistoryCatchupState::awaiting_page || (!body.data && body.bytes != 0) ||
        response.body_bytes != body.bytes) {
        MessageHistoryRestResult invalid{};
        invalid.disposition = MessageHistoryRestDisposition::failed;
        invalid.error = MessageHistoryRestError::invalid_input;
        return invalid;
    }
    return accept_page_rest<MessageHistoryCatchupState>(
        catchup, response, scratch, now_ms, pump->users, pump->members, pump->state, pump->timeline,
        [&](const MessageHistoryPage& page, ConstSlice<GatewayMessageEvent> messages) noexcept {
            return message_history_catchup_apply_page(catchup, pump, page, messages, empty_content_availability);
        },
        body);
}

MessageHistoryRestResult message_history_older_accept_rest(
    MessageHistoryOlder* older, MessageTimeline* timeline, UserStore* users,
    ChatStateStore* state, GuildMemberStore* members, const RestClientResponse& response,
    Utf8View body, const MessageHistoryDecodeScratch& scratch,
    MessageContentAvailability empty_content_availability, u64 now_ms) noexcept {
    if (!older || !timeline || older->state != MessageHistoryOlderState::awaiting_page ||
        (!body.data && body.bytes != 0) || response.body_bytes != body.bytes) {
        MessageHistoryRestResult invalid{};
        invalid.disposition = MessageHistoryRestDisposition::failed;
        invalid.error = MessageHistoryRestError::invalid_input;
        return invalid;
    }
    return accept_page_rest<MessageHistoryOlderState>(
        older, response, scratch, now_ms, users, members, state, timeline,
        [&](const MessageHistoryPage& page, ConstSlice<GatewayMessageEvent> messages) noexcept {
            return message_history_older_apply_page(older, timeline, page, messages, empty_content_availability);
        },
        body);
}

} // namespace maicord
