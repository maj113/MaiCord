#include "application_friends_storage.hpp"

namespace maicord::app_detail {
namespace {
bool admit(StableStorage* bank, u32 count) noexcept {
    return bank->arena.base && count <= kFriendsRelationshipCapacity &&
           (!count || stable_storage_reserve(bank, count));
}
} // namespace
bool friends_relationship_storage_initialize(FriendsRelationshipStorage* storage) noexcept {
    if (storage->active.arena.base || storage->candidate.arena.base || storage->indices.arena.base)
        return false;
    constexpr usize record_bytes = sizeof(RelationshipRecord);
    constexpr usize record_alignment = alignof(RelationshipRecord);
    if (!stable_storage_initialize(&storage->active, kFriendsRelationshipCapacity, 0, record_bytes,
                                   record_alignment) ||
        !stable_storage_initialize(&storage->candidate, kFriendsRelationshipCapacity, 0, record_bytes,
                                   record_alignment) ||
        !stable_storage_initialize(&storage->indices, kFriendsRelationshipCapacity, 0, sizeof(u32), alignof(u32))) {
        friends_relationship_storage_release(storage);
        return false;
    }
    return true;
}
void friends_relationship_storage_release(FriendsRelationshipStorage* storage) noexcept {
    stable_storage_release(&storage->active);
    stable_storage_release(&storage->candidate);
    stable_storage_release(&storage->indices);
}
bool friends_relationship_storage_active(FriendsRelationshipStorage* storage, u32 count) noexcept {
    return admit(&storage->active, count);
}
bool friends_relationship_storage_candidate(FriendsRelationshipStorage* storage, u32 count) noexcept {
    return admit(&storage->candidate, count);
}
bool friends_relationship_storage_indices(FriendsRelationshipStorage* storage, u32 count) noexcept {
    return admit(&storage->indices, count);
}
bool friends_relationship_storage_upsert(FriendsRelationshipStorage* storage,
                                         const RelationshipStore& store, u64 user_id) noexcept {
    if (!user_id || store.count > kFriendsRelationshipCapacity)
        return false;
    const bool existing = relationship_store_find(store, user_id) != nullptr;
    return friends_relationship_storage_active(storage, store.count + (existing ? 0u : 1u));
}
FriendsStorageBankMetrics friends_relationship_storage_metrics(const StableStorage& bank) noexcept {
    return {bank.arena.reserved, bank.arena.committed, bank.capacity, bank.arena.commit_count, bank.arena.base};
}
} // namespace maicord::app_detail
