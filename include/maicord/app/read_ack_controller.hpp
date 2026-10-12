#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
inline constexpr u32 kReadAckMaximumIntents = 4096;
inline constexpr u32 kReadAckAutomaticDelayMs = 3000;
enum class ReadAckKind : u8 { automatic, manual };
struct ReadAckIntent {
    u64 channel_id;
    u64 message_id;
    u32 last_viewed;
    u32 flags;
    u64 due_at_ms;
    ReadAckKind kind = ReadAckKind::automatic;
    // Manual wire metadata. Presence is separate from whether local history
    // was complete when estimating this count; it never asserts completeness.
    bool mention_count_known = false;
    u32 mention_count = 0;
};
// Full cursor, deadline and u32 metadata remain intact. The kind adds
// eight bytes per lazily backed intent: at most 32KiB for all 4096 entries.
static_assert(sizeof(ReadAckIntent) == 40);
using ReadAckReserve = bool (*)(void* context, u32 required_count) noexcept;
struct ReadAckRequestTicket {
    u64 request_id;
    u32 generation;
};
enum class ReadAckCompletion : u8 { success, retry, terminal };
enum class ReadAckCompletionResult : u8 { ignored, completed, retry_scheduled, superseded };
struct ReadAckController {
    Slice<ReadAckIntent> pending;
    ReadAckReserve reserve;
    void* reserve_context;
    ReadAckIntent inflight;
    u64 request_id;
    u64 retry_not_before_ms;
    u32 count;
    u32 backed_count;
    u32 revision;
    u32 generation;
    bool inflight_active;
    bool inflight_superseded;
};
// Used from one thread. A reserved slice never changes its address. Without a
// reserve callback its whole prefix is writable; a callback backs requested
// prefixes before insertion. Failure leaves desired/inflight state unchanged.
bool read_ack_controller_initialize(ReadAckController* controller,
    Slice<ReadAckIntent> pending) noexcept;
bool read_ack_controller_set_reserve(ReadAckController* controller,
    ReadAckReserve reserve, void* context) noexcept;
// Input due_at_ms is ignored. Automatic updates use 3s; immediate or
// mention-driven updates use 0ms. Coalescing cannot postpone an existing timer.
// Call this successfully before applying an optimistic local read mutation.
// A manual queue is a new user action: after backing succeeds it
// supersedes same-channel inflight work even for an identical target. Kind is
// part of target equality. Manual beginning-boundary cursor zero is valid.
bool read_ack_controller_queue(ReadAckController* controller,
    const ReadAckIntent& desired, u64 now_ms, bool immediate) noexcept;
// No sendable wake/deadline exists while transport has an inflight request.
// Retry deadlines are a controller-wide minimum, even for immediate work.
bool read_ack_controller_next_due(const ReadAckController& controller,
    u64* due_at_ms) noexcept;
bool read_ack_controller_peek_due(const ReadAckController& controller,
    u64 now_ms, ReadAckIntent* intent) noexcept;
// Only after transport accepted the peeked intent and nonzero ID. A
// busy/rejected transport must leave the queue intact and skip this call.
bool read_ack_controller_admit(ReadAckController* controller,
    const ReadAckIntent& intent, u64 request_id, ReadAckRequestTicket* ticket) noexcept;
// Each submission and reset advances generation, including reused request IDs.
// Capture the request handle at submission; never rebuild it from the current
// generation when a response arrives. Retry time is an absolute monotonic ms
// deadline; no policy clamp can shorten a long server-provided Retry-After.
ReadAckCompletionResult read_ack_controller_complete(ReadAckController* controller,
    ReadAckRequestTicket ticket, ReadAckCompletion completion,
    u64 retry_due_at_ms = 0) noexcept;
// Cancellation supersedes a submitted request without freeing transport's
// active slot. Its later response cannot remove freshly queued identical IDs.
bool read_ack_controller_cancel(ReadAckController* controller, u64 channel_id) noexcept;
// Fresh READY/account change only. RESUMED preserves the controller unchanged.
void read_ack_controller_reset(ReadAckController* controller) noexcept;
} // namespace maicord
