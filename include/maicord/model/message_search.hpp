#pragma once

#include "maicord/protocol/message_search.hpp"

namespace maicord {

enum class MessageSearchState : u8 {
    empty,
    ready_to_submit,
    fetching,
    indexing,
    rate_limited,
    ready,
    failed,
    cancelled
};
enum class MessageSearchFailure : u8 {
    none,
    transport,
    http,
    decode,
    retry_limit,
    deadline,
    admission
};
struct MessageSearchToken {
    u64 account_id;
    u64 scope_id;
    u64 generation;
    u64 request_id;
};
struct MessageSearchController {
    MessageSearchQuery query;
    MessageSearchToken token;
    u64 retry_at_ms;
    u64 total_results;
    u32 loaded_groups;
    u16 http_status;
    u8 indexing_retries;
    MessageSearchState state;
    MessageSearchFailure failure;
    bool has_results;
    // Set when the tabs endpoint was unavailable and the query moved to the legacy GET.
    bool endpoint_fallback;
};

bool message_search_begin(MessageSearchController*, u64 account_id, const MessageSearchQuery&) noexcept;
bool message_search_submit(MessageSearchController*, u64 request_id, u64 now_ms,
                           MessageSearchToken*) noexcept;
bool message_search_accepts(const MessageSearchController*, const MessageSearchToken&) noexcept;
// Busy admission waits for the existing REST owner's notification, not a timer.
bool message_search_submission_failed(MessageSearchController*, const MessageSearchToken&,
                                      bool busy) noexcept;
bool message_search_complete(MessageSearchController*, const MessageSearchToken&, u16 http_status,
                             bool transport_ok, bool decoded, const MessageSearchPage*, u64 now_ms,
                             u64 retry_after_ms) noexcept;
bool message_search_page(MessageSearchController*, u32 offset) noexcept;
// Moves an in-flight tabs request to the legacy GET; all-DM search has no GET form.
bool message_search_fallback(MessageSearchController*, const MessageSearchToken&) noexcept;
u64 message_search_cancel(MessageSearchController*) noexcept;
u64 message_search_next_wake(const MessageSearchController*) noexcept;

} // namespace maicord
