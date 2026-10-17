#pragma once

#include "maicord/model/message_store.hpp"
#include "maicord/protocol/reaction_users.hpp"

namespace maicord {
struct RestClientResponse;
inline constexpr u32 kPollControllerMaximumAnswers = 10;
inline constexpr u32 kPollControllerMaximumRetries = 3;
struct PollControllerActor {
    u64 account_scope{}, profile_id{}, self_user_id{};
    usize instance{};
};
struct PollControllerSource {
    PollControllerActor actor{};
    u64 channel_id{}, message_id{}, channel_epoch{};
    MessageHandle handle{};
    u32 poll_content_revision{};
    u16 channel_generation{};
};
// Caller normalizes the current owned poll record. Missing results mean
// mine_known=false, never an invented empty vote or zero aggregate count.
struct PollControllerSnapshot {
    PollControllerSource source{};
    u64 expiry_ms{};
    u32 answer_ids[kPollControllerMaximumAnswers]{};
    u16 mine_mask{};
    u8 answer_count{};
    bool mine_known{}, multiple{}, finalized{}, can_vote{}, can_inspect{};
};
enum class PollControllerWorkKind : u8 { none, vote, voters, refresh };
enum class PollControllerVotePhase : u8 { idle, queued, sending, awaiting_authority, uncertain, failed };
enum class PollControllerError : u8 {
    none, invalid_input, stale, busy, forbidden, expired, finalized, selection,
    transport, http, decode, uncertain, exhausted,
};
struct PollControllerCommand {
    PollControllerSnapshot snapshot{};
    u64 epoch{}, after_user_id{}, due_at_ms{};
    u32 answer_id{}, limit{};
    u16 desired_mask{};
    u8 retries{};
    PollControllerWorkKind kind{};
};
struct PollControllerTicket { u64 request_id{}, epoch{}; };
struct PollControllerFlight {
    PollControllerCommand command{};
    PollControllerTicket ticket{};
    bool active{}, retired{};
};
struct PollControllerResult {
    PollControllerError error{};
    bool changed{}, stale{}, refresh_required{}, page_ready{};
    bool ok() const noexcept { return error == PollControllerError::none; }
};
struct PollController {
    PollControllerSnapshot snapshot{};
    PollControllerCommand pending{};
    PollControllerFlight flight{};
    u64 epoch{}, after_user_id{};
    u32 voter_answer_id{};
    u16 desired_mask{};
    PollControllerVotePhase vote_phase{};
    PollControllerError last_error{};
    PollControllerError vote_barrier{};
    bool initialized{}, source_active{}, pending_ready{}, desired_active{};
    bool refresh_required{}, voters_more{}, voters_failed{};
};
bool poll_controller_source_equal(const PollControllerSource&, const PollControllerSource&) noexcept;
void poll_controller_initialize(PollController*) noexcept;
// Actor/source reset keeps the copied old in-flight request until its response arrives.
PollControllerResult poll_controller_observe(PollController*, const PollControllerSnapshot&,
                                            bool authoritative_refresh = false) noexcept;
void poll_controller_close(PollController*) noexcept;
PollControllerResult poll_controller_expire(PollController*, u64 unix_now_ms) noexcept;
PollControllerResult poll_controller_queue_vote(PollController*, u16 desired_mask,
                                                u64 now_ms, u64 unix_now_ms) noexcept;
// Opening a different answer may supersede an old read. Its copied in-flight
// request finishes before the latest selected page can go to the REST worker.
PollControllerResult poll_controller_queue_voter_page(PollController*, u32 answer_id,
    u64 after_user_id, u32 limit, u64 now_ms) noexcept;
PollControllerResult poll_controller_queue_refresh(PollController*, u64 now_ms) noexcept;
bool poll_controller_prepare(const PollController&, u64 now_ms, u64 unix_now_ms,
                              PollControllerCommand*) noexcept;
bool poll_controller_admit(PollController*, const PollControllerCommand&, u64 request_id,
                           u64 now_ms, u64 unix_now_ms, PollControllerTicket*) noexcept;
// Submit failure before transport accepted the request leaves the copied
// pending command available. No in-flight request or uncertain mutation is created.
void poll_controller_submit_failed(PollController*, u64 retry_at_ms) noexcept;
// Decode into caller-owned page buffers before completing a voter response.
// Only page_ready permits append. Vote success still requires server-confirmed
// own-vote convergence; transport/5xx ambiguity requests refresh, never replay.
PollControllerResult poll_controller_complete(PollController*, PollControllerTicket,
    const RestClientResponse&, u64 now_ms, u64 unix_now_ms,
    bool body_valid = true, ConstSlice<ReactionUser> voters = {}) noexcept;
u64 poll_controller_next_wake(const PollController&) noexcept;
} // namespace maicord
