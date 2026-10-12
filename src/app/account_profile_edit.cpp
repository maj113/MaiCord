#include "maicord/app/account_profile_edit.hpp"

#include <Windows.h>

namespace maicord {
static_assert(kProfileEditMaximumTargetBytes <= kRestClientMaximumTargetBytes);
namespace {
// Avatar and banner uploads may need up to two minutes.
constexpr u32 kLargeTimeoutMs = 120 * 1000;
} // namespace

RestClientSubmitResult account_profile_edit_submit(RestClient* rest, const ProfileEditRequest* request,
                                                   u64 request_id) noexcept {
    if (!rest || !request || !request_id || !profile_edit_request_valid(*request))
        return {RestClientSubmitError::invalid_input};
    u8 target[kProfileEditMaximumTargetBytes];
    const auto path = profile_edit_write_target(*request, {target, sizeof(target)});
    if (!path.ok)
        return {RestClientSubmitError::target_capacity};
    const u32 capacity = profile_edit_body_capacity(*request);
    if (!capacity)
        return rest_client_submit(rest, {{target, path.bytes},
                                         {},
                                         profile_edit_route(*request),
                                         request_id,
                                         kRestClientDefaultTimeoutMs,
                                         false});
    if (capacity > kRestClientMaximumLargeRequestBodyBytes + 6 * 1024)
        return {RestClientSubmitError::body_capacity};
    // Demand-backed body bank; cleared before release (bodies hold the
    // account's text and images).
    auto* body = static_cast<u8*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!body)
        return {RestClientSubmitError::memory};
    const auto written = profile_edit_write_body(*request, {body, capacity});
    const bool large = written.ok && written.bytes > kRestClientMaximumRequestBodyBytes;
    const RestClientRequest submit{{target, path.bytes},
                                   {body, written.ok ? written.bytes : 0},
                                   profile_edit_route(*request),
                                   request_id,
                                   large ? kLargeTimeoutMs : kRestClientDefaultTimeoutMs,
                                   true};
    const auto result = !written.ok || written.bytes > kRestClientMaximumLargeRequestBodyBytes
                            ? RestClientSubmitResult{RestClientSubmitError::body_capacity}
                        : large ? rest_client_submit_large(rest, submit)
                                : rest_client_submit(rest, submit);
    SecureZeroMemory(body, written.ok ? written.bytes : 0);
    VirtualFree(body, 0, MEM_RELEASE);
    return result;
}

RestClientSubmitResult account_profile_edit_cancel(RestClient* rest, u64 request_id) noexcept {
    if (!rest || !request_id)
        return {RestClientSubmitError::invalid_input};
    return {rest_client_cancel(rest, request_id) ? RestClientSubmitError::none
                                                         : RestClientSubmitError::busy};
}

bool account_profile_image(ProfileImageRequest* request) noexcept {
    if (!request || !request->info || !request->source.data || !request->source.count)
        return false;
    request->data_uri_bytes = 0;
    if (!request->crop.width) {
        *request->info = profile_image_probe(request->source);
        return request->info->error == ProfileImageError::none;
    }
    GuildImageBuffer buffer{};
    *request->info = profile_image_crop(request->source, request->crop, &buffer);
    bool ok = request->info->error == ProfileImageError::none && buffer.data;
    if (ok) {
        const auto kind = request->info->output_kind;
        const u32 needed = guild_image_data_uri_bytes(kind, buffer.bytes);
        request->data_uri_bytes = needed;
        ok = needed && request->data_uri.data && needed <= request->data_uri.count &&
             guild_image_write_data_uri(kind, {buffer.data, buffer.bytes}, {request->data_uri.data, needed}) == needed;
    }
    guild_image_release(&buffer);
    return ok;
}

} // namespace maicord
