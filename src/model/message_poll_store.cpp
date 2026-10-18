#include "maicord/model/message_poll_store.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
constexpr u32 kKnownFields = gateway_message_poll_field_question | gateway_message_poll_field_answers |
                             gateway_message_poll_field_expiry | gateway_message_poll_field_multiselect |
                             gateway_message_poll_field_layout | gateway_message_poll_field_results;
constexpr u32 kVoteObservations = 128;
struct VoteObservation {
    u64 message_id;
    u64 user_id;
    u32 answer_id;
    bool voted;
};
} // namespace
struct MessagePollOwnedStorage {
    StableStorage records;
    Arena text_banks[2];
    VoteObservation observations[kVoteObservations];
    u32 next_observation;
    u8 active_bank;
};
static_assert(sizeof(MessagePollOwnedStorage) <= 4096);
static_assert(sizeof(MessagePollRecord) <= 512);
namespace {
u32 position(const MessagePollStore& store, u64 id) noexcept {
    u32 first = 0, last = store.count;
    while (first < last) {
        const u32 middle = first + (last - first) / 2;
        if (store.records[middle].message_id < id)
            first = middle + 1;
        else
            last = middle;
    }
    return first;
}
MessagePollRecord* find(MessagePollStore& store, u64 id) noexcept {
    return const_cast<MessagePollRecord*>(message_poll_store_find(store, id));
}
MessagePollAnswer* answer(MessagePollRecord& poll, u32 id) noexcept {
    return const_cast<MessagePollAnswer*>(message_poll_record_answer(poll, id));
}
void advance(u32& revision) noexcept {
    if (!++revision)
        ++revision;
}
MessagePollStoreResult failure(MessagePollStore* store, MessagePollStoreError error) noexcept {
    if (store)
        saturating_increment(store->rejected);
    return {nullptr, error, false, false};
}
bool valid_text(Utf8View value, u32 maximum_bytes, u32 maximum_scalars) noexcept {
    if (value.bytes > maximum_bytes || (value.bytes && !value.data))
        return false;
    const auto valid = utf8_validate(value);
    return valid.valid && valid.codepoints <= maximum_scalars;
}
bool same_string(const MessagePollStore& store, MessagePollString before, Utf8View after) noexcept {
    const auto value = message_poll_store_string(store, before);
    return value.bytes == after.bytes && bytes_equal(value.data, after.data, after.bytes);
}
u32 string_bytes(const MessagePollRecord& poll) noexcept {
    u32 bytes = poll.question.bytes;
    for (u32 i = 0; i < poll.answer_count; ++i)
        bytes += poll.answers[i].text.bytes + poll.answers[i].emoji_name.bytes;
    return bytes;
}
void clear_observations(MessagePollStore& store, u64 id) noexcept {
    if (!store.owned)
        return;
    for (auto& item : store.owned->observations)
        if (item.message_id == id)
            item = {};
}
bool ensure_storage(MessagePollStore& store, u32 required) noexcept {
    if (!store.owned) {
        auto* storage = static_cast<MessagePollOwnedStorage*>(VirtualAlloc(
            nullptr, sizeof(MessagePollOwnedStorage), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!storage)
            return false;
        if (!stable_storage_initialize(&storage->records, store.capacity, 0, sizeof(MessagePollRecord),
                                       alignof(MessagePollRecord)) ||
            !arena_initialize(storage->text_banks, store.text_capacity, 0, 4096)) {
            stable_storage_release(&storage->records);
            arena_release(storage->text_banks);
            VirtualFree(storage, 0, MEM_RELEASE);
            return false;
        }
        store.owned = storage;
        store.records = stable_storage_data<MessagePollRecord>(storage->records);
        store.text = storage->text_banks[0].base;
    }
    return stable_storage_reserve(&store.owned->records, required);
}
MessagePollString write_string(MessagePollStore& store, Utf8View text) noexcept {
    if (!text.bytes)
        return {};
    const MessagePollString result{store.text_used, static_cast<u16>(text.bytes), 0};
    copy_bytes(store.text + store.text_used, text.data, text.bytes);
    store.text_used += text.bytes;
    return result;
}
bool prepare_text(MessagePollStore& store, u32 required, u64 replaced_id) noexcept {
    auto& storage = *store.owned;
    auto& active = storage.text_banks[storage.active_bank];
    if (required <= store.text_capacity - store.text_used)
        return !required || arena_allocate(&active, required, 1) != nullptr;
    u32 live = 0;
    for (u32 i = 0; i < store.count; ++i)
        if (store.records[i].message_id != replaced_id)
            live += string_bytes(store.records[i]);
    if (live > store.text_capacity || required > store.text_capacity - live)
        return false;
    auto& next = storage.text_banks[storage.active_bank ^ 1];
    if (!next.base && !arena_initialize(&next, store.text_capacity, 0, 4096))
        return false;
    arena_reset(&next);
    if (live + required && !arena_allocate(&next, live + required, 1))
        return false;
    const auto copy_string = [&](MessagePollString& value, u32& used) noexcept {
        if (!value.bytes) {
            value = {};
            return;
        }
        copy_bytes(next.base + used, store.text + value.offset, value.bytes);
        value.offset = used;
        used += value.bytes;
    };
    u32 used = 0;
    for (u32 i = 0; i < store.count; ++i) {
        auto& poll = store.records[i];
        if (poll.message_id == replaced_id)
            continue;
        copy_string(poll.question, used);
        for (u32 a = 0; a < poll.answer_count; ++a) {
            copy_string(poll.answers[a].text, used);
            copy_string(poll.answers[a].emoji_name, used);
        }
    }
    storage.active_bank ^= 1;
    store.text = next.base;
    store.text_used = used;
    store.text_wasted = 0;
    saturating_increment(store.compactions);
    return true;
}
VoteObservation* observation(MessagePollStore& store, u64 message, u64 user, u32 answer_id) noexcept {
    for (auto& item : store.owned->observations)
        if (item.message_id == message && item.user_id == user && item.answer_id == answer_id)
            return &item;
    return nullptr;
}
VoteObservation* admit_observation(MessagePollStore& store) noexcept {
    auto& storage = *store.owned;
    for (u32 i = 0; i < kVoteObservations; ++i) {
        const u32 at = (storage.next_observation + i) % kVoteObservations;
        if (!storage.observations[at].message_id) {
            storage.next_observation = (at + 1) % kVoteObservations;
            return storage.observations + at;
        }
    }
    auto& evicted = storage.observations[storage.next_observation];
    if (auto* poll = find(store, evicted.message_id)) {
        if (auto* value = answer(*poll, evicted.answer_id); value && value->count_known) {
            value->count_known = false;
            poll->results_known = false;
            advance(poll->result_revision);
            advance(store.revision);
        }
    }
    storage.next_observation = (storage.next_observation + 1) % kVoteObservations;
    return &evicted;
}
} // namespace

bool message_poll_store_initialize_owned(MessagePollStore* store, u32 capacity, u32 text_bytes) noexcept {
    if (!store || store->owned || !capacity || capacity > 65535 || !text_bytes ||
        text_bytes > 16 * 1024 * 1024)
        return false;
    *store = {};
    store->capacity = capacity;
    store->text_capacity = text_bytes;
    return true;
}
void message_poll_store_release(MessagePollStore* store) noexcept {
    if (!store)
        return;
    if (store->owned) {
        stable_storage_release(&store->owned->records);
        for (auto& bank : store->owned->text_banks)
            arena_release(&bank);
        VirtualFree(store->owned, 0, MEM_RELEASE);
    }
    *store = {};
}
const MessagePollRecord* message_poll_store_find(const MessagePollStore& store, u64 id) noexcept {
    if (!id || !store.records)
        return nullptr;
    const u32 at = position(store, id);
    return at < store.count && store.records[at].message_id == id ? store.records + at : nullptr;
}
Utf8View message_poll_store_string(const MessagePollStore& store, MessagePollString value) noexcept {
    if (!value.bytes)
        return {};
    if (!store.text || value.reserved || value.offset > store.text_used ||
        value.bytes > store.text_used - value.offset)
        return {};
    return {reinterpret_cast<const char*>(store.text + value.offset), value.bytes};
}
const MessagePollAnswer* message_poll_record_answer(const MessagePollRecord& poll, u32 id) noexcept {
    for (u32 i = 0; i < poll.answer_count; ++i)
        if (poll.answers[i].id == id)
            return poll.answers + i;
    return nullptr;
}
MessagePollResultTicket message_poll_store_result_ticket(const MessagePollStore& store, u64 id,
                                                         u64 serial) noexcept {
    const auto* poll = message_poll_store_find(store, id);
    return {id, serial, poll ? poll->content_revision : 0};
}

MessagePollStoreResult message_poll_store_apply(MessagePollStore* store, u64 id, u64 channel,
                                                const GatewayMessagePoll& input, u64 serial,
                                                const MessagePollResultTicket* ticket) noexcept {
    if (!store || !store->capacity || !id || !channel || (input.fields & ~kKnownFields))
        return failure(store, MessagePollStoreError::invalid_input);
    const auto* old = message_poll_store_find(*store, id);
    if ((old && old->channel_id != channel) ||
        (ticket &&
         (ticket->message_id != id || ticket->content_revision != (old ? old->content_revision : 0))))
        return failure(store, MessagePollStoreError::stale);
    if (serial && old && serial <= old->last_gateway_serial)
        return {old, MessagePollStoreError::stale, false, false};
    if (!input.present) {
        if (ticket && old && old->last_gateway_serial > ticket->gateway_serial)
            return {old, MessagePollStoreError::stale, false, false};
        const bool changed = message_poll_store_delete(store, id);
        return {nullptr, MessagePollStoreError::none, changed, false};
    }
    if (!old &&
        (input.fields & (gateway_message_poll_field_question | gateway_message_poll_field_answers)) !=
            (gateway_message_poll_field_question | gateway_message_poll_field_answers))
        return failure(store, MessagePollStoreError::incomplete);
    if ((input.fields & gateway_message_poll_field_question) &&
        !valid_text(input.question, kGatewayMessagePollQuestionMaximumBytes,
                    kGatewayMessagePollQuestionMaximumScalars))
        return failure(store, MessagePollStoreError::invalid_utf8);
    if ((input.fields & gateway_message_poll_field_answers) &&
        (!input.answers.data || !input.answers.count ||
         input.answers.count > kGatewayMessagePollMaximumAnswers))
        return failure(store, MessagePollStoreError::invalid_input);
    if ((input.fields & gateway_message_poll_field_results) &&
        (!input.results_known || input.answer_counts.count > kGatewayMessagePollMaximumAnswers ||
         (input.answer_counts.count && !input.answer_counts.data)))
        return failure(store, MessagePollStoreError::invalid_input);

    MessagePollRecord next = old ? *old : MessagePollRecord{};
    next.message_id = id;
    next.channel_id = channel;
    Utf8View question = old ? message_poll_store_string(*store, old->question) : Utf8View{};
    Utf8View texts[kGatewayMessagePollMaximumAnswers]{}, emojis[kGatewayMessagePollMaximumAnswers]{};
    if (input.fields & gateway_message_poll_field_question)
        question = input.question;
    bool content_changed = !old;
    if (old && !same_string(*store, old->question, question))
        content_changed = true;
    if (input.fields & gateway_message_poll_field_answers) {
        next.answer_count = static_cast<u8>(input.answers.count);
        for (u32 i = 0; i < input.answers.count; ++i) {
            const auto& source = input.answers[i];
            if (!source.answer_id)
                return failure(store, MessagePollStoreError::invalid_input);
            if (!valid_text(source.text, kGatewayMessagePollAnswerMaximumBytes,
                            kGatewayMessagePollAnswerMaximumScalars) ||
                !valid_text(source.emoji_name, kGatewayMessagePollEmojiMaximumBytes,
                            kGatewayMessagePollEmojiMaximumBytes))
                return failure(store, MessagePollStoreError::invalid_utf8);
            for (u32 j = 0; j < i; ++j)
                if (input.answers[j].answer_id == source.answer_id)
                    return failure(store, MessagePollStoreError::invalid_input);
            const auto* prior = old ? message_poll_record_answer(*old, source.answer_id) : nullptr;
            next.answers[i] = prior ? *prior : MessagePollAnswer{};
            next.answers[i].id = source.answer_id;
            next.answers[i].emoji_id = source.emoji_id;
            next.answers[i].emoji_animated = source.emoji_animated;
            texts[i] = source.text;
            emojis[i] = source.emoji_name;
            if (!old || i >= old->answer_count || old->answers[i].id != source.answer_id ||
                old->answers[i].emoji_id != source.emoji_id ||
                old->answers[i].emoji_animated != source.emoji_animated ||
                !same_string(*store, old->answers[i].text, source.text) ||
                !same_string(*store, old->answers[i].emoji_name, source.emoji_name))
                content_changed = true;
        }
        if (old && old->answer_count != next.answer_count)
            content_changed = true;
        for (u32 i = next.answer_count; i < kGatewayMessagePollMaximumAnswers; ++i)
            next.answers[i] = {};
    } else {
        for (u32 i = 0; i < next.answer_count; ++i) {
            texts[i] = message_poll_store_string(*store, next.answers[i].text);
            emojis[i] = message_poll_store_string(*store, next.answers[i].emoji_name);
        }
    }
    const auto apply_scalar = [&](u32 field, auto& target, const auto& value) noexcept {
        if (!(input.fields & field))
            return;
        if (!(next.fields & field) || target != value)
            content_changed = true;
        target = value;
    };
    apply_scalar(gateway_message_poll_field_expiry, next.expiry_ms, input.expiry_ms);
    apply_scalar(gateway_message_poll_field_expiry, next.expiry_null, input.expiry_null);
    apply_scalar(gateway_message_poll_field_multiselect, next.allow_multiselect, input.allow_multiselect);
    apply_scalar(gateway_message_poll_field_layout, next.layout_type, input.layout_type);
    const bool ignore_results = ticket && old && old->last_gateway_serial > ticket->gateway_serial;
    if (input.fields & gateway_message_poll_field_results) {
        for (u32 i = 0; i < input.answer_counts.count; ++i) {
            if (!answer(next, input.answer_counts[i].answer_id))
                return failure(store, MessagePollStoreError::invalid_input);
            for (u32 j = 0; j < i; ++j)
                if (input.answer_counts[j].answer_id == input.answer_counts[i].answer_id)
                    return failure(store, MessagePollStoreError::invalid_input);
        }
        if (!ignore_results) {
            if (old && (old->fields & gateway_message_poll_field_results) && old->is_finalized &&
                !input.is_finalized)
                return failure(store, MessagePollStoreError::stale);
            for (u32 i = 0; i < next.answer_count; ++i) {
                auto& target = next.answers[i];
                target.count = 0;
                target.me_voted = false;
                target.count_known = target.me_known = true;
            }
            for (u32 i = 0; i < input.answer_counts.count; ++i) {
                auto& target = *answer(next, input.answer_counts[i].answer_id);
                target.count = input.answer_counts[i].count;
                target.me_voted = input.answer_counts[i].me_voted;
            }
            next.results_known = true;
            next.is_finalized = input.is_finalized;
        }
    }
    next.fields |= input.fields & (ignore_results ? ~gateway_message_poll_field_results : ~u32{0});
    // Omitted results leave new answers unknown and retain previously observed
    // counts and own votes. Only an explicit results object establishes zeroes.
    for (u32 i = 0; i < next.answer_count; ++i)
        if (!next.answers[i].count_known)
            next.results_known = false;
    if (serial)
        next.last_gateway_serial = serial;
    bool results_changed =
        !old || old->results_known != next.results_known || old->is_finalized != next.is_finalized;
    if (old)
        for (u32 i = 0; i < next.answer_count; ++i) {
            const auto* prior = message_poll_record_answer(*old, next.answers[i].id);
            const auto& value = next.answers[i];
            if (!prior || prior->count != value.count || prior->count_known != value.count_known ||
                prior->me_voted != value.me_voted || prior->me_known != value.me_known)
                results_changed = true;
        }
    if (old && !content_changed && !results_changed) {
        if (serial)
            find(*store, id)->last_gateway_serial = serial;
        if ((input.fields & gateway_message_poll_field_results) && !ignore_results)
            clear_observations(*store, id);
        return {old, MessagePollStoreError::none, false,
                ignore_results && (input.fields & gateway_message_poll_field_results)};
    }
    if (!old && store->count == store->capacity)
        return failure(store, MessagePollStoreError::capacity);
    u32 bytes = question.bytes;
    for (u32 i = 0; i < next.answer_count; ++i)
        bytes += texts[i].bytes + emojis[i].bytes;
    if (content_changed && bytes > store->text_capacity)
        return failure(store, MessagePollStoreError::capacity);
    const u32 old_bytes = old ? string_bytes(*old) : 0;
    const u32 at = position(*store, id);
    if (!ensure_storage(*store, store->count + (old ? 0 : 1)))
        return failure(store, MessagePollStoreError::capacity);
    const u32 before_compactions = store->compactions;
    if (content_changed) {
        if (!prepare_text(*store, bytes, id))
            return failure(store, MessagePollStoreError::capacity);
        if (store->compactions == before_compactions)
            store->text_wasted += old_bytes;
        next.question = write_string(*store, question);
        for (u32 i = 0; i < next.answer_count; ++i) {
            next.answers[i].text = write_string(*store, texts[i]);
            next.answers[i].emoji_name = write_string(*store, emojis[i]);
        }
        advance(store->content_revision);
        next.content_revision = store->content_revision;
    }
    if (results_changed)
        advance(next.result_revision);
    if (!old) {
        for (u32 i = store->count; i > at; --i)
            store->records[i] = store->records[i - 1];
        ++store->count;
    }
    store->records[at] = next;
    if ((input.fields & gateway_message_poll_field_results) && !ignore_results)
        clear_observations(*store, id);
    else if (content_changed)
        for (auto& item : store->owned->observations)
            if (item.message_id == id && !answer(next, item.answer_id))
                item = {};
    if (content_changed || results_changed)
        advance(store->revision);
    return {store->records + at, MessagePollStoreError::none, content_changed || results_changed,
            ignore_results && (input.fields & gateway_message_poll_field_results)};
}

MessagePollStoreResult message_poll_store_vote(MessagePollStore* store,
                                               const GatewayMessagePollVoteEvent& event, u64 actor,
                                               u64 serial) noexcept {
    if (!store || !event.message_id || !event.channel_id || !event.user_id || !event.answer_id || !actor ||
        !serial)
        return failure(store, MessagePollStoreError::invalid_input);
    auto* poll = find(*store, event.message_id);
    if (!poll)
        return {nullptr, MessagePollStoreError::incomplete, false, false};
    if (poll->channel_id != event.channel_id || serial <= poll->last_gateway_serial)
        return {poll, MessagePollStoreError::stale, false, false};
    auto* value = answer(*poll, event.answer_id);
    if (!value)
        return failure(store, MessagePollStoreError::invalid_input);
    poll->last_gateway_serial = serial;
    if ((poll->fields & gateway_message_poll_field_results) && poll->is_finalized)
        return {poll, MessagePollStoreError::none, false, false};
    auto* observed = observation(*store, event.message_id, event.user_id, event.answer_id);
    const bool self = event.user_id == actor;
    const bool duplicate =
        observed ? observed->voted == event.add : self && value->me_known && value->me_voted == event.add;
    const u32 before_eviction = store->revision;
    if (!observed)
        observed = admit_observation(*store);
    *observed = {event.message_id, event.user_id, event.answer_id, event.add};
    bool changed = false;
    if (!duplicate && value->count_known) {
        if ((event.add && value->count == ~u32{0}) || (!event.add && !value->count)) {
            value->count_known = false;
            poll->results_known = false;
        } else if (event.add)
            ++value->count;
        else
            --value->count;
        changed = true;
    }
    if (self && (!value->me_known || value->me_voted != event.add)) {
        value->me_known = true;
        value->me_voted = event.add;
        changed = true;
    }
    if (changed) {
        advance(poll->result_revision);
        advance(store->revision);
    }
    return {poll, MessagePollStoreError::none, changed || store->revision != before_eviction, false};
}
MessagePollStoreResult message_poll_store_own_vote(MessagePollStore* store, u64 message_id, u64 channel_id,
                                                   u64 actor, u16 mask) noexcept {
    if (!store || !message_id || !channel_id || !actor)
        return failure(store, MessagePollStoreError::invalid_input);
    auto* poll = find(*store, message_id);
    if (!poll)
        return {nullptr, MessagePollStoreError::incomplete, false, false};
    if (poll->channel_id != channel_id || (mask >> poll->answer_count))
        return failure(store, MessagePollStoreError::invalid_input);
    if ((poll->fields & gateway_message_poll_field_results) && poll->is_finalized)
        return {poll, MessagePollStoreError::none, false, false};
    bool changed = false;
    for (u32 i = 0; i < poll->answer_count; ++i) {
        auto& value = poll->answers[i];
        const bool want = ((mask >> i) & 1u) != 0;
        if (value.me_known && value.me_voted == want)
            continue;
        if (value.me_known && value.count_known) {
            if (want && value.count != ~u32{0})
                ++value.count;
            else if (!want && value.count)
                --value.count;
            else {
                value.count_known = false;
                poll->results_known = false;
            }
        }
        value.me_known = true;
        value.me_voted = want;
        changed = true;
    }
    // Earlier own vote events are superseded: their echo dedupe follows the
    // accepted state.
    if (store->owned)
        for (auto& item : store->owned->observations)
            for (u32 i = 0; i < poll->answer_count; ++i)
                if (item.message_id == message_id && item.user_id == actor && item.answer_id == poll->answers[i].id)
                    item.voted = ((mask >> i) & 1u) != 0;
    if (changed) {
        advance(poll->result_revision);
        advance(store->revision);
    }
    return {poll, MessagePollStoreError::none, changed, false};
}
bool message_poll_store_delete(MessagePollStore* store, u64 id) noexcept {
    const auto* poll = store ? message_poll_store_find(*store, id) : nullptr;
    if (!poll)
        return false;
    const u32 at = static_cast<u32>(poll - store->records);
    store->text_wasted += string_bytes(store->records[at]);
    clear_observations(*store, id);
    for (u32 i = at + 1; i < store->count; ++i)
        store->records[i - 1] = store->records[i];
    --store->count;
    advance(store->revision);
    advance(store->content_revision);
    return true;
}
MessagePollStoreMetrics message_poll_store_metrics(const MessagePollStore& store) noexcept {
    MessagePollStoreMetrics result{};
    result.records = store.count;
    result.text_bytes = store.text_used;
    if (!store.owned)
        return result;
    Win32MemoryInformation owner{};
    if (VirtualQuery(store.owned, owner.native(), sizeof(owner))) {
        result.reserved_bytes = result.committed_bytes = owner.RegionSize;
        result.commit_count = 1;
    }
    const auto add = [&](const Arena& bank) noexcept {
        result.reserved_bytes += bank.reserved;
        result.committed_bytes += bank.committed;
        result.commit_count += bank.commit_count;
    };
    add(store.owned->records.arena);
    for (const auto& bank : store.owned->text_banks)
        add(bank);
    return result;
}
MessagePollPageResult message_poll_store_apply_page(MessagePollStore* store,
                                                    ConstSlice<MessagePollPageInput> inputs) noexcept {
    if (!store || inputs.count > kMessagePollPageMaximum || (inputs.count && !inputs.data))
        return {MessagePollStoreError::invalid_input, 0, 0, false};
    if (!inputs.count)
        return {};
    MessagePollStore candidate{};
    if (!message_poll_store_initialize_owned(&candidate, store->capacity, store->text_capacity))
        return {MessagePollStoreError::invalid_input, 0, 0, false};
    struct Release {
        MessagePollStore& store;
        ~Release() noexcept {
            message_poll_store_release(&store);
        }
    } release{candidate};
    MessagePollPageResult result{};
    bool cloned = false;
    bool publish = false;
    for (u32 i = 0; i < inputs.count; ++i) {
        const auto& input = inputs[i];
        if (!input.poll || !input.message_id || !input.channel_id)
            return {MessagePollStoreError::invalid_input, 0, result.ignored, false};
        const auto* resident = message_poll_store_find(*store, input.message_id);
        if ((resident && resident->channel_id != input.channel_id) ||
            (input.ticket &&
             (input.ticket->message_id != input.message_id ||
              input.ticket->content_revision != (resident ? resident->content_revision : 0))) ||
            (input.gateway_serial && resident && input.gateway_serial <= resident->last_gateway_serial)) {
            ++result.ignored;
            continue;
        }
        if (!cloned) {
            candidate.revision = store->revision;
            candidate.content_revision = store->content_revision;
            candidate.rejected = store->rejected;
            candidate.compactions = store->compactions;
            if (store->count) {
                if (!ensure_storage(candidate, store->count))
                    return {MessagePollStoreError::capacity, 0, result.ignored, false};
                u32 live_bytes = 0;
                for (u32 p = 0; p < store->count; ++p)
                    live_bytes += string_bytes(store->records[p]);
                if (!prepare_text(candidate, live_bytes, 0))
                    return {MessagePollStoreError::capacity, 0, result.ignored, false};
                for (u32 p = 0; p < store->count; ++p) {
                    auto copy = store->records[p];
                    copy.question =
                        write_string(candidate, message_poll_store_string(*store, copy.question));
                    for (u32 a = 0; a < copy.answer_count; ++a) {
                        copy.answers[a].text = write_string(
                            candidate, message_poll_store_string(*store, copy.answers[a].text));
                        copy.answers[a].emoji_name = write_string(
                            candidate, message_poll_store_string(*store, copy.answers[a].emoji_name));
                    }
                    candidate.records[p] = copy;
                }
                candidate.count = store->count;
                copy_bytes(candidate.owned->observations, store->owned->observations,
                           sizeof(candidate.owned->observations));
                candidate.owned->next_observation = store->owned->next_observation;
            }
            cloned = true;
        }
        const auto applied = message_poll_store_apply(&candidate, input.message_id, input.channel_id,
                                                      *input.poll, input.gateway_serial, input.ticket);
        if (applied.error == MessagePollStoreError::stale) {
            ++result.ignored;
            continue;
        }
        if (!applied.ok())
            return {applied.error, 0, result.ignored, false};
        ++result.applied;
        if (applied.results_ignored)
            ++result.ignored;
        publish |= applied.changed || input.gateway_serial != 0 ||
                   ((input.poll->fields & gateway_message_poll_field_results) && !applied.results_ignored);
    }
    if (!publish)
        return result;
    // Every consumer that borrows model text must see this epoch even when the
    // accepted page changes only result data and preserves per-poll versions.
    advance(candidate.revision);
    advance(candidate.content_revision);
    auto retired = *store;
    *store = candidate;
    candidate = {};
    message_poll_store_release(&retired);
    result.changed = true;
    return result;
}
} // namespace maicord
