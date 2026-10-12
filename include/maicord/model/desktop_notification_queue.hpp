#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
inline constexpr u32 kDesktopNotificationQueueRows = 32;
inline constexpr u32 kDesktopNotificationQueueBytes = 16 * 1024;
inline constexpr u32 kDesktopNotificationQueueItemBytes = 4096;
struct DesktopNotificationQueueInput {
    u64 profile_id{}, generation{}, channel_id{}, message_id{}, source_sequence{};
    Utf8View title{}, body{}, avatar_file_uri{};
    bool silent{}, suppress_popup{};
    // Optional policy/selection scope (e.g. thread-starter parent). Activation
    // still uses channel_id. Zero means the caller did not supply this scope.
    u64 policy_channel_id{};
    // Opaque optional sound intent copied on enqueue; zero ID means no sound.
    // The queue has no playback or epoch policy.
    u64 sound_epoch{};
    u32 sound_id{};
};
struct DesktopNotificationQueueRow {
    u64 token{}, profile_id{}, generation{}, channel_id{}, message_id{}, source_sequence{};
    u32 offset{}, title_bytes{}, body_bytes{}, avatar_bytes{};
    bool silent{}, suppress_popup{};
    u64 policy_channel_id{};
    u64 sound_epoch{};
    u32 sound_id{};
};
enum class DesktopNotificationQueueError : u8 {
    none, invalid_input, stale_generation, duplicate, full_rows, full_bytes,
    empty, busy, stale_token, token_exhausted
};
struct DesktopNotificationQueueResult {
    DesktopNotificationQueueError error{};
    u64 token{};
    bool wake_required{};
    bool ok() const noexcept { return error == DesktopNotificationQueueError::none; }
};
struct DesktopNotificationQueueDispatch {
    DesktopNotificationQueueRow identity{};
    Utf8View title{}, body{}, avatar_file_uri{};
    u32 attempt{};
};
enum class DesktopNotificationQueueOutcome : u8 { submitted, failed, retry };
struct DesktopNotificationQueueCompletion {
    DesktopNotificationQueueError error{};
    DesktopNotificationQueueRow identity{};
    bool retry_pending{}, canceled{}, removal_required{};
};
struct DesktopNotificationQueueMetrics {
    u64 admitted{}, submitted{}, failed{}, canceled{}, retries{}, capacity_failures{};
    u32 waiting{}, bytes{}, outstanding{}, high_rows{}, high_bytes{};
};
struct DesktopNotificationQueue {
    Slice<DesktopNotificationQueueRow> rows{};
    Slice<char> bytes{}, scratch{};
    DesktopNotificationQueueRow inflight{};
    DesktopNotificationQueueMetrics counters{};
    u64 profile_id{}, generation{}, next_token{1};
    u32 count{}, used{}, attempt{};
    bool has_inflight{}, dispatching{}, inflight_canceled{};
};
// Storage and the queue object must be disjoint. Fresh initialization invalidates
// all outstanding dispatch tokens; do not reinitialize while a dispatch is outstanding. No allocation,
// clock, OS call or thread synchronization. One owning event-loop thread only.
bool desktop_notification_queue_initialize(DesktopNotificationQueue*,
    Slice<DesktopNotificationQueueRow>, Slice<char> packed_bytes, Slice<char> dispatch_scratch) noexcept;
// Reset preserves the token counter and marks an active OS dispatch canceled.
// Use a fresh nonzero generation on READY/account change; RESUMED preserves it.
// Zero profile/generation disables enqueueing, for account stop/shutdown.
void desktop_notification_queue_reset(DesktopNotificationQueue*, u64 profile, u64 generation) noexcept;
// Conservative pre-pop reservation check; max_bytes includes all three strings.
// Waiting and in-flight together consume the caller's header capacity.
bool desktop_notification_queue_can_admit(const DesktopNotificationQueue*, u32 max_bytes) noexcept;
// Atomically deep-copies valid UTF8 without NUL. Input views must not alias any
// queue storage. Full/duplicate/stale input never replaces an existing intent.
DesktopNotificationQueueResult desktop_notification_queue_enqueue(DesktopNotificationQueue*,
    const DesktopNotificationQueueInput&) noexcept;
bool desktop_notification_queue_front(const DesktopNotificationQueue*, DesktopNotificationQueueRow*) noexcept;
bool desktop_notification_queue_pending(const DesktopNotificationQueue*) noexcept;
DesktopNotificationQueueMetrics desktop_notification_queue_metrics(const DesktopNotificationQueue*) noexcept;
// Begin deep-copies the head into scratch; returned views remain stable through
// later enqueue/purge/reset and the OS call, until complete or a later begin.
// Retry keeps the same scratch/token and precedes later waiting FIFO entries.
DesktopNotificationQueueResult desktop_notification_queue_begin(DesktopNotificationQueue*,
    DesktopNotificationQueueDispatch*) noexcept;
// Token AND attempt identify each begin, including retries. A late completion
// from an earlier attempt leaves the active attempt and its scratch untouched.
// A successful Show canceled during dispatch needs a deferred native removal;
// submission is not proof of a visible banner. Retry count is informational:
// the caller imposes its own attempt limit.
DesktopNotificationQueueCompletion desktop_notification_queue_complete(DesktopNotificationQueue*,
    u64 token, u32 attempt, DesktopNotificationQueueOutcome) noexcept;
u32 desktop_notification_queue_purge_channel(DesktopNotificationQueue*, u64 profile, u64 generation, u64 channel) noexcept;
u32 desktop_notification_queue_purge_message(DesktopNotificationQueue*, u64 profile, u64 generation,
    u64 channel, u64 message) noexcept;
u32 desktop_notification_queue_purge_profile(DesktopNotificationQueue*, u64 profile) noexcept;
u32 desktop_notification_queue_purge_all(DesktopNotificationQueue*) noexcept;
// Packed layout: enqueue copies only new bytes; begin/purge move at most the
// configured byte capacity plus header capacity, zeroing discarded private text.
// Defaults are 32 headers, 16KiB packed text and 4KiB dispatch scratch (<24KiB total).
static_assert(sizeof(DesktopNotificationQueueRow) <= 96);
static_assert(sizeof(DesktopNotificationQueue) + kDesktopNotificationQueueRows * sizeof(DesktopNotificationQueueRow) +
    kDesktopNotificationQueueBytes + kDesktopNotificationQueueItemBytes < 24 * 1024);
} // namespace maicord
