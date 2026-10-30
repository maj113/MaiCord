#include "maicord/app/account_guild_community.hpp"

namespace maicord {
static_assert(kGuildCommunityMaximumTargetBytes <= kRestClientMaximumTargetBytes);
static_assert(kGuildCommunityMaximumBodyBytes <= kRestClientMaximumRequestBodyBytes);

RestClientSubmitResult account_guild_community_submit(RestClient* rest,
                                                      const GuildCommunityRequest* request,
                                                      u64 request_id, Utf8View session_id) noexcept {
    if (!rest || !request || !request_id || !guild_community_request_valid(*request))
        return {RestClientSubmitError::invalid_input};
    const bool join = request->operation == GuildCommunityOperation::guild_join;
    u8 target[kGuildCommunityMaximumTargetBytes];
    const auto path = guild_community_write_target(*request, {target, sizeof(target)}, join ? session_id : Utf8View{});
    if (!path.ok())
        return {path.error == GuildCommunityWriteError::output_capacity ? RestClientSubmitError::target_capacity
                                                                       : RestClientSubmitError::invalid_input};
    u8 interest[32];
    ConstSlice<u8> body{request->body, request->body_bytes};
    if (request->operation == GuildCommunityOperation::event_interested)
        body = {interest, guild_community_write_interest_body({interest, sizeof(interest)})};
    else if (request->operation == GuildCommunityOperation::guild_join)
        body = {interest, guild_community_write_join_body({interest, sizeof(interest)})};
    // The RSVP list is a user route; everything else buckets per guild.
    const bool user_route = request->operation == GuildCommunityOperation::event_rsvps;
    // The tag popout's join carries the X-Context-Properties header.
    u8 context[64];
    const u32 context_bytes = join ? guild_community_write_join_context({context, sizeof(context)}) : 0;
    if (join && !context_bytes)
        return {RestClientSubmitError::invalid_input};
    return rest_client_submit(rest, {{target, path.target_bytes},
                                     body.count ? body : ConstSlice<u8>{},
                                     {path.route_id, user_route ? 0 : request->guild_id, path.method,
                                      user_route ? RestMajorParameter::none
                                                         : RestMajorParameter::guild},
                                     request_id,
                                     kRestClientDefaultTimeoutMs,
                                     body.count != 0,
                                     {},
                                     {context, context_bytes}});
}
} // namespace maicord
