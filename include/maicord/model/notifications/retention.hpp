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
    none, invalid_input, full_channels
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
// One owning event-loop thread. No allocation, clock, OS calls, account policy,
// ingress deduplication, TTL, or global channel eviction. A zeroed owner has no
// storage and reports full_channels until the caller binds some.
// Binds banks that are disjoint from each other and the owner (index capacity a power of two, >= twice the row
// capacity), keeping the active rows and rebuilding the index. The new banks may share a base with the old
// ones (the caller has grown storage in place) or be fresh; no views escape the API.
void notification_retention_rebind(NotificationRetention*, Slice<NotificationRetentionRow>, Slice<u32>) noexcept;
NotificationRetentionRoom notification_retention_room(const NotificationRetention*, u64 channel) noexcept;
// Call room/reserve BEFORE external Show; append only its successful outcome.
// The 21st success returns the oldest ticket after retiring it from metadata.
NotificationRetentionAppend notification_retention_append(NotificationRetention*, u64 channel, u64 ticket) noexcept;
bool notification_retention_ticket(const NotificationRetention*, u64 channel, u32 ordinal, u64* ticket) noexcept;
NotificationRetentionError notification_retention_discard_channel(NotificationRetention*, u64 channel) noexcept;
} // namespace maicord
