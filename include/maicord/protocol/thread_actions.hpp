#pragma once

#include "maicord/core/json_reader.hpp"

namespace maicord {

inline constexpr u32 kThreadActionMaximumTargetBytes = 112;
inline constexpr u32 kThreadActionMaximumBodyBytes = 18;
inline constexpr u32 kThreadMemberMaximumBodyBytes = 64 * 1024;
inline constexpr u64 kThreadMemberRouteId = 0x544852444d454d42ull;

enum class ThreadActionOperation : u8 { fetch_self, join, leave, unarchive };
struct ThreadActionRequest {
    u64 thread_id;
    u64 self_user_id;
    ThreadActionOperation operation;
    u8 reserved[7];
};
enum class ThreadActionError : u8 { none, input, capacity, json, duplicate, identity, field };
struct ThreadActionResult {
    u32 bytes{};
    ThreadActionError error{};
    constexpr bool ok() const noexcept { return error == ThreadActionError::none; }
};
enum ThreadMemberField : u32 {
    thread_member_thread_id = 1,
    thread_member_user_id = 2,
    thread_member_join_timestamp = 4,
    thread_member_flags = 8,
};
// IDs omitted by the server are filled in from the request; `fields` records
// which values were actually on the wire. No response strings are borrowed.
struct ThreadMemberSnapshot {
    u64 thread_id{}, user_id{}, join_timestamp_ms{};
    u32 flags{}, fields{};
};
bool thread_action_request_valid(const ThreadActionRequest&) noexcept;
// Writers preserve output on failure. Unarchive emits archived=false only.
ThreadActionResult thread_action_write_target(
    const ThreadActionRequest&, Slice<u8>) noexcept;
ThreadActionResult thread_action_write_body(
    const ThreadActionRequest&, Slice<u8>) noexcept;
// A standalone member response requires join_timestamp and flags; IDs that are
// present must match the request. Every failure leaves output unchanged.
ThreadActionResult thread_member_decode(
    Utf8View, u64 expected_thread_id, u64 expected_self_user_id,
    ThreadMemberSnapshot*) noexcept;
static_assert(sizeof(ThreadActionRequest) == 24);
static_assert(sizeof(ThreadMemberSnapshot) == 32);

} // namespace maicord
