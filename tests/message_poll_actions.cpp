#include "maicord/protocol/message_poll.hpp"
#include "maicord/app/account_message_poll.hpp"

namespace maicord {
void test_message_poll_actions(void (*check)(bool,const char*) noexcept) noexcept {
    u8 target[kPollMaximumTargetBytes]{},body[kPollMaximumRequestBodyBytes]{};
    PollRequest request{}; request.channel_id=5; request.message_id=6;
    request.operation=PollOperation::vote; request.answer_count=2;
    request.answer_ids[0]=7; request.answer_ids[1]=42;
    auto written=write_poll_request(request,{target,sizeof(target)},{body,sizeof(body)});
    constexpr char vote_target[]="/api/v10/channels/5/polls/6/answers/@me";
    constexpr char vote_body[]="{\"answer_ids\":[\"7\",\"42\"]}";
    check(written.ok() && written.method==RestMethod::put && written.route_id==kPollVoteRouteId &&
              written.target_bytes==sizeof(vote_target)-1 && written.body_bytes==sizeof(vote_body)-1 &&
              bytes_equal(target,vote_target,written.target_bytes) && bytes_equal(body,vote_body,written.body_bytes),
          "poll verified user vote route sends quoted validated answer ID strings with PUT");
    request.operation=PollOperation::remove_vote; request.answer_count=0;
    request.answer_ids[0]=request.answer_ids[1]=0;
    written=write_poll_request(request,{target,sizeof(target)},{body,sizeof(body)});
    constexpr char clear_body[]="{\"answer_ids\":[]}";
    check(written.ok() && written.method==RestMethod::put &&
              bytes_equal(target,vote_target,written.target_bytes) && written.body_bytes==sizeof(clear_body)-1 &&
              bytes_equal(body,clear_body,written.body_bytes),
          "poll remove own vote uses same verified PUT endpoint with empty answer array");
    request.operation=PollOperation::voters; request.answer_id=42; request.limit=25;
    request.after_user_id=123;
    written=write_poll_request(request,{target,sizeof(target)},{});
    constexpr char users_target[]="/api/v10/channels/5/polls/6/answers/42?limit=25&after=123&type=2";
    check(written.ok() && written.method==RestMethod::get && !written.body_bytes &&
              written.route_id==kPollVotersRouteId && written.target_bytes==sizeof(users_target)-1 &&
              bytes_equal(target,users_target,written.target_bytes),
          "poll verified voter page route includes actual cursor bounded limit and vote reaction type");
    request={}; request.channel_id=5; request.message_id=6; request.operation=PollOperation::refresh;
    written=write_poll_request(request,{target,sizeof(target)},{body,sizeof(body)});
    constexpr char refresh_target[]="/api/v10/channels/5/messages?limit=1&around=6";
    check(written.ok() && written.method==RestMethod::get && !written.body_bytes &&
              written.route_id==kPollMessageRouteId && written.target_bytes==sizeof(refresh_target)-1 &&
              bytes_equal(target,refresh_target,written.target_bytes),
          "poll refresh reads one message like the official single-message fetch (limit=1&around), "
          "never the bot-only /messages/{id} route that answers 403 code 20002");
    request.operation=PollOperation::voters; request.answer_id=42; request.limit=25;
    request.after_user_id=123;
    target[0]=123; body[0]=124;
    request.limit=101;
    check(!write_poll_request(request,{target,sizeof(target)},{body,sizeof(body)}).ok() &&
              target[0]==123 && body[0]==124,
          "poll malformed request preserves caller buffers before transport admission");
    request={}; request.channel_id=5; request.message_id=6; request.operation=PollOperation::vote;
    request.answer_count=2; request.answer_ids[0]=request.answer_ids[1]=7;
    check(!write_poll_request(request,{target,sizeof(target)},{body,sizeof(body)}).ok(),
          "poll duplicate selected answer IDs cannot enter the copied mutation payload");
    request.answer_ids[1]=42;
    check(write_poll_request(request,{target,1},{body,sizeof(body)}).error==PollWriteError::output_capacity &&
              target[0]==123 && body[0]==124,
          "poll transactional staging preserves output on target capacity failure");
    check(account_message_poll_submit(nullptr,&request,1).error==RestClientSubmitError::invalid_input,
          "poll account transport helper cannot create a worker or submit without existing authenticated owner");
    u32 page_scratch[512]{},object_scratch[128]{};
    char text[4096]{};
    ReactionUser users[3]{};
    auto voters=decode_poll_voters(utf8_literal("{\"users\":[{\"id\":\"101\",\"username\":\"a\",\"global_name\":\"A\",\"avatar\":null}]}"),
        {page_scratch,512},{object_scratch,128},16,{users,3},{text,sizeof(text)});
    check(voters.ok() && voters.users.user_count==1 && users[0].id==101 && users[0].display_name.bytes==1 &&
              users[0].display_name.data>=text && users[0].display_name.data<text+sizeof(text),
          "poll voter wrapper decodes bounded display fields into caller-owned text");
    voters=decode_poll_voters(utf8_literal("{\"users\":[{\"id\":\"102\",\"username\":\"b\","
        "\"avatar\":\"0123456789abcdef0123456789abcdef\"},{\"id\":\"103\",\"username\":\"c\",\"avatar\":null}]}"),
        {page_scratch,512},{object_scratch,128},16,{users,3},{text,sizeof(text)});
    check(voters.ok() && voters.users.user_count==2 && users[0].id==102 &&
              utf8_equal(users[0].avatar_hash,utf8_literal("0123456789abcdef0123456789abcdef")) &&
              users[0].avatar_hash.data>=text && users[0].avatar_hash.data<text+sizeof(text) &&
              users[1].id==103 && !users[1].avatar_hash.bytes,
          "poll voter avatar hashes decode into caller-owned text for voter row avatars");
    check(!decode_poll_voters(utf8_literal("[]"),{page_scratch,512},{object_scratch,128},16,
              {users,3},{text,sizeof(text)}).ok() &&
              !decode_poll_voters(utf8_literal("{\"users\":[],\"users\":[]}"),{page_scratch,512},
                  {object_scratch,128},16,{users,3},{text,sizeof(text)}).ok() &&
              decode_poll_voters(utf8_literal("{\"users\":[]}"),{page_scratch,512},
                  {object_scratch,128},16,{users,3},{text,sizeof(text)}).ok(),
          "poll voter response requires one typed users array and accepts an empty terminal page");
    GatewayMessagePollAnswer answers[10]{}; GatewayMessagePollAnswerCount counts[10]{};
    GatewayMessagePoll poll{};
    const auto json=utf8_literal("[{\"id\":\"6\",\"channel_id\":\"5\",\"content\":\"untouched\",\"poll\":{"
        "\"question\":{\"text\":\"Choose\"},\"answers\":[{\"answer_id\":7,\"poll_media\":{\"text\":\"A\"}},"
        "{\"answer_id\":42,\"poll_media\":{\"text\":\"B\"}}],\"expiry\":\"2026-10-06T00:00:00Z\","
        "\"allow_multiselect\":true,\"layout_type\":1,\"results\":{\"is_finalized\":false,"
        "\"answer_counts\":[{\"id\":7,\"count\":1,\"me_voted\":true}]}}}]");
    const auto decoded=decode_poll_message(json,5,6,{page_scratch,512},16,{text,sizeof(text)},
                                           {{answers,10},{counts,10}},&poll);
    check(decoded.ok() && poll.present && poll.answers.count==2 && poll.answers[1].answer_id==42 &&
              poll.results_known && poll.answer_counts.count==1 && poll.answer_counts[0].me_voted &&
              poll.question.data>=text && poll.question.data<text+sizeof(text),
          "poll refresh scopes exact message identity and decodes only complete poll into owned scratch");
    const auto before=poll;
    check(decode_poll_message(json,5,99,{page_scratch,512},16,{text,sizeof(text)},
              {{answers,10},{counts,10}},&poll).error==PollDecodeError::identity &&
              bytes_equal(&before,&poll,sizeof(poll)) &&
              !decode_poll_message(utf8_literal("[{\"id\":\"6\",\"channel_id\":\"5\",\"poll\":{\"question\":{\"text\":\"partial\"}}}]"),
                  5,6,{page_scratch,512},16,{text,sizeof(text)},{{answers,10},{counts,10}},&poll).ok() &&
              decode_poll_message(utf8_literal("[{\"id\":\"7\",\"channel_id\":\"5\"}]"),
                  5,6,{page_scratch,512},16,{text,sizeof(text)},{{answers,10},{counts,10}},&poll).error==
                  PollDecodeError::identity &&
              decode_poll_message(utf8_literal("[]"),5,6,{page_scratch,512},16,{text,sizeof(text)},
                  {{answers,10},{counts,10}},&poll).error==PollDecodeError::identity &&
              bytes_equal(&before,&poll,sizeof(poll)),
          "poll wrong-source, deleted-target neighbour, empty page or partial recovery body never replaces "
          "the current authoritative DTO");
}
} // namespace maicord
