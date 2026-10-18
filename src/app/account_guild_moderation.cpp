#include "maicord/app/account_guild_moderation.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
static_assert(kGuildModerationMaximumTargetBytes <= kRestClientMaximumTargetBytes);
static_assert(kGuildModerationMaximumBodyBytes <= kRestClientMaximumRequestBodyBytes);
namespace {
// Up to 500 members with their user objects outgrow the ordinary response.
constexpr u32 kSearchResponseBytes = kRestClientMaximumResponseBytes;
} // namespace

RestClientSubmitResult account_guild_moderation_submit(RestClient* rest, const GuildModerationRequest* request,
                                                       u64 request_id) noexcept {
    if (!rest || !request || !request_id || !guild_moderation_request_valid(*request))
        return {RestClientSubmitError::invalid_input};
    u8 target[kGuildModerationMaximumTargetBytes];
    const auto path = guild_moderation_write_target(*request, {target, sizeof(target)});
    if (!path.ok())
        return {RestClientSubmitError::target_capacity};
    u8 reason[kRestAuditReasonMaximumEncodedBytes];
    const auto encoded = guild_moderation_write_reason(*request, {reason, sizeof(reason)});
    if (!encoded.ok())
        return {RestClientSubmitError::invalid_input};
    auto* body = static_cast<u8*>(
        VirtualAlloc(nullptr, kGuildModerationMaximumBodyBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!body)
        return {RestClientSubmitError::memory};
    const auto written = guild_moderation_write_body(*request, {body, kGuildModerationMaximumBodyBytes});
    RestClientSubmitResult result{RestClientSubmitError::body_capacity};
    if (written.ok()) {
        RestClientRequest submit{{target, path.bytes},
                                 {written.bytes ? body : nullptr, written.bytes},
                                 guild_moderation_route(*request),
                                 request_id,
                                 kRestClientDefaultTimeoutMs,
                                 written.bytes != 0,
                                 false,
                                 {reason, encoded.bytes}};
        // "" sends an empty X-Audit-Log-Reason; null sends none.
        submit.send_empty_audit_reason = !encoded.bytes && guild_moderation_reason_header(*request);
        result = request->operation == GuildModerationOperation::members_search
                     ? rest_client_submit_large_response(rest, submit, kSearchResponseBytes)
                     : rest_client_submit(rest, submit);
    }
    secure_clear_bytes(body, written.ok() ? written.bytes : 0);
    VirtualFree(body, 0, MEM_RELEASE);
    secure_clear_bytes(reason, encoded.bytes);
    return result;
}

RestClientSubmitResult account_guild_moderation_cancel(RestClient* rest, u64 request_id) noexcept {
    return rest_client_cancel_result(rest, request_id);
}
} // namespace maicord
