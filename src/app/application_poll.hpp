#pragma once

#include "maicord/app/poll_controller.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/ui/poll_controls.hpp"
#include "maicord/ui/cursor.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
// Capture beside the native card action. An action never resolves a retired
// message slot, account or channel to a new current target.
struct ApplicationPollSource {
    PollControllerSource source;
    const void* ui;
    const MessageStore* store;
    u64 guild_id, parent_id, author_id;
    u16 parent_generation, message_type;
    u8 channel_type;
    u32 result_revision;
    u32 source_publication, source_index;
    UiPollSourceSurface surface;
};
// Loaded voter page of the open inspector.
struct ApplicationPollView {
    ConstSlice<ReactionUser> voters;
    u32 voter_page;
    bool voters_more;
};
// The scene owns its answer projection; this read-only query never allocates a
// selected owner merely to display a received card.
bool application_poll_state(u64 message_id, Slice<UiPollAnswer>, UiPollState*,
                            ApplicationPollSource* = nullptr) noexcept;
bool application_poll_search_state(u64 message_id, u32 source_publication, Slice<UiPollAnswer>,
                                   UiPollState*, ApplicationPollSource* = nullptr) noexcept;
bool application_poll_source_current(const ApplicationPollSource&) noexcept;
bool application_poll_action(const ApplicationPollSource&, const UiPollAction&) noexcept;
bool application_poll_open(const ApplicationPollSource&) noexcept;
bool application_poll_active() noexcept;
ApplicationPollView application_poll_view() noexcept;
bool application_poll_voters(u32 answer_id, bool more = false) noexcept;
bool application_poll_voters_previous() noexcept;
void application_poll_voters_close() noexcept;
bool application_poll_voters_active() noexcept;
bool application_poll_voters_action(const UiPollVotersAction&) noexcept;
// Received and selected timeline cards: hand on enabled answers/buttons only.
bool application_poll_cursor(UiPoint, UiCursor*) noexcept;
bool application_poll_window_message(UINT, WPARAM, LPARAM) noexcept;
bool application_poll_input(UiInputKind, u32 code, bool repeated) noexcept;
bool application_poll_consume_key_tail(UiInputKind, u32 code, bool repeated) noexcept;
void application_poll_close() noexcept;
bool application_poll_pending(u64 now_ms) noexcept;
bool application_poll_submit(u64 request_id, u64 now_ms) noexcept;
bool application_poll_response(const RestClientResponse&, Utf8View) noexcept;
bool application_poll_synchronize() noexcept;
u64 application_poll_next_wake() noexcept;
// Call after the authoritative lazy model has consumed a validated event.
void application_poll_observe(const GatewayMessagePollVoteEvent&, bool model_applied) noexcept;
void application_poll_message_observe(const GatewayMessageEvent&, bool model_applied) noexcept;
void application_poll_message_deleted(u64 channel_id, u64 message_id) noexcept;
void application_poll_gateway_reset() noexcept;
void application_poll_release() noexcept;
struct ApplicationPollMetrics {
    u32 owner_bytes, owner_committed_bytes, page_committed_bytes, revision;
    u64 publication;
    bool active, flight_active;
};
ApplicationPollMetrics application_poll_metrics() noexcept;
} // namespace maicord::app_detail
