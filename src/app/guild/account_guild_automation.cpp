#include "maicord/app/guild/account_guild_automation.hpp"

#include "maicord/app/net/request_body_bank.hpp"

namespace maicord {
static_assert(kGuildAutomationMaximumTargetBytes <= kRestClientMaximumTargetBytes);

RestClientSubmitResult account_guild_automation_submit(RestClient* rest, const GuildAutomationRequest* request,
                                                       u64 request_id) noexcept {
    if (!rest || !request || !request_id || !guild_automation_request_valid(*request))
        return {RestClientSubmitError::invalid_input};
    u8 target[kGuildAutomationMaximumTargetBytes];
    const auto path = guild_automation_write_target(*request, {target, sizeof(target)});
    if (!path.ok())
        return {RestClientSubmitError::target_capacity};
    const RestRouteKey route = guild_automation_route(*request);
    // Lists (a server's rules with their word lists, its apps and webhooks) outgrow the ordinary response.
    if (route.method == RestMethod::get)
        return rest_client_submit_large_response(
            rest, {{target, path.bytes}, {}, route, request_id, kRestClientDefaultTimeoutMs, false, {}},
            kRestClientMaximumResponseBytes);
    // A webhook avatar's data URI rides on top of the ordinary bound.
    const u32 avatar = request->webhook ? request->webhook->avatar.bytes : 0;
    RequestBodyBank body{kGuildAutomationMaximumBodyBytes + avatar};
    if (!body.data)
        return {RestClientSubmitError::memory};
    const auto written = guild_automation_write_body(*request, body.all());
    if (!written.ok())
        return {RestClientSubmitError::body_capacity};
    body.used = written.bytes;
    if (written.bytes > kRestClientMaximumLargeRequestBodyBytes)
        return {RestClientSubmitError::body_capacity};
    const RestClientRequest submit{{target, path.bytes},
                                   {written.bytes ? body.data : nullptr, written.bytes},
                                   route,
                                   request_id,
                                   kRestClientDefaultTimeoutMs,
                                   written.bytes != 0,
                                   {}};
    return written.bytes > kRestClientMaximumRequestBodyBytes ? rest_client_submit_large(rest, submit)
                                                              : rest_client_submit(rest, submit);
}
} // namespace maicord
