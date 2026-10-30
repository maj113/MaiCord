#pragma once

#include "maicord/app/message_event_pump.hpp"
#include "maicord/protocol/message_history.hpp"

namespace maicord {
struct RestClientResponse;

bool message_history_observe_page(UserStore* users, GuildMemberStore* members,
                                  ChatStateStore* state,
                                  ConstSlice<GatewayMessageEvent> messages) noexcept;
bool message_history_apply_mention_flags(MessageTimeline* timeline, const ChatStateStore* state,
                                         ConstSlice<GatewayMessageEvent> messages) noexcept;

enum class MessageHistoryDirection : u8 {
    older,
    newer,
};

enum class MessageHistoryApplyError : u8 {
    none,
    invalid_input,
    count_mismatch,
    invalid_page,
    channel_mismatch,
    capacity,
    reconcile,
    ui_mutation,
    not_found,
    stale_revision,
    attachment_missing,
    rich_store,
};

struct MessageHistoryApplyResult {
    MessageTimelineInvalidation invalidation;
    u32 inserted;
    u32 evicted;
    u32 compactions;
    MessageHistoryApplyError error;
    bool destructive_failure;
    // Messages the store could not admit (one unexpected field or value) are
    // left out of the page instead of failing the whole channel load.
    u32 skipped;
    // Inserted messages whose rich data (cards, embeds, ...) could not be kept.
    u32 rich_skipped;
    // Page messages already in the window.
    u32 resident;

    constexpr bool ok() const noexcept {
        return error == MessageHistoryApplyError::none;
    }
};

// Applies one already-decoded, newest-to-oldest page with at most two list
// rebuilds when no local outbox rows are loaded: one edge removal and one
// contiguous insertion. Local rows are removed for controller restoration.
// A newer page skips rows at or below the newest resident unless
// `fill_below_newest`: then a latest page also inserts, one by one, the rows
// it has below messages that landed while it was in flight.
// All externally supplied data is validated before the first change to the
// loaded timeline.
MessageHistoryApplyResult message_timeline_apply_history_page(
    MessageTimeline* timeline, ConstSlice<GatewayMessageEvent> messages, MessageHistoryDirection direction,
    MessageContentAvailability empty_content_availability, bool fill_below_newest = false) noexcept;

// Applies one around-message refresh without changing the loaded order.
// The source revision and attachment identity bind an asynchronous signed-URL
// refresh to the message that requested it; a concurrent gateway mutation is
// rejected as stale.
MessageHistoryApplyResult message_history_refresh_attachment(
    MessageTimeline* timeline, const GatewayMessageEvent& message, u64 expected_message_id,
    u64 expected_channel_id, u32 expected_message_revision, u64 expected_attachment_id,
    MessageContentAvailability empty_content_availability) noexcept;

enum class MessageHistoryCatchupState : u8 {
    idle,
    request_required,
    awaiting_page,
    waiting_retry,
    complete,
    failed,
    paused,
};

enum class MessageHistoryCatchupFailure : u8 {
    none,
    transport,
    http_status,
    rate_limited,
    decode,
    apply,
};

struct MessageHistoryCatchup {
    u64 channel_id;
    u64 cursor_after;
    u64 retry_at_ms;
    u32 request_limit;
    u32 requests;
    u32 pages;
    u32 received;
    u32 inserted;
    u32 evicted;
    u32 failures;
    // Timeline deferral counter when the current request basis was taken. A
    // live create deferred while the final page was in flight is not in that
    // page, so reaching the tail then asks once more instead of completing.
    u32 deferred_mark;
    MessageHistoryCatchupState state;
    MessageHistoryCatchupFailure last_failure;
    u8 transient_retries;
    bool incremental;
    // The last applied page answered a latest query and was short and
    // nonempty: it holds the channel's first message.
    bool latest_short;
};

bool message_history_catchup_begin(MessageHistoryCatchup* catchup, MessageEventPump* pump, u64 channel_id,
                                   u32 request_limit) noexcept;
bool message_history_catchup_take_query(MessageHistoryCatchup* catchup,
                                        MessageHistoryQuery* query) noexcept;
bool message_history_catchup_tick(MessageHistoryCatchup* catchup, u64 now_ms) noexcept;
bool message_history_catchup_fail(MessageHistoryCatchup* catchup,
                                  MessageHistoryCatchupFailure failure) noexcept;
MessageHistoryApplyResult message_history_catchup_apply_page(
    MessageHistoryCatchup* catchup, MessageEventPump* pump, const MessageHistoryPage& page,
    ConstSlice<GatewayMessageEvent> messages,
    MessageContentAvailability empty_content_availability) noexcept;

enum class MessageHistoryOlderState : u8 {
    idle,
    request_required,
    awaiting_page,
    waiting_retry,
    exhausted,
    failed,
};

struct MessageHistoryOlder {
    u64 channel_id;
    u64 cursor_before;
    u64 retry_at_ms;
    u32 request_limit;
    u32 requests;
    u32 pages;
    u32 received;
    u32 inserted;
    u32 evicted;
    u32 failures;
    u32 demands;
    MessageHistoryOlderState state;
    MessageHistoryCatchupFailure last_failure;
    u8 transient_retries;
};

// After a successful latest-page apply, a nonempty short page proves both
// history edges. Preserve its oldest ID so eviction can reopen older paging.
inline void message_history_older_observe_catchup(
    MessageHistoryOlder& older, const MessageHistoryCatchup& catchup,
    const MessageHistoryPage& page) noexcept {
    if (catchup.latest_short && page.oldest_id != 0) {
        older.channel_id = catchup.channel_id;
        older.cursor_before = page.oldest_id;
        older.state = MessageHistoryOlderState::exhausted;
    }
}

// Prefetch trigger for older/newer history, measured outside the loading
// placeholder. There is no synthetic placeholder here.
bool message_history_older_prefetch_needed(const MessageTimeline& timeline) noexcept;
bool message_history_newer_prefetch_needed(const MessageTimeline& timeline) noexcept;
constexpr bool message_history_browsing_ready(MessageHistoryCatchupState state) noexcept {
    return state == MessageHistoryCatchupState::complete || state == MessageHistoryCatchupState::paused;
}

// Whether the channel's window may be kept for its next visit, which only
// catches up with after=. A window shorter than one page must reach the
// channel start; anything else is partial and must reload the latest page.
bool message_history_window_retainable(const MessageHistoryCatchup& catchup, const MessageHistoryOlder& older,
                                       const MessageTimeline& timeline, u64 channel_id) noexcept;

// Records one user request for older messages. Repeated demands are a
// no-op while a page/retry is active and after the channel beginning is known.
bool message_history_older_demand(MessageHistoryOlder* older, const MessageTimeline* timeline,
                                  u64 channel_id, u32 request_limit) noexcept;
bool message_history_older_take_query(MessageHistoryOlder* older,
                                      MessageHistoryQuery* query) noexcept;
bool message_history_older_tick(MessageHistoryOlder* older, u64 now_ms) noexcept;
bool message_history_older_fail(MessageHistoryOlder* older, MessageHistoryCatchupFailure failure) noexcept;
MessageHistoryApplyResult message_history_older_apply_page(
    MessageHistoryOlder* older, MessageTimeline* timeline, const MessageHistoryPage& page,
    ConstSlice<GatewayMessageEvent> messages,
    MessageContentAvailability empty_content_availability) noexcept;

enum class MessageHistoryRestDisposition : u8 {
    applied,
    retry_scheduled,
    failed,
};

enum class MessageHistoryRestError : u8 {
    none,
    invalid_input,
    transport,
    http_status,
    rate_limit,
    decode,
    apply,
};

enum class AttachmentRefreshState : u8 {
    idle,
    request_required,
    awaiting_response,
    waiting_retry,
};

struct AttachmentRefresh {
    u64 channel_id;
    u64 message_id;
    u64 attachment_id;
    u64 retry_at_ms;
    u32 source_message_revision;
    u32 requests;
    u32 failures;
    AttachmentRefreshState state;
};

struct MessageHistoryDecodeScratch {
    Slice<u32> page_structurals;
    Slice<char> content;
    Slice<GatewayMessageEvent> messages;
    Slice<GatewayMessageNamedUser> mention_users;
    MessageHistoryRichScratch rich;
    u32 maximum_depth;
};

struct ReplyReferenceRefresh {
    u64 channel_id;
    u64 completed_through_id;
    u64 message_id;
    u64 reference_id;
    u64 retry_at_ms;
    AttachmentRefreshState state;
};

bool message_history_reply_queue(ReplyReferenceRefresh* refresh, const MessageTimeline& timeline,
                                 u64 channel_id, u64 now_ms) noexcept;

struct MessageHistoryRestResult {
    MessageHistoryApplyResult apply;
    MessageHistoryDecodeResult decode;
    MessageHistoryRestDisposition disposition;
    MessageHistoryRestError error;

    constexpr bool accepted() const noexcept {
        return error == MessageHistoryRestError::none;
    }
};

MessageHistoryRestResult message_history_reply_accept_rest(
    ReplyReferenceRefresh* refresh, MessageTimeline* timeline, UserStore* users,
    const RestClientResponse& response, Utf8View body,
    const MessageHistoryDecodeScratch& scratch) noexcept;

void message_history_attachment_reset(AttachmentRefresh* refresh) noexcept;
bool message_history_attachment_tick(AttachmentRefresh* refresh, u64 now_ms) noexcept;
bool message_history_attachment_queue(AttachmentRefresh* refresh, u64 channel_id, u64 message_id,
                                      u32 message_revision, u64 attachment_id) noexcept;

// Accepts one around-message response and atomically replaces the loaded
// window around the requested target. Scrolling is left to the
// presentation layer after this model commit succeeds.
MessageHistoryRestResult message_history_jump_accept_rest(
    MessageTimeline* timeline, UserStore* users, GuildMemberStore* members,
    ChatStateStore* state, const RestClientResponse& response, Utf8View body,
    u64 target_channel_id, u64 target_message_id, const MessageHistoryDecodeScratch& scratch,
    MessageContentAvailability empty_content_availability) noexcept;

// History edges one around-target page proves (the official client's history
// fetch for a jump): fewer than half the limit older than the target reaches the channel
// start; fewer than half at/after it, or a newest row equal to the channel's
// last message, reaches the present. A known newer channel tail keeps the
// window detached even when the page looked complete.
struct MessageHistoryJumpEdges {
    bool more_before;
    bool more_after;
};
MessageHistoryJumpEdges message_history_jump_edges(ConstSlice<GatewayMessageEvent> page, u64 target_message_id,
                                                   u32 request_limit, u64 channel_last_message_id) noexcept;

// Commits the paging state for a window just replaced around a jump target.
// The previous channel session's catch-up and older cursors describe another
// window and are discarded. A window short of the present is detached: live
// creates stay deferred, and approaching its bottom pages `after` its newest
// row until a short page merges it with the live tail.
bool message_history_jump_settle(MessageHistoryCatchup* catchup, MessageHistoryOlder* older,
                                 MessageEventPump* pump, u64 channel_id,
                                 ConstSlice<GatewayMessageEvent> page, MessageHistoryJumpEdges edges) noexcept;

// "Jump to present" for a detached, settled window: discards it and
// begins a latest-page catch-up that re-attaches to the live tail. Returns
// false (nothing changed) when the window already reaches the present or a
// non-incremental load is still in progress; the caller then only scrolls.
bool message_history_jump_to_present(MessageHistoryCatchup* catchup, MessageHistoryOlder* older,
                                     MessageEventPump* pump, u64 channel_id, u32 request_limit) noexcept;

// Applies retry/reset policy for a signed attachment-URL refresh. A concurrent
// message mutation supersedes the response without turning it into a failure.
MessageHistoryRestResult message_history_attachment_accept_rest(
    AttachmentRefresh* refresh, MessageTimeline* timeline, const RestClientResponse& response,
    Utf8View body, const MessageHistoryDecodeScratch& scratch,
    MessageContentAvailability empty_content_availability) noexcept;

// UI-thread bridge from one REST completion into the catch-up state. 429
// deadlines have already been computed by the REST scheduler;
// successful bodies are decoded and applied before the response slot is reused.
MessageHistoryRestResult message_history_catchup_accept_rest(
    MessageHistoryCatchup* catchup, MessageEventPump* pump, const RestClientResponse& response,
    Utf8View body, const MessageHistoryDecodeScratch& scratch,
    MessageContentAvailability empty_content_availability, u64 now_ms) noexcept;

MessageHistoryRestResult message_history_older_accept_rest(
    MessageHistoryOlder* older, MessageTimeline* timeline, UserStore* users,
    ChatStateStore* state, GuildMemberStore* members, const RestClientResponse& response,
    Utf8View body, const MessageHistoryDecodeScratch& scratch,
    MessageContentAvailability empty_content_availability, u64 now_ms) noexcept;

} // namespace maicord
