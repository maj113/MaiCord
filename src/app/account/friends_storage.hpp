#pragma once
#include "account_ui_runtime.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/model/user/relationship_store.hpp"

namespace maicord::app_detail {
// As many relationships as the list decoder takes: Discord allows 1,000 friends and 5,000 blocked users, plus the
// pending requests.
inline constexpr u32 kFriendsRelationshipCapacity = kRelationshipMaximumRecords;

// Four fixed address reservations. Advertised model capacity is the bounded
// reservation; every writer must first admit its entire destination prefix.
// Reserving initializes no records and touches no array pages. `names` holds
// the display name of each record that a projection sorts, beside `indices`.
struct FriendsRelationshipStorage {
    StableStorage active, candidate, indices, names;
};
bool friends_relationship_storage_initialize(FriendsRelationshipStorage* storage) noexcept;
void friends_relationship_storage_release(FriendsRelationshipStorage* storage) noexcept;
bool friends_relationship_storage_active(FriendsRelationshipStorage* storage, u32 count) noexcept;
bool friends_relationship_storage_candidate(FriendsRelationshipStorage* storage, u32 count) noexcept;
// Admits `count` indices and as many sort names.
bool friends_relationship_storage_indices(FriendsRelationshipStorage* storage, u32 count) noexcept;
bool friends_relationship_storage_upsert(FriendsRelationshipStorage* storage,
    const RelationshipStore& store, u64 user_id) noexcept;
} // namespace maicord::app_detail
