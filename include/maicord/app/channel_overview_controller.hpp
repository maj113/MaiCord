#pragma once

#include "maicord/protocol/channel_overview.hpp"

namespace maicord {
struct ChannelOverviewScope {
    u64 account_scope{}, profile_id{}, self_user_id{}, guild_id{}, channel_id{};
    u16 channel_generation{};
};
enum class ChannelOverviewPhase : u8 {
    empty,
    loading,
    ready,
    saving,
    failed,
    refetch,
    conflict,
    permission_lost,
    retired
};
enum class ChannelOverviewFailure : u8 {
    none,
    input,
    permissions,
    response,
    rejected,
    ambiguous,
    rate_limited,
    cancelled,
    conflict
};
enum class ChannelOverviewCompletionKind : u8 {
    success,
    rejected,
    permission_lost,
    ambiguous,
    rate_limited,
    cancelled
};
struct ChannelOverviewCompletion {
    ChannelOverviewCompletionKind kind{};
    u64 retry_at_ms{};
};
struct ChannelOverviewRequest {
    ChannelOverviewScope scope{};
    ChannelOverviewPatch patch{}; // Borrows the locked draft until the request is accepted.
    u64 revision{}, remote_revision{};
    ChannelOverviewOperation operation{};
    bool ready{};
};
// Two copied banks only. Saving locks edits, so no third bank is needed.
// Cancelled responses must be taken before the controller can be rebound.
struct ChannelOverviewController {
    static constexpr bool kZeroDefault = true; // Every member defaults to zero: reset_value clears bytes.
    ChannelOverviewSnapshot baseline{}, draft{};
    ChannelOverviewScope scope{};
    u64 revision{}, remote_revision{}, request_id{}, submitted_remote_revision{}, retry_at_ms{};
    ChannelOverviewOperation operation{};
    ChannelOverviewPhase phase{};
    ChannelOverviewFailure failure{};
    u8 dirty_fields{}, rate_attempts{}, conflict_fields{};
    bool active{}, available{}, can_manage{}, authoritative{}, has_baseline{}, queued{},
        permission_denied{};
};
bool channel_overview_scope_equal(ChannelOverviewScope, ChannelOverviewScope) noexcept;
bool channel_overview_controller_open(ChannelOverviewController*, ChannelOverviewScope, bool available,
                                      bool can_manage) noexcept;
// A different scope cancels without overwriting the outstanding request ID.
void channel_overview_controller_authority(ChannelOverviewController*, ChannelOverviewScope, bool available,
                                           bool can_manage) noexcept;
// Only a fresh permission observation may clear a known server denial.
void channel_overview_controller_reauthorize(ChannelOverviewController*, ChannelOverviewScope,
                                             bool available, bool can_manage) noexcept;
bool channel_overview_controller_edit(ChannelOverviewController*, const ChannelOverviewPatch&) noexcept;
bool channel_overview_controller_save(ChannelOverviewController*) noexcept;
bool channel_overview_controller_reset(ChannelOverviewController*) noexcept;
bool channel_overview_controller_reload(ChannelOverviewController*) noexcept;
// Accept the latest remote baseline and keep the reviewed draft.
bool channel_overview_controller_rebase(ChannelOverviewController*) noexcept;
ChannelOverviewRequest channel_overview_controller_request(const ChannelOverviewController&,
                                                           u64 now_ms) noexcept;
bool channel_overview_controller_submitted(ChannelOverviewController*, const ChannelOverviewRequest&,
                                           u64 request_id) noexcept;
// A busy result leaves the queued request intact. Other synchronous failures
// cancel only the matching queued revision and keep the editable draft.
bool channel_overview_controller_admission_failed(ChannelOverviewController*, const ChannelOverviewRequest&,
                                                  ChannelOverviewFailure) noexcept;
// Mailbox channel updates currently omit overview fields. Invalidate on their
// scoped update and fetch; never attempt to decode binary mailbox data as JSON.
bool channel_overview_controller_invalidate(ChannelOverviewController*, ChannelOverviewScope) noexcept;
// Reports response consumption, separate from server mutation success.
bool channel_overview_controller_complete(ChannelOverviewController*, ChannelOverviewScope, u64 request_id,
                                          ChannelOverviewCompletion, const ChannelOverviewSnapshot*,
                                          u64 now_ms) noexcept;
void channel_overview_controller_cancel(ChannelOverviewController*) noexcept;
u64 channel_overview_controller_next_wake(const ChannelOverviewController&) noexcept;
static_assert(sizeof(ChannelOverviewScope) == 48);
static_assert(sizeof(ChannelOverviewController) == 9'160);
} // namespace maicord
