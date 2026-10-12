#include "maicord/model/thread_member_store.hpp"
#include "maicord/core/base.hpp"
#include "maicord/core/heap_sort.hpp"

namespace maicord {
namespace {
u32 lower_bound(Slice<u64> ids, u32 count, u64 id) noexcept {
    u32 first = 0;
    while (first < count) {
        const u32 mid = first + (count - first) / 2;
        if (ids[mid] < id) first = mid + 1; else count = mid;
    }
    return first;
}
bool reserve(ThreadMemberStore* store, u32 count) noexcept {
    if (!store->reserve || count <= store->backed_count) return true;
    if (!store->reserve(store->reserve_context, count)) {
        saturating_increment(store->rejected); return false;
    }
    store->backed_count = count; return true;
}
void compact_stage(ThreadMemberStore* store) noexcept {
    if (store->staging_sorted) return;
    const auto bank = store->banks[store->active_bank ^ 1];
    heap_sort(bank.data, store->staging_count, [](u64 a, u64 b) noexcept { return a < b; });
    u32 unique = 0;
    for (u32 i = 0; i < store->staging_count; ++i)
        if (!unique || bank[i] != bank[unique - 1]) bank[unique++] = bank[i];
    store->staging_count = unique; store->staging_sorted = true;
}
}
bool thread_member_store_initialize(ThreadMemberStore* store, Slice<u64> first, Slice<u64> second) noexcept {
    if (!store || !first.data || !second.data || first.data == second.data || !first.count || first.count != second.count || first.count > kThreadMemberMaximumIds) return false;
    const usize a = reinterpret_cast<usize>(first.data), b = reinterpret_cast<usize>(second.data);
    const usize bytes = static_cast<usize>(first.count) * sizeof(u64);
    if ((a < b ? b - a : a - b) < bytes) return false;
    *store = {}; store->banks[0] = first; store->banks[1] = second; return true;
}
bool thread_member_store_set_reserve(ThreadMemberStore* store,
    ThreadMemberReserve callback, void* context) noexcept {
    if (!store || !store->banks[0].data || store->staging) return false;
    store->reserve = callback; store->reserve_context = context; store->backed_count = 0;
    return true;
}
bool thread_member_store_select(ThreadMemberStore* store, u64 guild_id, u64 thread_id) noexcept {
    if (!store || !store->banks[0].data || (!guild_id != !thread_id)) return false;
    if (store->guild_id == guild_id && store->thread_id == thread_id) return true;
    store->guild_id = guild_id; store->thread_id = thread_id;
    thread_member_store_reset_snapshot(store); return true;
}
void thread_member_store_reset_snapshot(ThreadMemberStore* store) noexcept {
    if (!store) return;
    store->count = 0; store->known = false; store->incomplete = false;
    store->staging = false; store->staging_count = 0;
    saturating_increment(store->revision); ++store->generation;
    if (!store->generation) ++store->generation;
}
bool thread_member_store_owns(const ThreadMemberStore& store, u64 guild_id, u64 thread_id) noexcept {
    return guild_id && thread_id && store.guild_id == guild_id && store.thread_id == thread_id;
}
bool thread_member_store_contains(const ThreadMemberStore& store, u64 id) noexcept {
    if (!id || !store.known || !store.banks[store.active_bank].data) return false;
    const u32 pos = lower_bound(store.banks[store.active_bank], store.count, id);
    return pos < store.count && store.banks[store.active_bank][pos] == id;
}
bool thread_member_store_snapshot_begin(ThreadMemberStore* store, u64 guild_id, u64 thread_id) noexcept {
    if (!store || !thread_member_store_owns(*store, guild_id, thread_id)) return false;
    store->staging = true; store->staging_count = 0; store->staging_incomplete = false; store->staging_sorted = true; return true;
}
bool thread_member_store_snapshot_append(ThreadMemberStore* store, u64 id) noexcept {
    if (!store || !store->staging) return false;
    if (!id) { store->staging = false; saturating_increment(store->rejected); return false; }
    const auto bank = store->banks[store->active_bank ^ 1];
    if (store->staging_count == bank.count) {
        // Free duplicate staging slots before diagnosing a distinct-ID overflow.
        compact_stage(store);
        const u32 position = lower_bound(bank, store->staging_count, id);
        if (position < store->staging_count && bank[position] == id) return true;
        if (store->staging_count == bank.count) { store->staging_incomplete = true; return true; }
    }
    if (!reserve(store, store->staging_count + 1)) { store->staging = false; return false; }
    bank[store->staging_count++] = id; store->staging_sorted = false;
    return true;
}
bool thread_member_store_snapshot_commit(ThreadMemberStore* store, bool incomplete) noexcept {
    if (!store || !store->staging) return false;
    compact_stage(store);
    store->active_bank ^= 1; store->count = store->staging_count;
    store->known = true; store->incomplete = incomplete || store->staging_incomplete;
    store->staging = false; saturating_increment(store->revision); return true;
}
void thread_member_store_snapshot_abort(ThreadMemberStore* store) noexcept { if (store) store->staging = false; }
bool thread_member_store_add(ThreadMemberStore* store, u64 id) noexcept {
    if (!store || !store->known || !id) return false;
    const auto bank = store->banks[store->active_bank];
    const u32 position = lower_bound(bank, store->count, id);
    if (position < store->count && bank[position] == id) return false;
    if (store->count == bank.count || !reserve(store, store->count + 1)) {
        saturating_increment_if(!store->incomplete, store->revision); store->incomplete = true; return false;
    }
    for (u32 i = store->count; i > position; --i) bank[i] = bank[i - 1];
    bank[position] = id; ++store->count; saturating_increment(store->revision); return true;
}
bool thread_member_store_remove(ThreadMemberStore* store, u64 id) noexcept {
    if (!store || !store->known || !id) return false;
    const auto bank = store->banks[store->active_bank];
    const u32 pos = lower_bound(bank, store->count, id);
    if (pos == store->count || bank[pos] != id) return false;
    for (u32 i = pos + 1; i < store->count; ++i) bank[i - 1] = bank[i];
    --store->count; saturating_increment(store->revision); return true;
}
} // namespace maicord
