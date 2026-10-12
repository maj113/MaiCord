#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/thread_actions.hpp"

namespace maicord::app_detail {
enum class ApplicationThreadActionEnqueueResult : u8 {
    accepted,
    coalesced,
    busy,
    permission,
    stale,
    invalid,
    storage
};
enum class ApplicationThreadActionPhase : u8 {
    idle,
    queued,
    submitting,
    retry_wait,
    settled,
    failed,
    cancelled
};
enum class ApplicationThreadMembershipStatus : u8 {
    unknown,
    pending,
    joined,
    not_joined,
    failed,
    stale
};
enum ApplicationThreadActionFlag : u8 {
    thread_action_fetch = 1u << 0,
    thread_action_join = 1u << 1,
    thread_action_leave = 1u << 2,
    thread_action_unarchive = 1u << 3,
    thread_action_retry = 1u << 4,
    thread_action_cancel = 1u << 5
};
// Actor/source snapshot held by value. UI currentness includes the committed
// version and channel revision; membership waiters keep their serial independently of repaint.
struct ApplicationThreadActionTicket {
    u64 serial, account_scope, profile_id, self_user_id, thread_id;
    usize account_instance;
    u32 channel_revision, publication_revision;
    u16 channel_generation;
};
struct ApplicationThreadActionView {
    ApplicationThreadActionTicket ticket;
    Utf8View status;
    ApplicationThreadMembershipStatus membership;
    ApplicationThreadActionPhase phase;
    ThreadActionOperation operation;
    u8 actions;
    bool archived, archived_known, locked, locked_known, error;
};
ApplicationThreadActionView application_thread_actions_view(u64 thread_id) noexcept;
bool application_thread_actions_current(const ApplicationThreadActionTicket&) noexcept;
ApplicationThreadActionEnqueueResult application_thread_actions_activate(const ApplicationThreadActionTicket&,
                                                                     ThreadActionOperation) noexcept;
ApplicationThreadActionEnqueueResult application_thread_actions_fetch(
    u64 thread_id, ApplicationThreadActionTicket* ticket) noexcept;
ApplicationThreadMembershipStatus application_thread_actions_membership(
    const ApplicationThreadActionTicket&) noexcept;
bool application_thread_actions_retry(const ApplicationThreadActionTicket&) noexcept;
bool application_thread_actions_cancel(const ApplicationThreadActionTicket&) noexcept;
Utf8View application_thread_actions_admission_text(ApplicationThreadActionEnqueueResult) noexcept;
bool application_thread_actions_admission_status(const ApplicationThreadActionTicket&,
                                                 ApplicationThreadActionEnqueueResult) noexcept;
bool application_thread_actions_pending(u64 now_ms) noexcept;
bool application_thread_actions_submit(u64 request_id, u64 now_ms) noexcept;
bool application_thread_actions_response(const RestClientResponse&, Utf8View body) noexcept;
bool application_thread_actions_synchronize() noexcept;
u64 application_thread_actions_next_wake() noexcept;
void application_thread_actions_release() noexcept;
u32 application_thread_actions_publication_revision() noexcept;
struct ApplicationThreadActionMetrics {
    u32 owner_bytes, owner_committed_bytes, entry_count, publication_revision;
    bool flight_active;
};
ApplicationThreadActionMetrics application_thread_actions_metrics() noexcept;
// Fresh dispatch checks also used when queued or retried send/edit/delete requests are accepted.
bool application_thread_actions_channel_readable(u64 channel_id) noexcept;
bool application_thread_actions_channel_sendable(u64 channel_id) noexcept;
} // namespace maicord::app_detail
