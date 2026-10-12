#pragma once

#include "maicord/app/message_send_tracker.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/model/message_draft_store.hpp"
#include "maicord/protocol/attachment_upload.hpp"
#include "maicord/protocol/message_create.hpp"

namespace maicord {

inline constexpr u32 kMessageComposerRequestCapacity = 16;

enum class MessageComposerRequestPhase : u8 {
    queued,
    submitting,
    retry_wait,
    awaiting_gateway,
    failed,
    confirmed,
};

enum class MessageComposerControllerError : u8 {
    none,
    invalid_input,
    draft,
    request_capacity,
    text_capacity,
    tracker,
    not_found,
    busy,
    reply_unavailable,
};

struct MessageComposerRequest {
    u64 nonce;
    u64 channel_id;
    u64 reply_message_id;
    u64 retry_at_ms;
    u32 text_offset;
    u32 text_bytes;
    u32 sequence;
    u16 http_status;
    MessageComposerRequestPhase phase;
    bool occupied;
    u32 attachment_bytes;
    u64 author_id;
    u64 created_at_ms;
    u64 sticker_ids[kMessageCreateMaximumStickerCount];
    u32 sticker_count;
    // Per-reply policy; ordinary sends always store false.
    bool mention_reply_author;
    bool reply_unavailable;
    // A leading "@silent" was stripped from the stored text.
    bool suppress_notifications;
    MessageForwardReference forward_reference;
};

struct MessageComposerSubmit {
    // prepare_submit initializes scalar fields on every outcome; attachment
    // elements are initialized only below attachment_count. Views borrow the
    // request bank until its next queue/discard/completion mutation.
    Utf8View content;
    u64 nonce;
    u64 channel_id;
    u64 reply_message_id;
    UploadedAttachment attachments[kAttachmentUploadMaximumCount];
    u32 attachment_count;
    u64 sticker_ids[kMessageCreateMaximumStickerCount];
    u32 sticker_count;
    bool mention_reply_author;
    bool suppress_notifications;
    MessageForwardReference forward_reference;
};
static_assert(sizeof(MessageComposerRequest) == 128);
static_assert(sizeof(MessageComposerSubmit) == 904);

struct MessageComposerControllerResult {
    MessageTimelineInvalidation invalidation;
    u64 nonce;
    MessageComposerControllerError error;
    bool changed;

    constexpr bool ok() const noexcept {
        return error == MessageComposerControllerError::none;
    }
};

// Captures the reply target for producers whose content is stored elsewhere.
// Navigation and reply changes invalidate this token, including a cancel/reselect
// of the same target. It never borrows text or message-cache storage.
struct MessageComposerReplySnapshot {
    u64 channel_id;
    u64 message_id;
    u32 revision;
    bool mention_reply_author;
};
static_assert(sizeof(MessageComposerReplySnapshot) == 24);

struct MessageComposerController {
    MessageComposer* composer;
    MessageDraftStore* drafts;
    MessageTimeline* timeline;
    u8* request_text;
    u32 request_text_capacity;
    u32 request_text_used;
    u32 request_text_wasted;
    u32 request_count;
    u32 request_sequence;
    // The transport request stays reserved across a delivery completion or local
    // timeout. Only observing the response (or submission failure) releases it.
    u64 inflight_nonce;
    MessageSendTracker sends;
    MessageComposerRequest requests[kMessageComposerRequestCapacity];
    u64 reply_message_id;
    u32 reply_revision;
    bool mention_reply_author;
    u32 channel_switches;
    u32 draft_commits;
    u32 queued;
    u32 submitted;
    u32 failed;
    u32 completed;
    u32 compactions;
    u32 rejected;
    bool initialized;
};

bool message_composer_controller_initialize(
    MessageComposerController* controller, MessageComposer* composer,
    MessageDraftStore* drafts, MessageTimeline* timeline,
    Slice<u8> request_text) noexcept;
// Requires zero-filled request storage (a fresh committed allocation) and
// leaves it untouched, so its pages commit only when messages are sent.
bool message_composer_controller_initialize_sparse(
    MessageComposerController* controller, MessageComposer* composer,
    MessageDraftStore* drafts, MessageTimeline* timeline,
    Slice<u8> request_text) noexcept;
MessageComposerControllerResult message_composer_controller_commit_draft(
    MessageComposerController* controller) noexcept;
MessageComposerControllerResult message_composer_controller_select_channel(
    MessageComposerController* controller, u64 channel_id) noexcept;
MessageComposerControllerResult message_composer_controller_set_reply(
    MessageComposerController* controller, u64 reply_message_id,
    bool mention_reply_author = true) noexcept;
bool message_composer_controller_reply_snapshot(
    const MessageComposerController& controller, MessageComposerReplySnapshot* snapshot) noexcept;
// Call with server-confirmed deletion; cache absence is insufficient.
MessageComposerControllerResult message_composer_controller_clear_reply(
    MessageComposerController* controller, u64 channel_id,
    u64 reply_message_id) noexcept;
MessageComposerControllerResult message_composer_controller_queue_send(
    MessageComposerController* controller, bool channel_allows_send,
    u64 author_id, u64 now_unix_ms, u64 now_monotonic_ms,
    u64 reply_message_id = 0,
    ConstSlice<UploadedAttachment> attachments = {},
    ConstSlice<u64> attachment_sizes = {},
    MessageComposer* source = nullptr, ConstSlice<u64> sticker_ids = {},
    bool mention_reply_author = true,
    const MessageComposerReplySnapshot* reply_snapshot = nullptr) noexcept;
// A complete message from outside the composer (plugin actions). The reply
// target is explicit and independent of the active draft, which is left
// untouched; silent sends SUPPRESS_NOTIFICATIONS. The caller checked that the
// channel is sendable and the reply target is cached in it.
MessageComposerControllerResult message_composer_controller_queue_text(
    MessageComposerController* controller, MessageComposer* source, u64 author_id, u64 now_unix_ms,
    u64 now_monotonic_ms, u64 reply_message_id = 0, bool mention_reply_author = true,
    bool silent = false) noexcept;
MessageComposerControllerResult message_composer_controller_prepare_submit(
    MessageComposerController* controller, u64 now_monotonic_ms,
    MessageComposerSubmit* submit) noexcept;
// The destination and caption are independent of the active channel draft.
MessageComposerControllerResult message_composer_controller_queue_forward(
    MessageComposerController* controller, u64 destination_channel_id, Utf8View caption,
    MessageForwardReference source, u64 author_id, u64 now_unix_ms,
    u64 now_monotonic_ms) noexcept;
MessageComposerControllerResult message_composer_controller_cancel_unsent(
    MessageComposerController* controller, u64 nonce) noexcept;
MessageComposerControllerResult message_composer_controller_mark_submitted(
    MessageComposerController* controller, u64 nonce) noexcept;
MessageComposerControllerResult message_composer_controller_submit_failed(
    MessageComposerController* controller, u64 nonce,
    u64 now_monotonic_ms) noexcept;
MessageComposerControllerResult message_composer_controller_observe_http(
    MessageComposerController* controller, u16 http_status,
    MessageSendHttpDisposition disposition, u64 now_monotonic_ms,
    u64 retry_at_ms = 0) noexcept;
MessageComposerControllerResult message_composer_controller_retry(
    MessageComposerController* controller, u64 nonce,
    u64 now_monotonic_ms) noexcept;
MessageComposerControllerResult message_composer_controller_discard_failed(
    MessageComposerController* controller, u64 nonce) noexcept;
// Call only after an entire REST history page has decoded and applied. Missing
// optional nonces and other authors/channels do not acknowledge local sends.
bool message_composer_controller_observe_history(MessageComposerController* controller,
    ConstSlice<GatewayMessageEvent> messages, bool* changed) noexcept;
// After a successful HTTP response, reconcile its decoded message
// using that response's nonce. An omitted response nonce is allowed; author and
// channel must match. Earlier Gateway confirmation supersedes the REST body.
MessageComposerControllerResult message_composer_controller_observe_response(
    MessageComposerController* controller, u64 nonce, const GatewayMessageEvent& message,
    u64 active_channel_id) noexcept;
bool message_composer_controller_tick(MessageComposerController* controller,
                                      u64 now_monotonic_ms, bool* changed) noexcept;
// Rebuilds only the selected channel's loaded outbox rows. Does not submit,
// alter retries/nonces, or revive confirmed messages. Call at the
// newest end of history, not while browsing an older window.
bool message_composer_controller_restore_local(MessageComposerController* controller,
                                               u64 channel_id, bool* changed) noexcept;
u64 message_composer_controller_next_wake_at(
    const MessageComposerController& controller) noexcept;
bool message_composer_controller_take_completion(
    MessageComposerController* controller,
    MessageSendCompletion* completion) noexcept;
MessageSendTracker* message_composer_controller_send_tracker(
    MessageComposerController* controller) noexcept;

// The official client's confirmation before a large @everyone/@here mention. Counts are
// the channel member list's members (all groups) and non-offline members,
// or the thread's member count for threads.
enum class MessageMentionWarningPlace : u8 {
    channel,
    thread,
    forum_post,
};
struct MessageMentionWarningInput {
    Utf8View content;
    u32 member_count;
    u32 online_count;
    u32 thread_member_count;
    MessageMentionWarningPlace place;
    bool guild_channel;
    bool can_mention_everyone;
};
struct MessageMentionWarning {
    // Mentioned people, rounded down to the leading digit (1,234 -> 1,000).
    u32 count;
    bool required;
    // The first mention outside code is @here rather than @everyone.
    bool here;
};
inline constexpr u32 kMessageMentionWarningThreshold = 30;
inline constexpr u32 kMessageMentionWarningBodyBytes = 112;
// Pure: the first @everyone/@here outside inline and fenced code decides.
MessageMentionWarning message_mention_warning_evaluate(const MessageMentionWarningInput& input) noexcept;
u32 message_mention_warning_round(u32 count) noexcept;
// "By using @everyone, you are about to mention 1,000+ people. Are you sure?"
u32 message_mention_warning_body(const MessageMentionWarning& warning, MessageMentionWarningPlace place,
                                 Slice<char> output) noexcept;

} // namespace maicord
