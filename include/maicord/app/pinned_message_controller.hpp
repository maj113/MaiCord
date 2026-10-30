#pragma once

#include "maicord/protocol/message_pins.hpp"

namespace maicord {

inline constexpr u32 kPinnedMessageAuthorBytes = 128;
inline constexpr u32 kPinnedMessageAvatarBytes = 64;
inline constexpr u32 kPinnedMessageExcerptBytes = 1024;
inline constexpr u32 kPinnedMessageCursorHistory = 64;

struct PinnedMessageScope {
    u64 account_scope{}, profile_id{}, self_user_id{}, channel_id{};
    u16 channel_generation{};
};
enum class PinnedMessagePhase : u8 { empty, loading, ready, mutating, failed, retired };
enum class PinnedMessageFailure : u8 {
    none, permissions, response, rejected, ambiguous, rate_limited, cancelled, admission, exhausted, capacity
};
enum class PinnedMessageCompletionKind : u8 {
    success, rejected, permission_lost, ambiguous, rate_limited, cancelled
};
struct PinnedMessageCompletion {
    PinnedMessageCompletionKind kind{};
    u64 retry_at_ms{};
};
// Excerpts are stored by value so they survive shared REST decode-buffer reuse. They are never
// inserted into the message timeline. The host formats the two timestamps and
// rich counts for presentation; empty content remains a valid pinned message.
struct PinnedMessageRow {
    u64 channel_id{}, message_id{}, author_id{}, timestamp_ms{}, pinned_at_ms{};
    char author[kPinnedMessageAuthorBytes]{}, avatar_hash[kPinnedMessageAvatarBytes]{},
         content[kPinnedMessageExcerptBytes]{};
    u16 author_bytes{}, avatar_bytes{}, content_bytes{}, type{};
    u32 attachment_count{}, embed_count{}, sticker_count{}, component_count{};
    bool forwarded{}, excerpt_truncated{}, rich_truncated{};
};
Utf8View pinned_message_author(const PinnedMessageRow&) noexcept;
Utf8View pinned_message_avatar(const PinnedMessageRow&) noexcept;
Utf8View pinned_message_content(const PinnedMessageRow&) noexcept;

struct PinnedMessageRequest {
    PinnedMessageScope scope{};
    MessagePinsRequest payload{};
    u64 revision{};
    bool ready{};
};
// One complete page of at most twenty-five pins, with Older/Newer navigation
// and copied cursors. Refresh returns to the first page. No initialization
// allocates memory: keep this relatively large value in the lazily created panel.
// Cancelling keeps the outstanding request ID until its response is taken.
struct PinnedMessageController {
    PinnedMessageRow rows[kMessagePinsPageLimit]{};
    PinnedMessageScope scope{};
    MessagePinsPage page{};
    MessagePinsRequest pending{};
    MessagePinsCursor cursors[kPinnedMessageCursorHistory]{};
    u64 revision{}, remote_revision{}, page_remote_revision{}, submitted_remote_revision{}, request_id{}, retry_at_ms{},
        selected_message_id{};
    PinnedMessagePhase phase{};
    PinnedMessageFailure failure{};
    u32 page_index{}, requested_page_index{}, cursor_count{};
    u8 rate_attempts{};
    bool active{}, available{}, can_pin{}, permission_denied{}, read_denied{}, authoritative{}, has_page{}, queued{}, has_previous{};
};
static_assert(sizeof(PinnedMessageController) <= 36 * 1024);
bool pinned_message_scope_equal(PinnedMessageScope, PinnedMessageScope) noexcept;
bool pinned_message_controller_open(PinnedMessageController*, PinnedMessageScope, bool available,
                                    bool can_pin) noexcept;
void pinned_message_controller_authority(PinnedMessageController*, PinnedMessageScope,
                                         bool available, bool can_pin) noexcept;
// Only a fresh permission update clears a server read/pin-permission denial.
void pinned_message_controller_reauthorize(PinnedMessageController*, PinnedMessageScope,
                                           bool available, bool can_pin) noexcept;
bool pinned_message_controller_refresh(PinnedMessageController*) noexcept;
bool pinned_message_controller_next(PinnedMessageController*) noexcept;
bool pinned_message_controller_previous(PinnedMessageController*) noexcept;
bool pinned_message_controller_retry(PinnedMessageController*) noexcept;
// Supports timeline context actions too: message_id need not be in this page.
// An unsent initial fetch can be replaced by a mutation, which always repages.
bool pinned_message_controller_mutate(PinnedMessageController*, u64 message_id, bool pin) noexcept;
PinnedMessageRequest pinned_message_controller_request(const PinnedMessageController&, u64 now_ms) noexcept;
bool pinned_message_controller_submitted(PinnedMessageController*, const PinnedMessageRequest&,
                                         u64 request_id) noexcept;
// A busy shared REST worker leaves the queued request intact.
bool pinned_message_controller_admission_failed(PinnedMessageController*, const PinnedMessageRequest&,
                                                bool busy) noexcept;
bool pinned_message_controller_invalidate(PinnedMessageController*, PinnedMessageScope) noexcept;
// Reports response consumption, separately from server mutation success.
// Page/items are required only for a successfully decoded fetch response.
bool pinned_message_controller_complete(PinnedMessageController*, PinnedMessageScope, u64 request_id,
                                        PinnedMessageCompletion, const MessagePinsPage*,
                                        ConstSlice<MessagePin>, u64 now_ms) noexcept;
const PinnedMessageRow* pinned_message_controller_row(const PinnedMessageController&, u32 index) noexcept;
bool pinned_message_controller_select(PinnedMessageController*, u64 message_id) noexcept;
bool pinned_message_controller_mutation_pending(const PinnedMessageController&) noexcept;
void pinned_message_controller_cancel(PinnedMessageController*) noexcept;
u64 pinned_message_controller_next_wake(const PinnedMessageController&) noexcept;

} // namespace maicord
