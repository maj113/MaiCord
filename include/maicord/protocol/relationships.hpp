#pragma once
#include "maicord/model/relationship_store.hpp"

namespace maicord {
enum class RestMethod : u8;
inline constexpr u64 kRelationshipsRouteId = 0x52454C4154494F4Eull;
inline constexpr u32 kRelationshipUsernameMaximumBytes = 32;
inline constexpr u32 kRelationshipsMaximumBytes = 4u * 1024u * 1024u;
enum class RelationshipOperation : u8 { list, request, accept, remove, block, accept_stranger,
    ignore, unignore, clear_incoming, clear_hidden };
enum class RelationshipEvent : u8 { add, remove, update };
enum class RelationshipWriteError : u8 { none, invalid_input, output_too_small };
struct RelationshipWriteResult {
    u32 bytes;
    RelationshipWriteError error;
    constexpr bool ok() const noexcept { return error == RelationshipWriteError::none; }
};
RestMethod relationship_method(RelationshipOperation operation) noexcept;
RelationshipWriteResult write_relationship_target(RelationshipOperation operation, u64 user_id, Slice<u8> output) noexcept;
RelationshipWriteResult write_relationship_body(RelationshipOperation operation, Utf8View username, Slice<u8> output) noexcept;
struct RelationshipUser {
    u64 id;
    u16 username_bytes, display_name_bytes;
    u8 avatar_bytes;
    bool avatar_seen;
    char username[kUserDisplayNameMaximumBytes];
    char display_name[kUserDisplayNameMaximumBytes];
    char avatar[kUserAvatarHashMaximumBytes];
};
static_assert(sizeof(RelationshipUser) == 304);
static_assert(offsetof(RelationshipUser, avatar_seen) == 13);
struct RelationshipMutation {
    RelationshipRecord record;
    RelationshipUser user;
    bool preserve_nickname;
};
static_assert(sizeof(RelationshipMutation) == 456);
static_assert(offsetof(RelationshipMutation, record) == 0);
static_assert(offsetof(RelationshipMutation, user) == 144);
static_assert(offsetof(RelationshipMutation, preserve_nickname) == 448);
enum class RelationshipsDecodeError : u8 { none, invalid_input, scan, json, record, capacity, duplicate, store };
struct RelationshipsDecodeResult {
    RelationshipsDecodeError error;
    u32 count, user_count;
    bool changed;
    constexpr bool ok() const noexcept { return error == RelationshipsDecodeError::none; }
};
RelationshipsDecodeResult relationship_decode_record(Utf8View input,
    Slice<u32> structural_scratch, RelationshipEvent event, RelationshipMutation* output) noexcept;
RelationshipStoreResult relationship_apply_decoded(RelationshipStore* store,
    RelationshipEvent event, const RelationshipMutation& mutation) noexcept;
// Optional storage hook: called with the array's record count for the candidate
// rows (active=false), then after full validation with max(new count, previous
// count) for the active rows, so a shrinking snapshot can clear its old tail. It may
// grow storage but must not change pointers, counts or row data. If it rejects, the
// active rows, count and revision are unchanged.
struct RelationshipsAdmission {
    void* context;
    bool (*admit)(void* context, u32 record_count, bool active) noexcept;
};
// Parses a complete REST/READY relationships array into caller scratch, sorts,
// rejects duplicate IDs and only then replaces the active rows. User snapshots go to
// `users`; an empty slice skips collecting them but still validates every user.
RelationshipsDecodeResult relationships_replace(RelationshipStore* store, Utf8View input,
    Slice<u32> structural_scratch, Slice<RelationshipRecord> candidate,
    Slice<RelationshipUser> users, RelationshipsAdmission admission = {}) noexcept;
// Apply only after successful decode. Reports the first user-store error and the
// count applied; relationship decode never silently drops rows on name capacity.
struct RelationshipUserApplyResult {
    UserStoreError error;
    u32 applied;
    constexpr bool ok() const noexcept { return error == UserStoreError::none; }
};
RelationshipUserApplyResult relationship_users_apply(UserStore* users, ConstSlice<RelationshipUser> snapshots) noexcept;
// Call with the same array that already decoded successfully as a snapshot.
// Streams one user snapshot at a time instead of duplicating every cached name.
RelationshipUserApplyResult relationships_hydrate(UserStore* users,
    Utf8View validated_array, Slice<u32> structural_scratch) noexcept;
} // namespace maicord
