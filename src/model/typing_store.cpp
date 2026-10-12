#include "maicord/model/typing_store.hpp"

namespace maicord {
namespace {

TypingStoreResult result(TypingStoreError error, bool changed = false,
                                bool accepted = false) noexcept {
    return TypingStoreResult{error, changed, accepted};
}

bool store_valid(const TypingStore& store) noexcept {
    return store.records.data && store.records.count != 0 &&
           store.records.count <= kTypingRecordCapacity && store.count <= store.records.count;
}

u32 find_record(const TypingStore& store, u64 channel_id, u64 user_id) noexcept {
    for (u32 index = 0; index < store.count; ++index) {
        const TypingRecord& record = store.records[index];
        if (record.channel_id == channel_id && record.user_id == user_id) {
            return index;
        }
    }
    return ~0u;
}

void remove_record(TypingStore* store, u32 index) noexcept {
    if (!store || index >= store->count) {
        return;
    }
    for (u32 cursor = index + 1; cursor < store->count; ++cursor) {
        store->records[cursor - 1] = store->records[cursor];
    }
    --store->count;
    store->records[store->count] = TypingRecord{};
}

void clear_local_controller(TypingStore* store) noexcept {
    store->local_channel_id = 0;
    store->local_user_id = 0;
    store->local_previous_send_ms = 0;
    store->local_post_at_ms = 0;
    store->local_post_pending = false;
}

} // namespace

bool typing_store_initialize(TypingStore* store,
                                     Slice<TypingRecord> records) noexcept {
    if (!store || !records.data || records.count == 0 || records.count > kTypingRecordCapacity) {
        return false;
    }
    *store = TypingStore{};
    store->records = records;
    for (u32 index = 0; index < records.count; ++index) {
        records[index] = TypingRecord{};
    }
    return true;
}

bool typing_store_reset(TypingStore* store) noexcept {
    if (!store || !store_valid(*store)) {
        return false;
    }
    const bool changed = store->count != 0;
    for (u32 index = 0; index < store->count; ++index) {
        store->records[index] = TypingRecord{};
    }
    store->count = 0;
    clear_local_controller(store);
    saturating_increment_if(changed, store->revision);
    return true;
}

TypingStoreResult typing_store_start(TypingStore* store, u64 channel_id, u64 guild_id,
                                                    u64 user_id, u64 now_ms) noexcept {
    if (!store || !store_valid(*store) || channel_id == 0 || user_id == 0 ||
        now_ms > ~0ull - kTypingLifetimeMs) {
        return result(TypingStoreError::invalid_input);
    }
    const u64 expires_at = now_ms + kTypingLifetimeMs;
    const u32 existing = find_record(*store, channel_id, user_id);
    if (existing != ~0u) {
        TypingRecord& record = store->records[existing];
        record.guild_id = guild_id;
        record.expires_at_ms = expires_at;
        return result(TypingStoreError::none, false, true);
    }
    if (store->count == store->records.count) {
        saturating_increment(store->dropped);
        return result(TypingStoreError::capacity);
    }
    store->records[store->count++] = TypingRecord{channel_id, guild_id, user_id, expires_at};
    saturating_increment(store->revision);
    return result(TypingStoreError::none, true, true);
}

TypingStoreResult typing_store_stop(TypingStore* store, u64 channel_id,
                                                   u64 user_id) noexcept {
    if (!store || !store_valid(*store) || channel_id == 0 || user_id == 0) {
        return result(TypingStoreError::invalid_input);
    }
    const u32 existing = find_record(*store, channel_id, user_id);
    if (existing == ~0u) {
        return result(TypingStoreError::none);
    }
    remove_record(store, existing);
    saturating_increment(store->revision);
    return result(TypingStoreError::none, true, true);
}

TypingStoreResult typing_store_note_local_input(TypingStore* store, u64 channel_id,
                                                               u64 guild_id, u64 user_id,
                                                               u64 now_ms) noexcept {
    if (!store || !store_valid(*store) || channel_id == 0 || user_id == 0) {
        return result(TypingStoreError::invalid_input);
    }
    if (store->local_channel_id != 0 && store->local_channel_id != channel_id) {
        clear_local_controller(store);
    }
    if (store->local_channel_id != 0 &&
        (store->local_post_pending ||
         (now_ms >= store->local_previous_send_ms &&
          now_ms - store->local_previous_send_ms < kTypingLocalRefreshMs))) {
        return result(TypingStoreError::none);
    }
    const bool had_local = store->local_channel_id != 0;
    const bool recent =
        had_local && (now_ms < 2u * kTypingLocalRefreshMs ||
                      store->local_previous_send_ms > now_ms - 2u * kTypingLocalRefreshMs);
    const u64 delay = !had_local || recent ? kTypingLocalDelayMs : 0;
    if (now_ms > ~0ull - delay) {
        return result(TypingStoreError::invalid_input);
    }
    store->local_channel_id = channel_id;
    store->local_user_id = user_id;
    store->local_previous_send_ms = now_ms;
    store->local_post_at_ms = now_ms + delay;
    store->local_post_pending = true;
    const TypingStoreResult started =
        typing_store_start(store, channel_id, guild_id, user_id, now_ms);
    if (!started.ok()) {
        clear_local_controller(store);
        return started;
    }
    return result(TypingStoreError::none, started.changed, true);
}

TypingStoreResult typing_store_stop_local(TypingStore* store,
                                                         u64 channel_id) noexcept {
    if (!store || !store_valid(*store) || channel_id == 0) {
        return result(TypingStoreError::invalid_input);
    }
    if (!store->local_post_pending || store->local_channel_id != channel_id || store->local_user_id == 0) {
        return result(TypingStoreError::none);
    }
    const u64 user_id = store->local_user_id;
    clear_local_controller(store);
    return typing_store_stop(store, channel_id, user_id);
}

TypingStoreResult typing_store_note_message(TypingStore* store, u64 channel_id,
                                                           u64 author_id, bool optimistic) noexcept {
    if (!store || !store_valid(*store) || channel_id == 0 || author_id == 0) {
        return result(TypingStoreError::invalid_input);
    }
    if (optimistic && store->local_channel_id == channel_id) {
        clear_local_controller(store);
    }
    return typing_store_stop(store, channel_id, author_id);
}

TypingStoreResult typing_store_tick(TypingStore* store, u64 now_ms) noexcept {
    if (!store || !store_valid(*store)) {
        return result(TypingStoreError::invalid_input);
    }
    bool changed = false;
    for (u32 index = 0; index < store->count;) {
        if (store->records[index].expires_at_ms <= now_ms) {
            remove_record(store, index);
            changed = true;
        } else {
            ++index;
        }
    }
    saturating_increment_if(changed, store->revision);
    return result(TypingStoreError::none, changed, changed);
}

TypingPost typing_store_take_local_post(TypingStore* store, u64 now_ms) noexcept {
    if (!store || !store_valid(*store) || !store->local_post_pending || store->local_channel_id == 0 ||
        now_ms < store->local_post_at_ms) {
        return TypingPost{};
    }
    const u64 channel_id = store->local_channel_id;
    store->local_post_pending = false;
    store->local_post_at_ms = 0;
    const bool suppressed = typing_store_channel_count(*store, channel_id) > 5u;
    return TypingPost{suppressed ? 0 : channel_id, true, suppressed};
}

u64 typing_store_next_wake_at(const TypingStore& store) noexcept {
    if (!store_valid(store)) {
        return 0;
    }
    u64 wake = store.local_post_pending ? store.local_post_at_ms : 0;
    for (u32 index = 0; index < store.count; ++index) {
        const u64 expiry = store.records[index].expires_at_ms;
        if (expiry != 0 && (wake == 0 || expiry < wake)) {
            wake = expiry;
        }
    }
    return wake;
}

u32 typing_store_channel_count(const TypingStore& store, u64 channel_id,
                                       u64 excluded_user_id) noexcept {
    if (!store_valid(store) || channel_id == 0) {
        return 0;
    }
    u32 count = 0;
    for (u32 index = 0; index < store.count; ++index) {
        const TypingRecord& record = store.records[index];
        if (record.channel_id == channel_id && record.user_id != excluded_user_id) {
            ++count;
        }
    }
    return count;
}

u32 typing_store_channel_users(const TypingStore& store, u64 channel_id,
                                       u64 excluded_user_id, Slice<u64> users) noexcept {
    if (!store_valid(store) || channel_id == 0 || (!users.data && users.count != 0)) {
        return 0;
    }
    u32 written = 0;
    for (u32 index = 0; index < store.count && written < users.count; ++index) {
        const TypingRecord& record = store.records[index];
        if (record.channel_id == channel_id && record.user_id != excluded_user_id) {
            users[written++] = record.user_id;
        }
    }
    return written;
}

} // namespace maicord
