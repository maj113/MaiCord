#pragma once

#include "maicord/ui/reaction_viewer.hpp"

namespace maicord {
enum class RestClientError : u8;
struct RestClientResponse;

inline constexpr u32 kReactionViewerMaximumRetries = 3;

enum class ReactionViewerRequestPhase : u8 {
    idle,
    required,
    submitted,
    retry_wait,
};

enum class ReactionViewerControllerError : u8 {
    none,
    invalid_input,
    viewer,
    state,
    transport,
    http,
    decode,
};

struct ReactionViewerQuery {
    Utf8View emoji_name;
    u64 channel_id;
    u64 message_id;
    u64 emoji_id;
    u64 after_user_id;
    u64 epoch;
    MessageReactionType type;
    u32 limit;
};

struct ReactionViewerControllerResult {
    ReactionViewerControllerError error;
    bool changed;
    bool stale;
    bool retry_scheduled;

    constexpr bool ok() const noexcept {
        return error == ReactionViewerControllerError::none;
    }
};

struct ReactionViewerController {
    ReactionViewer* viewer;
    ReactionUser* page_users;
    u32 page_user_capacity;
    u64 epoch;
    u64 request_epoch;
    u64 retry_at_ms;
    u32 retry_attempt;
    u32 requests;
    u32 pages;
    u32 stale_responses;
    u32 retries;
    u32 failures;
    ReactionUsersDecodeError last_decode_error;
    JsonScanError last_scan_error;
    JsonReadError last_read_error;
    RestClientError last_transport_error;
    u32 last_decode_offset;
    u16 last_http_status;
    ReactionViewerRequestPhase phase;
    bool initialized;
};

bool reaction_viewer_controller_initialize(
    ReactionViewerController* controller, ReactionViewer* viewer,
    Slice<ReactionUser> page_users) noexcept;
ReactionViewerControllerResult reaction_viewer_controller_open(
    ReactionViewerController* controller, u64 guild_id, u64 channel_id,
    u64 message_id, const MessageRichStore& rich_store,
    const MessageRichRecord& rich, UiRect viewport) noexcept;
ReactionViewerControllerResult reaction_viewer_controller_close(
    ReactionViewerController* controller) noexcept;
ReactionViewerControllerResult reaction_viewer_controller_select(
    ReactionViewerController* controller, u32 aggregate_index) noexcept;
bool reaction_viewer_controller_prepare_request(
    ReactionViewerController* controller, u64 now_monotonic_ms,
    ReactionViewerQuery* query) noexcept;
ReactionViewerControllerResult reaction_viewer_controller_mark_submitted(
    ReactionViewerController* controller, u64 epoch) noexcept;
ReactionViewerControllerResult reaction_viewer_controller_submit_failed(
    ReactionViewerController* controller, u64 now_monotonic_ms) noexcept;
ReactionViewerControllerResult reaction_viewer_controller_accept_response(
    ReactionViewerController* controller,
    const RestClientResponse& response, ConstSlice<u8> body,
    Slice<u32> page_structural_scratch,
    Slice<u32> object_structural_scratch) noexcept;
ReactionViewerControllerResult reaction_viewer_controller_request_more(
    ReactionViewerController* controller) noexcept;
u64 reaction_viewer_controller_next_wake_at(
    const ReactionViewerController& controller) noexcept;

} // namespace maicord
