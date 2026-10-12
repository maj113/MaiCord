#include "maicord/protocol/message_poll.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/rest_route.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {
bool request_valid(const PollRequest& r) noexcept {
    if (!r.channel_id || !r.message_id || r.answer_count>kGatewayMessagePollMaximumAnswers) return false;
    for (auto reserved:r.reserved) if (reserved) return false;
    for (u32 i=0;i<kGatewayMessagePollMaximumAnswers;++i) {
        if (i<r.answer_count) {
            if (!r.answer_ids[i]) return false;
            for (u32 j=0;j<i;++j) if (r.answer_ids[j]==r.answer_ids[i]) return false;
        } else if (r.answer_ids[i]) return false;
    }
    switch (r.operation) {
    case PollOperation::vote:
        return r.answer_count && !r.answer_id && !r.after_user_id && !r.limit;
    case PollOperation::remove_vote:
    case PollOperation::refresh:
        return !r.answer_count && !r.answer_id && !r.after_user_id && !r.limit;
    case PollOperation::voters:
        return !r.answer_count && r.answer_id && r.limit && r.limit<=kReactionUsersMaximumPage;
    }
    return false;
}
struct MessageMembers {
    JsonValue poll{};
    u64 channel{},message{};
    bool poll_seen{},channel_seen{},message_seen{};
    PollDecodeError error{};
};
bool message_member(void* opaque,JsonString name,JsonValue value) noexcept {
    auto& c=*static_cast<MessageMembers*>(opaque);
    if (json_string_equals_ascii(name,utf8_literal("id"))) {
        if (c.message_seen || !json_value_to_snowflake(value,&c.message)) {
            c.error=PollDecodeError::identity; return false;
        }
        c.message_seen=true;
    } else if (json_string_equals_ascii(name,utf8_literal("channel_id"))) {
        if (c.channel_seen || !json_value_to_snowflake(value,&c.channel)) {
            c.error=PollDecodeError::identity; return false;
        }
        c.channel_seen=true;
    } else if (json_string_equals_ascii(name,utf8_literal("poll"))) {
        if (c.poll_seen) { c.error=PollDecodeError::poll; return false; }
        c.poll_seen=true; c.poll=value;
    }
    return true;
}
// The around page holds the requested message when it still exists; a
// neighbour (the target was deleted) is an identity failure, never a poll.
struct MessagePage {
    const JsonIndexedDocument* document;
    u32 depth;
    u64 expected;
    MessageMembers members;
    bool found, failed;
};
bool page_message(void* opaque,u32,JsonValue value) noexcept {
    auto& page=*static_cast<MessagePage*>(opaque);
    if (page.found || value.kind!=JsonValueKind::object) return true;
    MessageMembers candidate{};
    if (!json_visit_indexed_object(*page.document,value,page.depth,message_member,&candidate).ok()) {
        page.failed=true; page.members=candidate; return false;
    }
    if (candidate.message_seen && candidate.message==page.expected) {
        page.members=candidate; page.found=true;
    }
    return true;
}
struct VoterMembers { JsonValue users{}; bool seen{}; };
bool voters_member(void* opaque,JsonString name,JsonValue value) noexcept {
    auto& c=*static_cast<VoterMembers*>(opaque);
    if (json_string_equals_ascii(name,utf8_literal("users"))) {
        if (c.seen || value.kind!=JsonValueKind::array) return false;
        c.seen=true; c.users=value;
    }
    return true;
}
}
PollWriteResult write_poll_request(const PollRequest& request,
    Slice<u8> target_output,Slice<u8> body_output) noexcept {
    PollWriteResult result{};
    if (!request_valid(request) || (!target_output.data && target_output.count) ||
        (!body_output.data && body_output.count)) {
        result.error=PollWriteError::invalid_input; return result;
    }
    u8 target[kPollMaximumTargetBytes]{},body[kPollMaximumRequestBodyBytes]{};
    RouteWriter route{};
    ProtocolWriter payload{{body,sizeof(body)},0};
    const bool refresh=request.operation==PollOperation::refresh;
    const bool voters=request.operation==PollOperation::voters;
    // GET /channels/{c}/messages/{m} is bot-only (403, code 20002). A user
    // client reads one message as the official client's single-message fetch does:
    // /messages?limit=1&around={m}, an array whose element must match {m}.
    bool okay=route_writer_initialize(&route,{target,sizeof(target)}) &&
        route_append(&route,utf8_literal("/api/v10/channels/")) &&
        route_append_u64(&route,request.channel_id) &&
        (refresh ? route_append(&route,utf8_literal("/messages?limit=1&around="))
                 : route_append(&route,utf8_literal("/polls/"))) &&
        route_append_u64(&route,request.message_id);
    if (voters) {
        okay=okay && route_append(&route,utf8_literal("/answers/")) &&
            route_append_u64(&route,request.answer_id) &&
            route_append(&route,utf8_literal("?limit=")) &&
            route_append_u64(&route,request.limit) &&
            (!request.after_user_id || (route_append(&route,utf8_literal("&after=")) &&
                                       route_append_u64(&route,request.after_user_id))) &&
            route_append(&route,utf8_literal("&type=2"));
        result.method=RestMethod::get; result.route_id=kPollVotersRouteId;
    } else if (refresh) {
        result.method=RestMethod::get; result.route_id=kPollMessageRouteId;
    } else {
        okay=okay && route_append(&route,utf8_literal("/answers/@me")) &&
            protocol_writer_append_literal(&payload,"{\"answer_ids\":[");
        for (u32 i=0;okay && i<request.answer_count;++i)
            okay=(!i || protocol_writer_append_byte(&payload,',')) &&
                protocol_writer_append_byte(&payload,'"') &&
                protocol_writer_append_u64(&payload,request.answer_ids[i]) &&
                protocol_writer_append_byte(&payload,'"');
        okay=okay && protocol_writer_append_literal(&payload,"]}");
        result.method=RestMethod::put; result.route_id=kPollVoteRouteId;
    }
    if (!okay || route.bytes>target_output.count || payload.used>body_output.count ||
        !target_output.data || (payload.used && !body_output.data)) {
        result.error=PollWriteError::output_capacity; return result;
    }
    copy_bytes(target_output.data,target,route.bytes);
    copy_bytes(body_output.data,body,payload.used);
    result.target_bytes=route.bytes; result.body_bytes=payload.used;
    return result;
}
PollMessageDecodeResult decode_poll_message(Utf8View input,
    u64 expected_channel,u64 expected_message,Slice<u32> scratch,u32 depth,
    Slice<char> text,const GatewayMessagePollScratch& poll_scratch,GatewayMessagePoll* output) noexcept {
    PollMessageDecodeResult result{};
    if (!output || !expected_channel || !expected_message || !input.data || !input.bytes ||
        input.bytes>kPollMaximumResponseBytes || !scratch.data || !scratch.count || !depth) {
        result.error=PollDecodeError::invalid_input; return result;
    }
    const auto scan=json_scan_structurals_avx1(input,scratch);
    if (!scan.ok()) {
        result.error=PollDecodeError::scan; result.scan_error=scan.error;
        result.error_offset=scan.error_offset; return result;
    }
    const JsonIndexedDocument document{input,{scratch.data,scan.count}};
    MessagePage page{&document,depth,expected_message,{},false,false};
    const auto read=json_visit_root_array(input,document.structurals,depth,page_message,&page);
    MessageMembers& members=page.members;
    if (!read.ok() || page.failed) {
        result.error=members.error==PollDecodeError::none?PollDecodeError::json:members.error;
        result.read_error=read.error; result.error_offset=read.error_offset; return result;
    }
    if (!members.channel_seen || !members.message_seen || members.channel!=expected_channel ||
        members.message!=expected_message) { result.error=PollDecodeError::identity; return result; }
    if (!members.poll_seen) { result.error=PollDecodeError::poll; return result; }
    GatewayMessagePoll decoded{};
    result.poll=gateway_decode_message_poll(members.poll,document,depth,text,&result.text_bytes,
                                           poll_scratch,&decoded,true);
    if (!result.poll.ok()) {
        result.error=PollDecodeError::poll; result.error_offset=result.poll.error_offset; return result;
    }
    *output=decoded;
    return result;
}
PollVotersDecodeResult decode_poll_voters(Utf8View input,Slice<u32> page_scratch,
    Slice<u32> object_scratch,u32 depth,Slice<ReactionUser> users,Slice<char> text) noexcept {
    PollVotersDecodeResult result{};
    if (!input.data || !input.bytes || input.bytes>kPollMaximumResponseBytes || !depth ||
        !page_scratch.data || !page_scratch.count || !users.data || !users.count) {
        result.error=PollDecodeError::invalid_input; return result;
    }
    const auto scan=json_scan_structurals_avx1(input,page_scratch);
    if (!scan.ok()) {
        result.error=PollDecodeError::scan; result.scan_error=scan.error;
        result.error_offset=scan.error_offset; return result;
    }
    VoterMembers members{};
    const auto read=json_visit_root_object(input,{page_scratch.data,scan.count},depth,voters_member,&members);
    if (!read.ok() || !members.seen) {
        result.error=PollDecodeError::json; result.read_error=read.error;
        result.error_offset=read.error_offset; return result;
    }
    result.users=decode_reaction_users(members.users.source,page_scratch,object_scratch,depth,
        {users.data,minimum(users.count,kReactionUsersMaximumPage)},text);
    if (!result.users.ok()) {
        result.error=PollDecodeError::users;
        result.error_offset=members.users.source_offset+result.users.error_offset;
    }
    return result;
}
} // namespace maicord
