#pragma once

#include "maicord/core/json_reader.hpp"

namespace maicord {
inline constexpr u32 kThreadCreationNameMaximumBytes = 400;
inline constexpr u32 kThreadCreationNameMaximumScalars = 100;
inline constexpr u32 kThreadCreationMaximumTargetBytes = 112;
inline constexpr u32 kThreadCreationMaximumBodyBytes = 768;
inline constexpr u32 kThreadCreationMaximumResponseBytes = 64 * 1024;
inline constexpr u32 kThreadCreationForumMaximumResponseBytes = 256 * 1024;
inline constexpr u16 kThreadCreationDefaultArchiveDuration = 4320;
inline constexpr u64 kThreadCreationRouteId = 0x5448524443524541ull;

enum class ThreadCreationOperation : u8 {
    from_message,
    reconcile
};
enum class ThreadCreationEnvelope : u8 {
    channel,
    forum_post
};
// Creation intent. Reconcile keeps this intent but only sends GET for the
// source message ID, which is also the public/announcement thread ID.
// The transport copies name before the synchronous submit call returns.
struct ThreadCreationRequest {
    u64 parent_id, guild_id, source_message_id;
    Utf8View name;
    u16 auto_archive_duration;
    u8 thread_type;
    ThreadCreationOperation operation;
    u8 reserved[4];
};
enum class ThreadCreationError : u8 {
    none,
    input,
    capacity,
    json,
    duplicate,
    identity,
    field
};
struct ThreadCreationResult {
    u32 bytes{};
    ThreadCreationError error{};
    constexpr bool ok() const noexcept {
        return error == ThreadCreationError::none;
    }
};
enum ThreadCreationField : u8 {
    thread_creation_last_message_known = 1,
    thread_creation_archived = 2,
    thread_creation_locked = 4,
    thread_creation_self_joined = 8,
};
// Required metadata is always known after a successful decode. A missing member
// field means unknown and must never erase existing self membership in the thread state.
// The name and every other value are copied; no response pointer is kept.
struct ThreadCreationSnapshot {
    u64 id, parent_id, guild_id, last_message_id, archive_timestamp_ms, self_join_timestamp_ms;
    u32 name_bytes, self_flags;
    u16 auto_archive_duration;
    u8 type, fields;
    u8 reserved[4];
    char name[kThreadCreationNameMaximumBytes];
};
bool thread_creation_request_valid(const ThreadCreationRequest&) noexcept;
// POST for from_message, GET for reconcile. Writers preserve output on failure.
ThreadCreationResult thread_creation_write_target(const ThreadCreationRequest&, Slice<u8>) noexcept;
ThreadCreationResult thread_creation_write_body(const ThreadCreationRequest&, Slice<u8>) noexcept;
// Structural scratch may change on failure, but output is written only on success.
// Input, structural scratch and output must be disjoint. The self member field is
// optional; every ID it supplies must match the thread and the current user.
ThreadCreationResult thread_creation_decode(Utf8View, const ThreadCreationRequest&, u64 self_user_id,
                                            Slice<u32> structural_scratch,
                                            ThreadCreationSnapshot* output) noexcept;
// A forum response also contains the starter message. The envelope overload raises
// only the response size limit; the overload above keeps the 64 KiB limit for channels.
ThreadCreationResult thread_creation_decode(Utf8View, const ThreadCreationRequest&, u64 self_user_id,
                                            Slice<u32> structural_scratch, ThreadCreationSnapshot* output,
                                            ThreadCreationEnvelope) noexcept;
Utf8View thread_creation_name_view(const ThreadCreationSnapshot&) noexcept;
static_assert(sizeof(ThreadCreationRequest) == 48);
static_assert(sizeof(ThreadCreationSnapshot) == 464);
} // namespace maicord
