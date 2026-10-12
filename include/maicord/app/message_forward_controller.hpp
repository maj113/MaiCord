#pragma once
#include "maicord/app/quick_switcher_controller.hpp"
#include "maicord/app/message_composer_controller.hpp"

namespace maicord {
inline constexpr u32 kMessageForwardMaximumDestinations = 5;
inline constexpr u32 kMessageForwardCaptionMaximumBytes = 8000;
enum class MessageForwardDestinationState : u8 { ready, queued, sending, retry_wait, sent, failed, unavailable, cancelled };
enum class MessageForwardError : u8 { none, invalid_source, stale_scope, stale_results, destination_unavailable,
    selection_limit, no_destinations, invalid_caption, busy, queue_failed };
struct MessageForwardSource {
    MessageForwardReference reference;
    const MessageStore* store;
    MessageHandle handle;
    u64 author_id;
    u16 channel_generation;
    char author[128], preview[512], media_summary[128];
    u16 author_bytes, preview_bytes, media_summary_bytes;
    bool age_restricted;
};
struct MessageForwardDestination {
    QuickSwitcherRow row;
    u64 nonce;
    u16 http_status;
    MessageForwardDestinationState state;
};
struct MessageForwardController {
    QuickSwitcherScope scope;
    MessageForwardSource source;
    QuickSwitcherController candidates;
    MessageForwardDestination destinations[kMessageForwardMaximumDestinations];
    char caption[kMessageForwardCaptionMaximumBytes];
    u32 caption_bytes, destination_count, revision;
    MessageForwardError error;
    bool initialized, committed, caption_valid;
};
// Captures display strings and validated server-confirmed identity. The captured
// source survives timeline eviction and channel navigation. No store mutation.
bool message_forward_source_available(QuickSwitcherSources sources, const MessageStore& store, MessageHandle source) noexcept;
bool message_forward_open(MessageForwardController* controller, QuickSwitcherScope scope,
    QuickSwitcherSources sources, const MessageStore* store, MessageHandle source,
    Utf8View author, Utf8View media_summary = {}) noexcept;
bool message_forward_synchronize(MessageForwardController* controller, QuickSwitcherScope scope,
    QuickSwitcherSources sources, Utf8View query, MessageComposerController* outbox = nullptr) noexcept;
bool message_forward_toggle(MessageForwardController* controller, QuickSwitcherScope scope,
    QuickSwitcherSources sources, u32 index, u32 candidate_revision) noexcept;
bool message_forward_remove(MessageForwardController* controller, u64 channel_id) noexcept;
bool message_forward_set_caption(MessageForwardController* controller, Utf8View caption) noexcept;
bool message_forward_confirm(MessageForwardController* controller, QuickSwitcherScope scope,
    QuickSwitcherSources sources, MessageComposerController* outbox, u64 unix_ms, u64 monotonic_ms) noexcept;
bool message_forward_retry_failed(MessageForwardController* controller, QuickSwitcherScope scope,
    QuickSwitcherSources sources, MessageComposerController* outbox, u64 unix_ms, u64 monotonic_ms,
    u64 channel_id = 0) noexcept;
bool message_forward_refresh(MessageForwardController* controller, const MessageComposerController& outbox) noexcept;
// Host must invalidate account scope before delivering completions. Pending
// requests stay valid after the source is cleared: only a new selection or
// submission requires initialization.
bool message_forward_observe_completion(MessageForwardController* controller, const MessageSendCompletion& completion) noexcept;
// Cancellation drops only work that has not reached transport. Hiding
// the modal does not call this: submitted requests and progress are kept.
bool message_forward_cancel_pending(MessageForwardController* controller, MessageComposerController* outbox) noexcept;
bool message_forward_pending(const MessageForwardController& controller) noexcept;
bool message_forward_current(const MessageForwardController& controller, QuickSwitcherScope scope,
    QuickSwitcherSources sources) noexcept;
Utf8View message_forward_status(const MessageForwardController& controller) noexcept;
Utf8View message_forward_destination_status(const MessageForwardDestination& destination) noexcept;
} // namespace maicord
