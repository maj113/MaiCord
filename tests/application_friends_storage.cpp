#include "application_friends_storage.hpp"
#include "maicord/protocol/relationships.hpp"
#include "maicord/core/utf.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::friends_storage_test {
using namespace app_detail;
u32 failures;
void check(bool success, const char* message) noexcept {
    test_check(&failures, success, message);
}
bool state(const void* address, DWORD expected) noexcept {
    Win32MemoryInformation memory{};
    return VirtualQuery(address, memory.native(), sizeof(memory)) == sizeof(memory) && memory.State == expected;
}
RelationshipRecord record(u64 id) noexcept {
    RelationshipRecord value{};value.user_id = id;value.type = RelationshipType::friend_;
    value.presence = RelationshipPresence::unknown;return value;
}
void test_banks() noexcept {
    FriendsRelationshipStorage storage{};
    check(friends_relationship_storage_initialize(&storage), "friends storage: zero-commit initialization succeeds");
    if (!storage.active.arena.base) return;
    auto* active = stable_storage_data<RelationshipRecord>(storage.active);
    auto* candidate = stable_storage_data<RelationshipRecord>(storage.candidate);
    auto* indices = stable_storage_data<u32>(storage.indices);
    check(!storage.active.arena.committed && !storage.candidate.arena.committed && !storage.indices.arena.committed &&
        state(active, MEM_RESERVE) && state(candidate, MEM_RESERVE) && state(indices, MEM_RESERVE),
        "friends storage: empty reservations have no committed data pages");
    check(!friends_relationship_storage_initialize(&storage) && state(active, MEM_RESERVE),
        "friends storage: duplicate initialization preserves reservations");
    RelationshipStore store{{active, kFriendsRelationshipCapacity}, 0, 0};
    check(friends_relationship_storage_upsert(&storage, store, 30) && relationship_store_upsert(&store, record(30)).ok(),
        "friends storage: first live upsert commits before writing");
    const auto committed = storage.active.arena.committed;
    check(committed == 4096 && !storage.candidate.arena.committed && !storage.indices.arena.committed,
        "friends storage: a small live list commits one record-bank page only");
    for (u32 i = 1; i < 30; ++i)
        check(friends_relationship_storage_upsert(&storage, store, i) && relationship_store_upsert(&store, record(i)).ok(),
            "friends storage: sorted insertion admits every shifted destination");
    check(storage.active.arena.committed == 8192 && active == stable_storage_data<RelationshipRecord>(storage.active) &&
        store.records[0].user_id == 1 && store.records[29].user_id == 30,
        "friends storage: crossing the first page keeps the address and ordered records");
    check(friends_relationship_storage_indices(&storage, store.count), "friends storage: projection commits its destination prefix");
    for (u32 i = 0; i < store.count; ++i) indices[i] = i;
    const auto highwater = storage.active.arena.committed;
    for (u32 i = 1; i <= 30; ++i) check(relationship_store_remove(&store, i).ok(), "friends storage: remove clears only admitted rows");
    check(!store.count && storage.active.arena.committed == highwater && !storage.candidate.arena.committed,
        "friends storage: emptying a list retains bounded reusable highwater");
    const u32 commits = storage.active.arena.commit_count;
    check(friends_relationship_storage_upsert(&storage, store, 7) && relationship_store_upsert(&store, record(7)).ok() &&
        storage.active.arena.commit_count == commits, "friends storage: regrowth reuses committed pages");
    check(friends_relationship_storage_candidate(&storage, kFriendsRelationshipCapacity) &&
        friends_relationship_storage_active(&storage, kFriendsRelationshipCapacity) &&
        friends_relationship_storage_indices(&storage, kFriendsRelationshipCapacity), "friends storage: maximum bounded prefixes commit successfully");
    for (u32 i = 0; i < kFriendsRelationshipCapacity; ++i) candidate[i] = record(i + 1);
    check(relationship_store_replace(&store, {candidate, kFriendsRelationshipCapacity}).ok() &&
        store.count == kFriendsRelationshipCapacity && !friends_relationship_storage_upsert(&storage, store, kFriendsRelationshipCapacity + 1) &&
        friends_relationship_storage_upsert(&storage, store, kFriendsRelationshipCapacity),
        "friends storage: full store rejects new IDs while permitting an existing-record update");
    const auto full_commits = storage.candidate.arena.commit_count;
    check(!friends_relationship_storage_candidate(&storage, kFriendsRelationshipCapacity + 1) &&
        !friends_relationship_storage_active(&storage, kFriendsRelationshipCapacity + 1) &&
        !friends_relationship_storage_indices(&storage, kFriendsRelationshipCapacity + 1) &&
        storage.candidate.arena.commit_count == full_commits,
        "friends storage: over-bound admission cannot extend any bank");
    check(storage.active.arena.committed == 589824 && storage.candidate.arena.committed == 589824 &&
        storage.indices.arena.committed == 16384, "friends storage: full bank commitment remains bounded by the original payload sizes");
    friends_relationship_storage_release(&storage);
    check(!storage.active.arena.base && !storage.candidate.arena.base && !storage.indices.arena.base &&
        state(active, MEM_FREE) && state(candidate, MEM_FREE) && state(indices, MEM_FREE),
        "friends storage: release clears metadata and all three OS reservations");
    friends_relationship_storage_release(&storage);
}
void test_rest_preflight() noexcept {
    FriendsRelationshipStorage storage{};u32 scratch[16384]{};
    check(friends_relationship_storage_initialize(&storage), "friends storage: REST fixture reserves banks");
    auto* active = stable_storage_data<RelationshipRecord>(storage.active);
    auto* candidate = stable_storage_data<RelationshipRecord>(storage.candidate);
    RelationshipStore store{{active, kFriendsRelationshipCapacity}, 0, 0};
    const RelationshipsAdmission admission{&storage,
        +[](void* opaque, u32 count, bool active) noexcept {
            auto* storage = static_cast<FriendsRelationshipStorage*>(opaque);
            return active ? friends_relationship_storage_active(storage, count) :
                friends_relationship_storage_candidate(storage, count);
        }};
    const auto replace = [&](Utf8View body) noexcept {
        return relationships_replace(&store, body, {scratch, 16384},
            {candidate, kFriendsRelationshipCapacity}, {}, admission);
    };
    check(!replace(utf8_literal("[{\"id\":\"1\"},]")).ok() && !storage.active.arena.committed && !storage.candidate.arena.committed,
        "friends storage: malformed JSON preflight leaves all record banks uncommitted");
    check(replace(utf8_literal("[]")).ok() && !store.count &&
        !storage.active.arena.committed && !storage.candidate.arena.committed,
        "friends storage: empty REST arrays require no committed record pages");
    check(!replace(utf8_literal("[{\"id\":\"1\",\"type\":100}]")).ok() &&
        storage.candidate.arena.committed == 4096 && !storage.active.arena.committed && !store.count && !store.revision,
        "friends storage: semantic decode failure may stage candidate pages but never commits active pages");
    check(!replace(utf8_literal("[{\"id\":\"1\",\"type\":1},{\"id\":\"1\",\"type\":2}]")).ok() &&
        !storage.active.arena.committed && !store.count && !store.revision,
        "friends storage: duplicate validation precedes active admission");
    char many[8195]{};u32 bytes = 0;many[bytes++] = '[';
    for (u32 i = 0; i < 4097; ++i) { if (i) many[bytes++] = ',';many[bytes++] = '0'; }
    many[bytes++] = ']';
    const auto candidate_highwater = storage.candidate.arena.committed;
    check(!replace({many, bytes}).ok() && !store.count &&
        storage.candidate.arena.committed == candidate_highwater && !storage.active.arena.committed,
        "friends storage: 4097-array preflight cannot increase commitment or silently truncate");
    check(replace(utf8_literal("[{\"id\":\"1\",\"type\":1}]")).ok() && store.count == 1 &&
        storage.active.arena.committed == 4096 && store.records[0].user_id == 1,
        "friends storage: validated REST snapshot commits active prefix before replacement");
    const u32 revision = store.revision;
    check(!replace(utf8_literal("[{\"id\":\"2\",\"type\":1},{\"id\":\"3\",\"type\":99}]")).ok() &&
        store.count == 1 && store.revision == revision && store.records[0].user_id == 1 && storage.active.arena.committed == 4096,
        "friends storage: a bad recovery response preserves authoritative rows and active commitment");
    check(replace(utf8_literal("[]")).ok() && !store.count && !store.records[0].user_id && storage.active.arena.committed == 4096,
        "friends storage: shrinking REST snapshot safely clears its previously committed tail");
    friends_relationship_storage_release(&storage);
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_friends_storage_entry() noexcept {
    using namespace maicord::friends_storage_test;
    test_banks();test_rest_preflight();
    const char* result = failures ? "FAIL Friends storage\n" : "PASS Friends storage\n";
    DWORD written = 0;maicord::u32 bytes = 0;while (result[bytes]) ++bytes;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), result, bytes, &written, nullptr);
    ExitProcess(failures ? 1 : 0);
}
