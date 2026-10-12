#include "application_friends_storage.hpp"

namespace maicord::app_detail {
namespace {
bool admit(StableStorage* bank, u32 count) noexcept {
    return bank && bank->arena.base && count <= kFriendsRelationshipCapacity &&
        (!count || stable_storage_reserve(bank, count));
}
}
bool friends_relationship_storage_initialize(FriendsRelationshipStorage* storage) noexcept {
    if (!storage || storage->active.arena.base || storage->candidate.arena.base || storage->indices.arena.base) return false;
    if (!stable_storage_initialize(&storage->active, kFriendsRelationshipCapacity, 0,
            sizeof(RelationshipRecord), alignof(RelationshipRecord)) ||
        !stable_storage_initialize(&storage->candidate, kFriendsRelationshipCapacity, 0,
            sizeof(RelationshipRecord), alignof(RelationshipRecord)) ||
        !stable_storage_initialize(&storage->indices, kFriendsRelationshipCapacity, 0, sizeof(u32), alignof(u32))) {
        friends_relationship_storage_release(storage);return false;
    }
    return true;
}
void friends_relationship_storage_release(FriendsRelationshipStorage* storage) noexcept {
    if (!storage) return;
    stable_storage_release(&storage->active);stable_storage_release(&storage->candidate);stable_storage_release(&storage->indices);
}
bool friends_relationship_storage_active(FriendsRelationshipStorage* storage, u32 count) noexcept {
    return storage && admit(&storage->active, count);
}
bool friends_relationship_storage_candidate(FriendsRelationshipStorage* storage, u32 count) noexcept {
    return storage && admit(&storage->candidate, count);
}
bool friends_relationship_storage_indices(FriendsRelationshipStorage* storage, u32 count) noexcept {
    return storage && admit(&storage->indices, count);
}
bool friends_relationship_storage_upsert(FriendsRelationshipStorage* storage,
    const RelationshipStore& store, u64 user_id) noexcept {
    if (!user_id || store.count > kFriendsRelationshipCapacity) return false;
    const bool existing = relationship_store_find(store, user_id) != nullptr;
    return friends_relationship_storage_active(storage, store.count + (existing ? 0u : 1u));
}
FriendsStorageBankMetrics friends_relationship_storage_metrics(const StableStorage& bank) noexcept {
    return {bank.arena.reserved, bank.arena.committed, bank.capacity, bank.arena.commit_count, bank.arena.base};
}
} // namespace maicord::app_detail
