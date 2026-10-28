#include "maicord/app/poll_controller.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {
namespace {
bool actor_equal(PollControllerActor a, PollControllerActor b) noexcept {
    return a.account_scope==b.account_scope && a.profile_id==b.profile_id &&
           a.self_user_id==b.self_user_id && a.instance==b.instance;
}
bool snapshot_valid(const PollControllerSnapshot& p) noexcept {
    const auto& s=p.source;
    if (!s.actor.account_scope || !s.actor.profile_id || !s.actor.self_user_id || !s.actor.instance ||
        !s.channel_id || !s.message_id || !s.channel_epoch || !s.handle.generation ||
        !s.poll_content_revision || !s.channel_generation || !p.answer_count ||
        p.answer_count>kPollControllerMaximumAnswers || p.mine_mask>>p.answer_count ||
        (!p.mine_known && p.mine_mask)) return false;
    for (u32 i=0;i<p.answer_count;++i) {
        if (!p.answer_ids[i]) return false;
        for (u32 j=0;j<i;++j) if (p.answer_ids[j]==p.answer_ids[i]) return false;
    }
    return true;
}
bool answers_equal(const PollControllerSnapshot& a,const PollControllerSnapshot& b) noexcept {
    if (a.answer_count!=b.answer_count || a.multiple!=b.multiple) return false;
    for (u32 i=0;i<a.answer_count;++i) if (a.answer_ids[i]!=b.answer_ids[i]) return false;
    return true;
}
bool advance(PollController& c) noexcept {
    if (c.epoch==~u64{0}) return false;
    ++c.epoch; return true;
}
void drop_pending(PollController& c) noexcept {
    reset_value(&c.pending); c.pending_ready=false;
}
PollControllerResult result(PollController& c,PollControllerError error=PollControllerError::none,
                            bool changed=false,bool stale=false,bool page=false) noexcept {
    if (error!=PollControllerError::none && !stale) c.last_error=error;
    return {error,changed,stale,c.refresh_required,page};
}
PollControllerError vote_allowed(const PollController& c,u16 mask,u64 unix_now) noexcept {
    const auto& p=c.snapshot;
    if (!c.source_active || !snapshot_valid(p) || !unix_now) return PollControllerError::invalid_input;
    if (c.vote_barrier!=PollControllerError::none) return c.vote_barrier;
    if (!p.can_vote || !p.can_inspect) return PollControllerError::forbidden;
    if (p.finalized) return PollControllerError::finalized;
    if (p.expiry_ms && unix_now>=p.expiry_ms) return PollControllerError::expired;
    if ((mask>>p.answer_count) || (!p.multiple && mask && (mask&(mask-1))) ||
        (!mask && (!p.mine_known || !p.mine_mask)) ||
        (mask && p.mine_known && p.mine_mask && mask!=p.mine_mask &&
         (!c.desired_active || mask!=c.desired_mask))) return PollControllerError::selection;
    return PollControllerError::none;
}
bool command_equal(const PollControllerCommand& a,const PollControllerCommand& b) noexcept {
    return a.epoch==b.epoch && a.kind==b.kind && a.desired_mask==b.desired_mask &&
        a.answer_id==b.answer_id && a.after_user_id==b.after_user_id && a.limit==b.limit &&
        poll_controller_source_equal(a.snapshot.source,b.snapshot.source) &&
        answers_equal(a.snapshot,b.snapshot);
}
bool flight_current(const PollController& c,const PollControllerCommand& command,u64 unix_now) noexcept {
    if (!c.source_active || !poll_controller_source_equal(command.snapshot.source,c.snapshot.source) ||
        !answers_equal(command.snapshot,c.snapshot) || !c.snapshot.can_inspect) return false;
    return command.kind!=PollControllerWorkKind::vote ||
        vote_allowed(c,command.desired_mask,unix_now)==PollControllerError::none;
}
void retire_source(PollController& c) noexcept {
    if (c.flight.active) c.flight.retired=true;
    drop_pending(c); c.desired_active=false; c.desired_mask=0;
    c.vote_phase=PollControllerVotePhase::idle; c.refresh_required=false;
    c.vote_barrier=PollControllerError::none;
    c.after_user_id=0; c.voter_answer_id=0; c.voters_more=false; c.voters_failed=false;
}
}
bool poll_controller_source_equal(const PollControllerSource& a,const PollControllerSource& b) noexcept {
    return actor_equal(a.actor,b.actor) && a.channel_id==b.channel_id && a.message_id==b.message_id &&
        a.channel_epoch==b.channel_epoch && a.handle.slot==b.handle.slot &&
        a.handle.generation==b.handle.generation && a.poll_content_revision==b.poll_content_revision &&
        a.channel_generation==b.channel_generation;
}
void poll_controller_initialize(PollController* c) noexcept {
    if (c) { reset_value(c); c->initialized=true; }
}
void poll_controller_close(PollController* c) noexcept {
    if (!c || !c->initialized) return;
    retire_source(*c); c->source_active=false; reset_value(&c->snapshot); advance(*c);
}
PollControllerResult poll_controller_observe(PollController* c,const PollControllerSnapshot& p,
                                            bool authoritative_refresh) noexcept {
    if (!c || !c->initialized || !snapshot_valid(p))
        return {PollControllerError::invalid_input};
    const bool replaced=!c->source_active || !poll_controller_source_equal(c->snapshot.source,p.source) ||
                        !answers_equal(c->snapshot,p);
    if (replaced) {
        if (!advance(*c)) return result(*c,PollControllerError::exhausted);
        retire_source(*c);
    }
    c->snapshot=p; c->source_active=true;
    if (authoritative_refresh && c->vote_barrier==PollControllerError::forbidden && p.can_vote &&
        p.mine_known && !p.finalized) c->vote_barrier=PollControllerError::none;
    if (!p.can_inspect) {
        if (c->flight.active) c->flight.retired=true;
        drop_pending(*c);
    }
    if (!p.can_inspect || p.finalized || !p.can_vote) {
        if (c->pending_ready && c->pending.kind==PollControllerWorkKind::vote) drop_pending(*c);
        if (c->flight.active && c->flight.command.kind==PollControllerWorkKind::vote)
            c->flight.retired=true;
        c->desired_active=false; c->vote_phase=PollControllerVotePhase::idle;
        c->refresh_required=false;
    } else if (c->desired_active && p.mine_known &&
               (p.mine_mask==c->desired_mask || authoritative_refresh)) {
        // Authority without the wanted vote rolls it back with a notice.
        const bool landed=p.mine_mask==c->desired_mask;
        c->desired_active=false;
        c->vote_phase=landed?PollControllerVotePhase::idle:PollControllerVotePhase::failed;
        c->refresh_required=false;
        c->last_error=landed?PollControllerError::none:PollControllerError::transport;
        if (c->pending_ready && c->pending.kind==PollControllerWorkKind::vote) drop_pending(*c);
    } else if (authoritative_refresh) {
        c->refresh_required=false;
        if (c->desired_active) {
            c->vote_phase=PollControllerVotePhase::uncertain;
            c->last_error=PollControllerError::uncertain;
        } else if (p.mine_known && c->vote_phase==PollControllerVotePhase::uncertain) {
            c->vote_phase=PollControllerVotePhase::idle;
            c->last_error=PollControllerError::none;
        }
    }
    return result(*c,PollControllerError::none,true);
}
PollControllerResult poll_controller_expire(PollController* c,u64 unix_now) noexcept {
    if (!c || !c->initialized || !unix_now) return {PollControllerError::invalid_input};
    if (!c->source_active || !c->snapshot.expiry_ms || unix_now<c->snapshot.expiry_ms)
        return result(*c);
    const bool changed=c->vote_barrier!=PollControllerError::expired;
    if (c->pending_ready && c->pending.kind==PollControllerWorkKind::vote) drop_pending(*c);
    if (c->flight.active && c->flight.command.kind==PollControllerWorkKind::vote)
        c->flight.retired=true;
    c->desired_active=false; c->vote_phase=PollControllerVotePhase::failed;
    c->vote_barrier=PollControllerError::expired;
    if (changed && c->snapshot.can_inspect && !c->snapshot.finalized) c->refresh_required=true;
    return result(*c,PollControllerError::expired,changed);
}
PollControllerResult poll_controller_queue_vote(PollController* c,u16 mask,u64 now,u64 unix_now) noexcept {
    if (!c || !c->initialized || !now) return {PollControllerError::invalid_input};
    if (c->vote_phase==PollControllerVotePhase::uncertain)
        return result(*c,PollControllerError::uncertain);
    if (c->flight.active || c->pending_ready || c->desired_active)
        return result(*c,PollControllerError::busy);
    const auto allowed=vote_allowed(*c,mask,unix_now);
    if (allowed!=PollControllerError::none) return result(*c,allowed);
    if (!c->snapshot.mine_known) {
        c->vote_phase=PollControllerVotePhase::uncertain; c->refresh_required=true;
        return result(*c,PollControllerError::uncertain,true);
    }
    if (c->snapshot.mine_mask==mask) return result(*c);
    if (!advance(*c)) return result(*c,PollControllerError::exhausted);
    reset_value(&c->pending); c->pending.snapshot=c->snapshot; c->pending.kind=PollControllerWorkKind::vote;
    c->pending.desired_mask=mask; c->pending.epoch=c->epoch; c->pending.due_at_ms=now;
    c->pending_ready=true; c->desired_active=true; c->desired_mask=mask;
    c->vote_phase=PollControllerVotePhase::queued; c->last_error=PollControllerError::none;
    return result(*c,PollControllerError::none,true);
}
PollControllerResult poll_controller_queue_voter_page(PollController* c,u32 answer,u64 after,
                                                      u32 limit,u64 now) noexcept {
    if (!c || !c->initialized || !c->source_active || !now || !limit ||
        limit>kReactionUsersMaximumPage) return {PollControllerError::invalid_input};
    if (!c->snapshot.can_inspect) return result(*c,PollControllerError::forbidden);
    bool found=false;
    for (u32 i=0;i<c->snapshot.answer_count;++i) found|=c->snapshot.answer_ids[i]==answer;
    if (!found) return result(*c,PollControllerError::selection);
    if (c->pending_ready && c->pending.kind!=PollControllerWorkKind::voters)
        return result(*c,PollControllerError::busy);
    if (!advance(*c)) return result(*c,PollControllerError::exhausted);
    if (c->flight.active && c->flight.command.kind==PollControllerWorkKind::voters)
        c->flight.retired=true;
    reset_value(&c->pending); c->pending.snapshot=c->snapshot; c->pending.kind=PollControllerWorkKind::voters;
    c->pending.answer_id=answer; c->pending.after_user_id=after; c->pending.limit=limit;
    c->pending.epoch=c->epoch; c->pending.due_at_ms=now; c->pending_ready=true;
    c->voter_answer_id=answer; c->after_user_id=after; c->voters_more=false; c->voters_failed=false;
    c->last_error=PollControllerError::none;
    return result(*c,PollControllerError::none,true);
}
PollControllerResult poll_controller_queue_refresh(PollController* c,u64 now) noexcept {
    if (!c || !c->initialized || !c->source_active || !now) return {PollControllerError::invalid_input};
    if (!c->snapshot.can_inspect) return result(*c,PollControllerError::forbidden);
    if (c->pending_ready) return result(*c,PollControllerError::busy);
    if (!advance(*c)) return result(*c,PollControllerError::exhausted);
    reset_value(&c->pending); c->pending.snapshot=c->snapshot; c->pending.kind=PollControllerWorkKind::refresh;
    c->pending.epoch=c->epoch; c->pending.due_at_ms=now; c->pending_ready=true;
    return result(*c,PollControllerError::none,true);
}
bool poll_controller_prepare(const PollController& c,u64 now,u64 unix_now,
                              PollControllerCommand* command) noexcept {
    if (!command || !c.initialized || !c.pending_ready || c.flight.active || !now ||
        now<c.pending.due_at_ms || !flight_current(c,c.pending,unix_now)) return false;
    *command=c.pending; return true;
}
bool poll_controller_admit(PollController* c,const PollControllerCommand& command,u64 request_id,
                           u64 now,u64 unix_now,PollControllerTicket* ticket) noexcept {
    PollControllerCommand current{};
    if (!c || !ticket || !request_id || !poll_controller_prepare(*c,now,unix_now,&current) ||
        !command_equal(command,current)) return false;
    reset_value(&c->flight); c->flight.command=current; c->flight.ticket={request_id,current.epoch};
    c->flight.active=true; drop_pending(*c);
    if (command.kind==PollControllerWorkKind::vote) c->vote_phase=PollControllerVotePhase::sending;
    *ticket=c->flight.ticket; return true;
}
PollControllerResult poll_controller_submit_failed(PollController* c,u64 now) noexcept {
    if (!c || !c->initialized || !c->pending_ready || c->flight.active || !now)
        return {PollControllerError::invalid_input};
    if (c->pending.busy_retries<kPollControllerMaximumBusyRetries) {
        c->pending.due_at_ms=deadline_after(now,50ull<<c->pending.busy_retries);
        ++c->pending.busy_retries;
        return result(*c,PollControllerError::busy);
    }
    const auto kind=c->pending.kind;
    drop_pending(*c);
    if (kind==PollControllerWorkKind::vote) {
        c->desired_active=false; c->vote_phase=PollControllerVotePhase::failed;
    } else if (kind==PollControllerWorkKind::voters) c->voters_failed=true;
    else c->refresh_required=false;
    return result(*c,PollControllerError::transport,true);
}
namespace {
PollControllerResult complete_flight(PollController* c,const PollControllerCommand& command,
    const RestClientResponse& response,u64 now,bool body_valid,ConstSlice<ReactionUser> voters) noexcept {
    const bool vote=command.kind==PollControllerWorkKind::vote;
    if (response.transport_ok() && response.status_code==429 && response.retry_at_ms &&
        command.retries<kPollControllerMaximumRetries && !c->pending_ready &&
        (!vote || c->desired_active)) {
        c->pending=command; ++c->pending.retries; c->pending.busy_retries=0;
        c->pending.due_at_ms=response.retry_at_ms>now?response.retry_at_ms:deadline_after(now,250);
        c->pending_ready=true;
        if (vote) c->vote_phase=PollControllerVotePhase::queued;
        return result(*c,PollControllerError::none,true);
    }
    const bool ambiguous=!response.transport_ok() || response.status_code==408 || response.status_code>=500;
    const bool success=response.transport_ok() && response.status_code>=200 && response.status_code<300;
    if (vote) {
        if (!c->desired_active) return result(*c,PollControllerError::none,true);
        if (ambiguous || (success && !body_valid)) {
            c->vote_phase=PollControllerVotePhase::uncertain; c->refresh_required=true;
            return result(*c,PollControllerError::uncertain,true);
        }
        if (success) {
            c->vote_phase=PollControllerVotePhase::awaiting_authority; c->refresh_required=true;
            c->last_error=PollControllerError::none;
            return result(*c,PollControllerError::none,true);
        }
        c->vote_phase=PollControllerVotePhase::failed; c->desired_active=false;
        c->refresh_required=response.status_code==404 || response.status_code==400 || response.status_code==403;
        if (response.rejection.has_code && response.rejection.code==520001) {
            c->snapshot.can_vote=false; c->vote_barrier=PollControllerError::expired; c->refresh_required=true;
            return result(*c,PollControllerError::expired,true);
        }
        if (response.rejection.has_code && response.rejection.code==520000) {
            c->snapshot.can_vote=false; c->vote_barrier=PollControllerError::forbidden; c->refresh_required=true;
            return result(*c,PollControllerError::forbidden,true);
        }
        return result(*c,PollControllerError::http,true);
    }
    const bool transient=(!response.transport_ok() && response.error!=RestClientError::cancelled) ||
                         response.status_code==408 || response.status_code>=500;
    if (transient && command.retries<kPollControllerMaximumRetries && !c->pending_ready) {
        c->pending=command; ++c->pending.retries; c->pending.busy_retries=0;
        c->pending.due_at_ms=deadline_after(now,u64{kPollControllerReadRetryMs}<<command.retries);
        c->pending_ready=true;
        return result(*c,PollControllerError::none,true);
    }
    if (!success || response.status_code!=200 || !body_valid) {
        if (command.kind==PollControllerWorkKind::voters) c->voters_failed=true;
        // Reads may be retried by an explicit user action. An uncertain write
        // stays fenced even if its recovery read fails.
        if (command.kind==PollControllerWorkKind::refresh) c->refresh_required=false;
        return result(*c,!body_valid?PollControllerError::decode:
            !response.transport_ok()?PollControllerError::transport:PollControllerError::http,true);
    }
    if (command.kind==PollControllerWorkKind::refresh) {
        c->refresh_required=false;
        return result(*c,PollControllerError::none,true);
    }
    if ((!voters.data && voters.count) || voters.count>command.limit) {
        c->voters_failed=true; return result(*c,PollControllerError::decode,true);
    }
    for (u32 i=0;i<voters.count;++i) {
        if (!voters[i].id || voters[i].id<=command.after_user_id) {
            c->voters_failed=true; return result(*c,PollControllerError::decode,true);
        }
        for (u32 j=0;j<i;++j) if (voters[j].id==voters[i].id) {
            c->voters_failed=true; return result(*c,PollControllerError::decode,true);
        }
    }
    const u64 cursor=voters.count?voters[voters.count-1].id:command.after_user_id;
    c->after_user_id=cursor; c->voter_answer_id=command.answer_id;
    c->voters_more=voters.count==command.limit; c->voters_failed=false;
    return result(*c,PollControllerError::none,true,false,true);
}
}
PollControllerResult poll_controller_complete(PollController* c,PollControllerTicket ticket,
    const RestClientResponse& response,u64 now,u64 unix_now,bool body_valid,
    ConstSlice<ReactionUser> voters) noexcept {
    if (!c || !c->initialized || !c->flight.active || !ticket.request_id ||
        response.request_id!=ticket.request_id || c->flight.ticket.request_id!=ticket.request_id ||
        c->flight.ticket.epoch!=ticket.epoch) return {PollControllerError::stale,false,true};
    const bool stale=c->flight.retired || !flight_current(*c,c->flight.command,unix_now);
    // The command is read from the flight, which nothing below consults.
    const PollControllerResult done = stale ? result(*c,PollControllerError::none,false,true)
                                            : complete_flight(c,c->flight.command,response,now,body_valid,voters);
    reset_value(&c->flight);
    return done;
}
u64 poll_controller_next_wake(const PollController& c) noexcept {
    return c.pending_ready && !c.flight.active ? c.pending.due_at_ms : 0;
}
} // namespace maicord
