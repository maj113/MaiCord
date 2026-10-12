#pragma once
#include "maicord/model/user_store.hpp"
#include <stddef.h>

namespace maicord {
inline constexpr u32 kRelationshipMaximumRecords = 16384;
inline constexpr u32 kRelationshipNicknameMaximumBytes = 128;
enum class RelationshipType : u8 { none, friend_, blocked, incoming, outgoing, implicit, suggestion };
enum class RelationshipPresence : u8 { offline, online, idle, dnd, unknown };
enum class RelationshipFilter : u8 { all, online, pending, blocked };
struct RelationshipRecord {
    u64 user_id;
    RelationshipType type;
    RelationshipPresence presence;
    bool ignored, spam, stranger, stranger_known;
    u16 nickname_bytes;
    char nickname[kRelationshipNicknameMaximumBytes];
};
static_assert(sizeof(RelationshipRecord) == 144);
static_assert(offsetof(RelationshipRecord, ignored) == 10);
static_assert(offsetof(RelationshipRecord, spam) == 11);
static_assert(offsetof(RelationshipRecord, stranger) == 12);
static_assert(offsetof(RelationshipRecord, stranger_known) == 13);
static_assert(offsetof(RelationshipRecord, nickname_bytes) == 14);
static_assert(offsetof(RelationshipRecord, nickname) == 16);
struct RelationshipStore {
    Slice<RelationshipRecord> records;
    u32 count, revision;
};
enum class RelationshipStoreError : u8 { none, invalid_input, capacity, duplicate };
struct RelationshipStoreResult {
    RelationshipStoreError error;
    bool changed;
    constexpr bool ok() const noexcept { return error == RelationshipStoreError::none; }
};
// Initialize a zeroed or empty caller-owned store; storage stays stable.
bool relationship_store_initialize(RelationshipStore* store, Slice<RelationshipRecord> records) noexcept;
const RelationshipRecord* relationship_store_find(const RelationshipStore& store, u64 user_id) noexcept;
RelationshipStoreResult relationship_store_upsert(RelationshipStore* store, const RelationshipRecord& record) noexcept;
RelationshipStoreResult relationship_store_remove(RelationshipStore* store, u64 user_id) noexcept;
// Validate all sorted rows before mutating active storage. Empty snapshot clears.
RelationshipStoreResult relationship_store_replace(RelationshipStore* store, ConstSlice<RelationshipRecord> sorted) noexcept;
RelationshipStoreResult relationship_store_presence(RelationshipStore* store, u64 user_id, RelationshipPresence presence) noexcept;
bool relationship_matches(const RelationshipRecord& record, RelationshipFilter filter) noexcept;
Utf8View relationship_display_name(const RelationshipRecord& record, const UserStore& users) noexcept;
} // namespace maicord
