#pragma once

#include "maicord/app/read_scope.hpp"
#include "maicord/app/rest_client.hpp"

namespace maicord::app_detail {
struct ApplicationScopedRead;
struct ApplicationScopedReadTicket {
    ReadScopeSource source;
    const void* account_ui;
    const void* account_instance;
    u64 account_scope, profile_id, self_user_id, serial;
    u32 read_generation;
};
struct ApplicationScopedReadView {
    ApplicationScopedReadTicket ticket;
    Utf8View status;
    u32 publication_revision, total, pending, completed, failed;
    bool available, running, error, can_retry;
};
bool application_scoped_read_capture(ReadScopeKind kind, u64 id,
                                     ApplicationScopedReadTicket* ticket) noexcept;
bool application_scoped_read_current(const ApplicationScopedReadTicket&) noexcept;
bool application_scoped_read_begin(const ApplicationScopedReadTicket&) noexcept;
bool application_scoped_read_retry(const ApplicationScopedReadTicket&) noexcept;
ApplicationScopedReadView application_scoped_read_view(ReadScopeKind kind, u64 id) noexcept;
u64 application_scoped_read_next_wake() noexcept;
bool application_scoped_read_submit(u64 request_id, u64 now_ms) noexcept;
bool application_scoped_read_response(const RestClientResponse&, Utf8View body, u64 now_ms) noexcept;
// After actual Core publication, validates only changed authority epochs.
void application_scoped_read_synchronize() noexcept;
void application_scoped_read_reset() noexcept;
void application_scoped_read_release() noexcept;
struct ApplicationScopedReadMetrics {
    u32 owner_bytes, owner_reserved_bytes, owner_committed_bytes;
    u32 queue_reserved_bytes, queue_committed_bytes;
    u32 targets_reserved_bytes, targets_committed_bytes;
    u32 plan_reserved_bytes, plan_committed_bytes;
    u32 jobs_reserved_bytes, jobs_committed_bytes;
    u32 flight_reserved_bytes, flight_committed_bytes;
    u32 candidate_reserved_bytes, candidate_committed_bytes;
    u32 plan_builds, target_checks, encodings, commits;
    u32 publication_revision;
    bool flight_active;
};
ApplicationScopedReadMetrics application_scoped_read_metrics() noexcept;
// Shared authority broker callbacks; no transport or UI action is admitted here.
void application_scoped_read_superseded(u64 channel_id, u64 latest_action_serial) noexcept;
void application_scoped_read_individual_result(u64 channel_id, u64 action_serial, bool success,
                                               bool retry) noexcept;
} // namespace maicord::app_detail
