#include "maicord/app/account_guild_admin.hpp"

#include <Windows.h>

namespace maicord {
static_assert(kGuildAdminMaximumTargetBytes <= kRestClientMaximumTargetBytes);
namespace {
// Per-server avatar and banner uploads may need up to two minutes.
constexpr u32 kLargeTimeoutMs = 120 * 1000;
// Demand-backed body bank sized to the transport limit plus any image data
// URIs (per-server avatar or banner); cleared before release. Bodies over
// the ordinary ceiling take the large path.
__declspec(noinline) RestClientSubmitResult submit_with_body(RestClient* rest,
                                                             const GuildAdminRequest& request,
                                                             ConstSlice<u8> target,
                                                             ConstSlice<u8> reason,
                                                             u64 request_id) noexcept {
    const u32 images = guild_admin_image_bytes(request);
    if (images > kRestClientMaximumLargeRequestBodyBytes - kRestClientMaximumRequestBodyBytes)
        return {RestClientSubmitError::body_capacity};
    const u32 capacity = kRestClientMaximumRequestBodyBytes + images;
    auto* body = static_cast<u8*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!body)
        return {RestClientSubmitError::memory};
    const auto written = guild_admin_write_body(request, {body, capacity});
    const bool large = written.ok() && written.bytes > kRestClientMaximumRequestBodyBytes;
    const RestClientRequest submit{target,
                                   {body, written.ok() ? written.bytes : 0},
                                   guild_admin_route(request),
                                   request_id,
                                   large ? kLargeTimeoutMs : kRestClientDefaultTimeoutMs,
                                   true,
                                   false,
                                   reason};
    const auto result = !written.ok() ? RestClientSubmitResult{RestClientSubmitError::body_capacity}
                        : large       ? rest_client_submit_large(rest, submit)
                                      : rest_client_submit(rest, submit);
    SecureZeroMemory(body, written.ok() ? written.bytes : 0);
    VirtualFree(body, 0, MEM_RELEASE);
    return result;
}
} // namespace

RestClientSubmitResult account_guild_admin_submit(RestClient* rest, const GuildAdminRequest* request,
                                                  u64 request_id) noexcept {
    if (!rest || !request || !request_id || !guild_admin_request_valid(*request))
        return {RestClientSubmitError::invalid_input};
    u8 target[kGuildAdminMaximumTargetBytes];
    const auto path = guild_admin_write_target(*request, {target, sizeof(target)});
    if (!path.ok())
        return {RestClientSubmitError::target_capacity};
    u8 reason[kRestAuditReasonMaximumEncodedBytes];
    const auto encoded = guild_admin_write_reason(*request, {reason, sizeof(reason)});
    if (!encoded.ok())
        return {RestClientSubmitError::invalid_input};
    RestClientSubmitResult result{};
    if (guild_admin_has_body(request->operation))
        result = submit_with_body(rest, *request, {target, path.bytes}, {reason, encoded.bytes}, request_id);
    else
        result = rest_client_submit(rest, {{target, path.bytes},
                                           {},
                                           guild_admin_route(*request),
                                           request_id,
                                           kRestClientDefaultTimeoutMs,
                                           false,
                                           false,
                                           {reason, encoded.bytes}});
    SecureZeroMemory(reason, encoded.bytes);
    return result;
}

RestClientSubmitResult account_guild_admin_cancel(RestClient* rest, u64 request_id) noexcept {
    if (!rest || !request_id)
        return {RestClientSubmitError::invalid_input};
    return {rest_client_cancel(rest, request_id) ? RestClientSubmitError::none
                                                         : RestClientSubmitError::busy};
}
} // namespace maicord
