#include "maicord/core/stable_storage.hpp"
#include "maicord/model/thread_member_store.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace thread_storage_probe {
struct Fixture {
    StableStorage banks[2];
    ThreadMemberStore store;
    u32 reserve_calls;
};
Fixture fixture{};
bool state(const void* address, DWORD expected) noexcept {
    Win32MemoryInformation info{};
    return address && VirtualQuery(address, info.native(), sizeof(info)) == sizeof(info) && info.State == expected;
}
bool reserve(void* context, u32 count) noexcept {
    auto& f = *static_cast<Fixture*>(context);
    ++f.reserve_calls;
    return stable_storage_reserve(&f.banks[0], count) && stable_storage_reserve(&f.banks[1], count);
}
usize page_bytes(u32 ids, u32 page_size) noexcept {
    const usize bytes = static_cast<usize>(ids) * sizeof(u64);
    return (bytes + page_size - 1) & ~(static_cast<usize>(page_size) - 1);
}
bool committed(const Fixture& f, u32 ids, u32 page_size) noexcept {
    const usize expected = page_bytes(ids, page_size);
    for (const auto& bank : f.banks) {
        if (bank.capacity != ids || bank.arena.offset != ids * sizeof(u64) || bank.arena.committed != expected ||
            !state(bank.arena.base, MEM_COMMIT) || !state(bank.arena.base + expected - 1, MEM_COMMIT)) return false;
        if (expected < bank.arena.reserved && !state(bank.arena.base + expected, MEM_RESERVE)) return false;
    }
    return true;
}
bool snapshot(Fixture* f, u32 ids) noexcept {
    if (!thread_member_store_snapshot_begin(&f->store, 7, 70)) return false;
    for (u64 id = ids; id; --id)
        if (!thread_member_store_snapshot_append(&f->store, id)) return false;
    return thread_member_store_snapshot_commit(&f->store);
}
void release(Fixture* f) noexcept {
    // The lifetime owner withdraws the model views before releasing VM banks.
    f->store = {};
    stable_storage_release(&f->banks[0]); stable_storage_release(&f->banks[1]);
}
}
void test_thread_storage(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace thread_storage_probe;
    auto& f = fixture;
    release(&f); f.reserve_calls = 0;
    Win32SystemInfo system{}; GetSystemInfo(system.native());
    const u32 page_size = system.dwPageSize;
    const bool valid_page = page_size && !(page_size & (page_size - 1));
    check(valid_page, "thread storage system page size supports arena alignment");
    if (!valid_page) return;
    const bool initialized = stable_storage_initialize(&f.banks[0], kThreadMemberMaximumIds, 0, sizeof(u64), alignof(u64)) &&
        stable_storage_initialize(&f.banks[1], kThreadMemberMaximumIds, 0, sizeof(u64), alignof(u64));
    check(initialized, "thread storage reserves two independent maximum ID banks with zero initial capacity");
    if (!initialized) { release(&f); return; }
    const auto* first = f.banks[0].arena.base; const auto* second = f.banks[1].arena.base;
    check(first != second && !f.banks[0].capacity && !f.banks[1].capacity &&
        !f.banks[0].arena.committed && !f.banks[1].arena.committed &&
        state(first, MEM_RESERVE) && state(second, MEM_RESERVE),
        "thread storage initial actual VM pages are reserved rather than committed");
    const bool model_initialized = thread_member_store_initialize(&f.store,
        {stable_storage_data<u64>(f.banks[0]), kThreadMemberMaximumIds},
        {stable_storage_data<u64>(f.banks[1]), kThreadMemberMaximumIds}) &&
        thread_member_store_set_reserve(&f.store, reserve, &f) &&
        thread_member_store_select(&f.store, 7, 70);
    check(model_initialized && !f.reserve_calls && !f.banks[0].arena.committed && !f.banks[1].arena.committed &&
        state(first, MEM_RESERVE) && state(second, MEM_RESERVE),
        "thread model initialization and selected ownership leave both VM banks untouched");
    if (!model_initialized) { release(&f); return; }
    check(snapshot(&f, 1) && committed(f, 1, page_size) && f.reserve_calls == 1 &&
        f.store.count == 1 && thread_member_store_contains(f.store, 1),
        "thread snapshot first ID commits one OS page in each bank before any identity write");
    const u32 initial_commits = f.banks[0].arena.commit_count;
    check(snapshot(&f, 512) && committed(f, 512, page_size) &&
        f.banks[0].arena.commit_count == initial_commits && f.banks[1].arena.commit_count == initial_commits &&
        thread_member_store_contains(f.store, 1) && thread_member_store_contains(f.store, 512),
        "thread snapshot 512 IDs reuses each first 4096 byte page and sorts reverse admission");
    check(snapshot(&f, 513) && committed(f, 513, page_size) &&
        thread_member_store_contains(f.store, 513),
        "thread snapshot 513 IDs grows each bank to page aligned 8192 bytes");
    check(snapshot(&f, kThreadMemberMaximumIds) && committed(f, kThreadMemberMaximumIds, page_size) &&
        f.store.count == kThreadMemberMaximumIds && !f.store.incomplete &&
        f.store.banks[f.store.active_bank][0] == 1 &&
        f.store.banks[f.store.active_bank][kThreadMemberMaximumIds - 1] == kThreadMemberMaximumIds,
        "thread snapshot maximum 2048 IDs commits bounded 16384 byte banks and retains sorted complete identity set");
    const usize first_highwater = f.banks[0].arena.committed, second_highwater = f.banks[1].arena.committed;
    const u32 first_commits = f.banks[0].arena.commit_count, second_commits = f.banks[1].arena.commit_count;
    const u32 calls = f.reserve_calls;
    check(snapshot(&f, 0) && f.store.known && !f.store.count && f.reserve_calls == calls &&
        f.banks[0].arena.committed == first_highwater && f.banks[1].arena.committed == second_highwater,
        "empty thread snapshot commits no new VM and retains reusable bounded bank highwater");
    check(thread_member_store_select(&f.store, 7, 71) && !f.store.known &&
        thread_member_store_select(&f.store, 7, 70) && snapshot(&f, 1) && f.reserve_calls == calls &&
        f.banks[0].arena.committed == first_highwater && f.banks[1].arena.committed == second_highwater &&
        f.banks[0].arena.commit_count == first_commits && f.banks[1].arena.commit_count == second_commits,
        "thread scope reset and small replacement reuse committed highwater without further VM commitment");
    release(&f);
    check(!f.banks[0].arena.base && !f.banks[1].arena.base && !f.banks[0].capacity && !f.banks[1].capacity &&
        !f.banks[0].reserved_capacity && !f.banks[1].reserved_capacity &&
        !f.banks[0].arena.committed && !f.banks[1].arena.committed &&
        !f.store.banks[0].data && !f.store.banks[1].data && !f.store.reserve &&
        state(first, MEM_FREE) && state(second, MEM_FREE),
        "thread lifetime owner release withdraws model views and clears both storage descriptors and OS reservations");
    release(&f);
}
} // namespace maicord
