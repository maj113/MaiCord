#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
inline constexpr u32 kNotificationRetentionTickets = 20;
// Sparse successful-notification metadata. Tickets are opaque, including zero;
// duplicate tickets are retained independently in successful append order.
struct NotificationRetentionRow {
    u64 channel{};
    u64 tickets[kNotificationRetentionTickets]{};
    u8 head{}, count{}, reserved[6]{};
};
static_assert(sizeof(NotificationRetentionRow) == 176);
struct NotificationRetention {
    Slice<NotificationRetentionRow> rows{};
    Slice<u32> index{}; // Zero is empty; otherwise dense row index plus one.
    u32 count{};
};
enum class NotificationRetentionError : u8 {
    none, invalid_input, full_channels, output_too_small
};
struct NotificationRetentionRoom {
    NotificationRetentionError error{};
    u32 tickets{};
    bool exists{};
    bool ok() const noexcept { return error == NotificationRetentionError::none; }
};
struct NotificationRetentionAppend {
    NotificationRetentionError error{};
    u64 evicted_ticket{};
    bool evicted{};
    bool ok() const noexcept { return error == NotificationRetentionError::none; }
};
struct NotificationRetentionTake {
    NotificationRetentionError error{};
    u32 count{};
    bool ok() const noexcept { return error == NotificationRetentionError::none; }
};
// One owning event-loop thread. No allocation, clock, OS calls, account policy,
// ingress deduplication, TTL, or global channel eviction. Rows/index/owner must
// be disjoint. Nonempty index capacity is a power of two, >= twice row capacity.
// Empty storage is valid and reports full_channels until the caller binds it.
bool notification_retention_initialize(NotificationRetention*,
    Slice<NotificationRetentionRow>, Slice<u32>) noexcept;
// Preserves active dense rows and rebuilds the index. Corresponding old/new
// bases may be identical (caller has grown storage); otherwise banks must be
// disjoint, including across old/new row/index banks. No views escape the API.
// Invalid input leaves owner, existing storage and destination storage intact.
NotificationRetentionError notification_retention_rebind(NotificationRetention*,
    Slice<NotificationRetentionRow>, Slice<u32>) noexcept;
NotificationRetentionRoom notification_retention_room(const NotificationRetention*, u64 channel) noexcept;
// Call room/reserve BEFORE external Show; append only its successful outcome.
// The 21st success returns the oldest ticket after retiring it from metadata.
NotificationRetentionAppend notification_retention_append(NotificationRetention*, u64 channel, u64 ticket) noexcept;
bool notification_retention_ticket(const NotificationRetention*, u64 channel, u32 ordinal, u64* ticket) noexcept;
// Copies FIFO tickets and deletes the channel before returning, so external
// close calls never borrow storage and cannot leave a half-retired channel.
NotificationRetentionTake notification_retention_remove_channel(NotificationRetention*, u64 channel, Slice<u64>) noexcept;
NotificationRetentionError notification_retention_discard_channel(NotificationRetention*, u64 channel) noexcept;
// Retains caller storage, clears all metadata. Caller owns account/lifetime rules.
NotificationRetentionError notification_retention_reset(NotificationRetention*) noexcept;
} // namespace maicord
