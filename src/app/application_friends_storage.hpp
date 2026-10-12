#pragma once
#include "account_ui_runtime.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/model/relationship_store.hpp"

namespace maicord::app_detail {
inline constexpr u32 kFriendsRelationshipCapacity = kUserCapacity;

// Three fixed address reservations. Advertised model capacity is the bounded
// reservation; every writer must first admit its entire destination prefix.
// Reserving initializes no records and touches no array pages.
struct FriendsRelationshipStorage {
    StableStorage active, candidate, indices;
};
struct FriendsStorageBankMetrics {
    usize reserved_bytes, committed_bytes;
    u32 admitted_count, commit_count;
    const void* address;
};
struct ApplicationFriendsStorageMetrics {
    usize controller_bytes;
    FriendsStorageBankMetrics active, candidate, indices, requests_indices;
};
bool friends_relationship_storage_initialize(FriendsRelationshipStorage* storage) noexcept;
void friends_relationship_storage_release(FriendsRelationshipStorage* storage) noexcept;
bool friends_relationship_storage_active(FriendsRelationshipStorage* storage, u32 count) noexcept;
bool friends_relationship_storage_candidate(FriendsRelationshipStorage* storage, u32 count) noexcept;
bool friends_relationship_storage_indices(FriendsRelationshipStorage* storage, u32 count) noexcept;
bool friends_relationship_storage_upsert(FriendsRelationshipStorage* storage,
    const RelationshipStore& store, u64 user_id) noexcept;
FriendsStorageBankMetrics friends_relationship_storage_metrics(const StableStorage& bank) noexcept;
ApplicationFriendsStorageMetrics application_friends_storage_metrics() noexcept;
} // namespace maicord::app_detail
