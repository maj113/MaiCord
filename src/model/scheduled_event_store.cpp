#include "maicord/model/scheduled_event_store.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
ScheduledEvent* find(ScheduledEventStore* store, u64 id) noexcept {
    for (u32 i = 0; i < store->count; ++i)
        if (store->entries[i].id == id)
            return &store->entries[i];
    return nullptr;
}
void erase(ScheduledEventStore* store, u32 index) noexcept {
    if (index + 1 < store->count)
        copy_bytes(&store->entries[index], &store->entries[store->count - 1], sizeof(ScheduledEvent));
    --store->count;
    ++store->revision;
}
// The entry to drop when full: a finished one, else the latest start.
u32 victim(const ScheduledEventStore& store) noexcept {
    u32 chosen = 0;
    for (u32 i = 0; i < store.count; ++i) {
        if (!scheduled_event_listed(store.entries[i]))
            return i;
        if (store.entries[i].start_ms > store.entries[chosen].start_ms)
            chosen = i;
    }
    return chosen;
}
bool earlier(const ScheduledEvent& a, const ScheduledEvent& b) noexcept {
    const bool live_a = a.status == ScheduledEventStatus::active, live_b = b.status == ScheduledEventStatus::active;
    if (live_a != live_b)
        return live_a;
    return a.start_ms != b.start_ms ? a.start_ms < b.start_ms : a.id < b.id;
}
} // namespace

void scheduled_event_store_reset(ScheduledEventStore* store) noexcept {
    if (!store)
        return;
    const u32 revision = store->revision;
    store->count = 0;
    store->revision = revision + 1;
}

const ScheduledEvent* scheduled_event_store_find(const ScheduledEventStore& store, u64 event_id) noexcept {
    return find(const_cast<ScheduledEventStore*>(&store), event_id);
}

bool scheduled_event_store_upsert(ScheduledEventStore* store, const ScheduledEvent& event) noexcept {
    if (!store || !event.id || !event.guild_id)
        return false;
    ScheduledEvent* existing = find(store, event.id);
    if (!scheduled_event_listed(event)) {
        if (!existing)
            return false;
        erase(store, static_cast<u32>(existing - store->entries));
        return true;
    }
    ScheduledEvent next = event;
    if (existing) {
        if (!next.user_count_known && existing->user_count_known) {
            next.user_count = existing->user_count;
            next.user_count_known = true;
        }
        if (!next.interested_known && existing->interested_known) {
            next.interested = existing->interested;
            next.interested_known = true;
        }
        copy_bytes(existing, &next, sizeof(next));
    } else {
        if (store->count >= kScheduledEventStoreCapacity) {
            const u32 index = victim(*store);
            // A new event that starts after every stored one is not kept.
            if (scheduled_event_listed(store->entries[index]) && store->entries[index].start_ms <= next.start_ms)
                return false;
            erase(store, index);
        }
        copy_bytes(&store->entries[store->count++], &next, sizeof(next));
    }
    ++store->revision;
    return true;
}

bool scheduled_event_store_remove(ScheduledEventStore* store, u64 event_id) noexcept {
    ScheduledEvent* existing = store ? find(store, event_id) : nullptr;
    if (!existing)
        return false;
    erase(store, static_cast<u32>(existing - store->entries));
    return true;
}

bool scheduled_event_store_clear_guild(ScheduledEventStore* store, u64 guild_id) noexcept {
    if (!store || !guild_id)
        return false;
    bool changed = false;
    for (u32 i = store->count; i-- > 0;)
        if (store->entries[i].guild_id == guild_id) {
            erase(store, i);
            changed = true;
        }
    return changed;
}

bool scheduled_event_store_user(ScheduledEventStore* store, u64 event_id, u64 user_id, u64 self_user_id,
                                bool added) noexcept {
    ScheduledEvent* event = store ? find(store, event_id) : nullptr;
    if (!event)
        return false;
    const bool self = user_id && user_id == self_user_id;
    // Our own confirmed change already moved the count.
    if (self && event->interested_known && event->interested == added)
        return false;
    if (event->user_count_known)
        event->user_count = added ? event->user_count + 1 : (event->user_count ? event->user_count - 1 : 0);
    if (self) {
        event->interested = added;
        event->interested_known = true;
    }
    ++store->revision;
    return true;
}

bool scheduled_event_store_set_interest(ScheduledEventStore* store, u64 event_id, bool interested,
                                        bool adjust_count) noexcept {
    ScheduledEvent* event = store ? find(store, event_id) : nullptr;
    if (!event || (event->interested_known && event->interested == interested))
        return false;
    if (adjust_count && event->user_count_known)
        event->user_count =
            interested ? event->user_count + 1 : (event->user_count ? event->user_count - 1 : 0);
    event->interested = interested;
    event->interested_known = true;
    ++store->revision;
    return true;
}

void scheduled_event_store_reset_interest(ScheduledEventStore* store, u64 guild_id) noexcept {
    if (!store)
        return;
    for (u32 i = 0; i < store->count; ++i)
        if (store->entries[i].guild_id == guild_id) {
            store->entries[i].interested = false;
            store->entries[i].interested_known = true;
        }
    ++store->revision;
}

u32 scheduled_event_store_guild_events(const ScheduledEventStore& store, u64 guild_id,
                                       Slice<const ScheduledEvent*> output) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < store.count; ++i) {
        const auto& event = store.entries[i];
        if (event.guild_id != guild_id || !scheduled_event_listed(event))
            continue;
        if (output.data && count < output.count) {
            // Insertion sort: the store is small.
            u32 at = count;
            while (at && earlier(event, *output[at - 1])) {
                output[at] = output[at - 1];
                --at;
            }
            output[at] = &event;
        }
        ++count;
    }
    return minimum<u32>(count, output.data ? output.count : count);
}

u32 scheduled_event_store_guild_count(const ScheduledEventStore& store, u64 guild_id) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < store.count; ++i)
        count += store.entries[i].guild_id == guild_id && scheduled_event_listed(store.entries[i]);
    return count;
}

} // namespace maicord
