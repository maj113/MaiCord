#include "maicord/app/account_guild_invite.hpp"

namespace maicord {
RestClientSubmitResult account_guild_invite_submit(RestClient* rest, const GuildInviteRequest* request,
                                                   u64 request_id, Utf8View session_id) noexcept {
    if (!rest || !request || !request_id)
        return {RestClientSubmitError::invalid_input};
    u8 target[kGuildInviteMaximumTargetBytes]{}, body[kGuildInviteMaximumBodyBytes]{}, context[512]{};
    const auto written =
        guild_invite_write_request(*request, {target, sizeof(target)}, {body, sizeof(body)}, session_id);
    const u32 context_bytes = request->operation == GuildInviteOperation::accept
                                  ? guild_invite_write_context_header(*request, {context, sizeof(context)})
                                  : 0;
    if (!written.ok())
        return {written.error == GuildInviteWriteError::output_capacity ? RestClientSubmitError::target_capacity
                                                                        : RestClientSubmitError::invalid_input};
    return rest_client_submit(rest, {{target, written.target_bytes},
                                     {body, written.body_bytes},
                                     {written.route_id, 0, written.method, RestMajorParameter::none},
                                     request_id,
                                     kRestClientDefaultTimeoutMs,
                                     written.body_bytes != 0,
                                     false,
                                     {},
                                     {context, context_bytes}});
}
RestClientSubmitResult account_guild_invite_cancel(RestClient* rest, u64 request_id) noexcept {
    return rest_client_cancel_result(rest, request_id);
}
} // namespace maicord
