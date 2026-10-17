#include "maicord/app/poll_controller.hpp"
#include "maicord/app/rest_client.hpp"

namespace maicord {
namespace {
PollControllerSnapshot source() noexcept {
    PollControllerSnapshot p{};
    p.source.actor={1,2,3,4}; p.source.channel_id=5; p.source.message_id=6;
    p.source.channel_epoch=7; p.source.handle={0,1}; p.source.poll_content_revision=1;
    p.source.channel_generation=2; p.expiry_ms=100'000;
    p.answer_ids[0]=7; p.answer_ids[1]=42; p.answer_count=2;
    p.mine_known=true; p.multiple=true; p.can_vote=p.can_inspect=true;
    return p;
}
RestClientResponse receipt(u64 id,u16 status=200) noexcept {
    RestClientResponse r{}; r.request_id=id; r.status_code=status; return r;
}
}
void test_poll_controller(void (*check)(bool,const char*) noexcept) noexcept {
    PollController c{}; poll_controller_initialize(&c);
    auto p=source();
    check(poll_controller_observe(&c,p).ok() && poll_controller_queue_vote(&c,3,1,1).ok(),
          "poll multiselect queues exact desired IDs without projecting aggregate counts");
    PollControllerCommand command{}; PollControllerTicket ticket{};
    check(poll_controller_prepare(c,1,1,&command) && command.desired_mask==3 &&
              poll_controller_admit(&c,command,77,1,1,&ticket) && c.flight.active &&
              !poll_controller_prepare(c,1,1,&command), "poll controller admits one copied exact flight");
    p.mine_mask=1; poll_controller_observe(&c,p);
    auto limited=receipt(77,429); limited.retry_at_ms=300;
    check(poll_controller_complete(&c,ticket,limited,2,2).ok() && c.pending_ready &&
              c.vote_phase==PollControllerVotePhase::queued && !poll_controller_prepare(c,299,2,&command) &&
              poll_controller_prepare(c,300,2,&command) && command.desired_mask==3 &&
              poll_controller_admit(&c,command,78,300,2,&ticket),
          "poll definitive rate rejection retries same copied choice after server delay");
    p.mine_mask=3; poll_controller_observe(&c,p);
    auto timeout=receipt(78); timeout.error=RestClientError::timeout;
    check(poll_controller_complete(&c,ticket,timeout,301,3).ok() && !c.desired_active &&
              c.vote_phase==PollControllerVotePhase::idle && !c.refresh_required && !c.pending_ready,
          "poll authoritative own vote wins a later ambiguous transport receipt without replay");
    p.mine_mask=0; poll_controller_observe(&c,p);
    poll_controller_queue_vote(&c,1,400,4); poll_controller_prepare(c,400,4,&command);
    poll_controller_admit(&c,command,79,400,4,&ticket);
    timeout=receipt(79); timeout.error=RestClientError::receive;
    check(poll_controller_complete(&c,ticket,timeout,401,4).error==PollControllerError::uncertain &&
              c.refresh_required && !c.pending_ready &&
              poll_controller_queue_vote(&c,1,402,4).error==PollControllerError::uncertain,
          "poll possibly committed mutation demands authoritative refresh and forbids automatic or repeated vote");
    check(poll_controller_queue_refresh(&c,403).ok() && poll_controller_prepare(c,403,4,&command) &&
              command.kind==PollControllerWorkKind::refresh &&
              poll_controller_admit(&c,command,80,403,4,&ticket), "poll recovery uses copied scoped read flight");
    poll_controller_observe(&c,p,true);
    check(poll_controller_complete(&c,ticket,receipt(80),404,4).ok() && !c.refresh_required &&
              !c.desired_active && poll_controller_queue_vote(&c,1,405,4).ok(),
          "poll authoritative refresh settles uncertainty before an explicit new vote");
    poll_controller_prepare(c,405,4,&command); poll_controller_admit(&c,command,81,405,4,&ticket);
    check(poll_controller_complete(&c,ticket,receipt(81,204),406,4).ok() &&
              c.vote_phase==PollControllerVotePhase::awaiting_authority && c.refresh_required,
          "poll mutation success does not fabricate own membership and requests convergence");
    p.mine_mask=1; poll_controller_observe(&c,p);
    check(!c.refresh_required && !c.desired_active &&
              poll_controller_queue_vote(&c,2,407,4).error==PollControllerError::selection &&
              poll_controller_queue_vote(&c,0,408,4).ok(),
          "poll existing own vote requires explicit removal before another selection");
    poll_controller_prepare(c,408,4,&command); poll_controller_admit(&c,command,82,408,4,&ticket);
    p.mine_mask=0; poll_controller_observe(&c,p);
    poll_controller_complete(&c,ticket,receipt(82,204),409,4);
    check(!c.desired_active && c.vote_phase==PollControllerVotePhase::idle,
          "poll own remove echo settles empty exact selection");
    poll_controller_queue_voter_page(&c,7,0,2,500); poll_controller_prepare(c,500,5,&command);
    poll_controller_admit(&c,command,83,500,5,&ticket);
    check(poll_controller_queue_voter_page(&c,42,100,2,501).ok() && c.flight.active && c.flight.retired &&
              !poll_controller_prepare(c,501,5,&command) &&
              poll_controller_complete(&c,ticket,receipt(83),502,5).stale && c.pending_ready &&
              poll_controller_prepare(c,502,5,&command) && command.answer_id==42 && command.after_user_id==100,
          "poll latest selected voter answer waits for retired exact read receipt without reusing flight buffers");
    poll_controller_admit(&c,command,84,502,5,&ticket);
    ReactionUser users[2]{}; users[0].id=101; users[1].id=103;
    check(poll_controller_complete(&c,ticket,receipt(84),503,5,true,{users,2}).page_ready &&
              c.after_user_id==103 && c.voters_more && poll_controller_queue_voter_page(&c,42,c.after_user_id,2,504).ok(),
          "poll voter pagination retains real advancing last snowflake and full-page continuation");
    poll_controller_prepare(c,504,5,&command); poll_controller_admit(&c,command,85,504,5,&ticket);
    check(poll_controller_complete(&c,ticket,receipt(85),505,5,true,{}).page_ready &&
              !c.voters_more && c.after_user_id==103,
          "poll empty voter page is a safe terminal page without invented totals or cursor");
    poll_controller_queue_voter_page(&c,42,100,2,506); poll_controller_prepare(c,506,5,&command);
    poll_controller_admit(&c,command,86,506,5,&ticket); users[0].id=100;
    check(poll_controller_complete(&c,ticket,receipt(86),507,5,true,{users,2}).error==PollControllerError::decode &&
              c.voters_failed, "poll voter page cannot append repeated or non-advancing IDs");
    poll_controller_queue_voter_page(&c,42,100,2,507); poll_controller_prepare(c,507,5,&command);
    poll_controller_admit(&c,command,90,507,5,&ticket); users[0].id=103; users[1].id=101;
    check(poll_controller_complete(&c,ticket,receipt(90),508,5,true,{users,2}).page_ready &&
              c.after_user_id==101 && c.voters_more,
          "poll unsorted advancing voter page uses verified exact last returned ID without assuming max-tail ordering");
    poll_controller_queue_voter_page(&c,7,0,2,508); poll_controller_prepare(c,508,5,&command);
    poll_controller_admit(&c,command,87,508,5,&ticket);
    p.source.actor.account_scope=99; poll_controller_observe(&c,p);
    check(c.flight.active && c.flight.retired &&
              poll_controller_complete(&c,ticket,receipt(88),509,5).stale && c.flight.active &&
              poll_controller_complete(&c,ticket,receipt(87),510,5).stale && !c.flight.active &&
              c.snapshot.source.actor.account_scope==99,
          "poll actor reset holds old flight tombstone and ignores mismatched request receipt");
    p.can_vote=false; poll_controller_observe(&c,p);
    check(poll_controller_queue_vote(&c,1,600,6).error==PollControllerError::forbidden,
          "poll current permission is required before mutation admission");
    p.can_vote=true; p.finalized=true; poll_controller_observe(&c,p);
    check(poll_controller_queue_vote(&c,1,601,6).error==PollControllerError::finalized &&
              poll_controller_queue_voter_page(&c,7,0,2,601).ok(),
          "poll finalized source remains inspectable but cannot accept vote");
    poll_controller_close(&c); p.finalized=false; p.expiry_ms=6; poll_controller_observe(&c,p);
    check(poll_controller_queue_vote(&c,1,602,6).error==PollControllerError::expired,
          "poll Unix expiry fences queued vote independently of monotonic scheduling");
    poll_controller_close(&c); p=source(); p.expiry_ms=8; poll_controller_observe(&c,p);
    poll_controller_queue_vote(&c,1,603,7);
    check(poll_controller_expire(&c,8).error==PollControllerError::expired && !c.pending_ready &&
              c.refresh_required && !poll_controller_next_wake(c),
          "poll queued expiry retires pending mutation without a past-due scheduler loop");
    poll_controller_close(&c); p=source(); poll_controller_observe(&c,p);
    poll_controller_queue_vote(&c,1,604,8); poll_controller_prepare(c,604,8,&command);
    poll_controller_admit(&c,command,89,604,8,&ticket);
    auto expired=receipt(89,400); expired.rejection.has_code=true; expired.rejection.code=520001;
    check(poll_controller_complete(&c,ticket,expired,605,8).error==PollControllerError::expired &&
              c.refresh_required && !c.pending_ready, "poll server-expired rejection fences local clock disagreement");
    poll_controller_observe(&c,p);
    check(poll_controller_queue_vote(&c,1,606,8).error==PollControllerError::expired,
          "poll cached snapshot cannot clear server-expired mutation barrier before convergence");
    poll_controller_close(&c); p=source(); p.mine_known=false; poll_controller_observe(&c,p);
    check(poll_controller_queue_vote(&c,1,607,8).error==PollControllerError::uncertain &&
              c.refresh_required && !c.pending_ready && !c.desired_active,
          "poll absent own results require scoped refresh before a new mutation without invented zero vote");
    poll_controller_observe(&c,p,true);
    check(poll_controller_queue_vote(&c,1,608,8).error==PollControllerError::uncertain,
          "poll recovery that still omits own results cannot authorize a repeated vote");
    p.mine_known=true; poll_controller_observe(&c,p,true);
    check(poll_controller_queue_vote(&c,1,609,8).ok(),
          "poll known authoritative own result enables a later explicit user vote");
}
} // namespace maicord
