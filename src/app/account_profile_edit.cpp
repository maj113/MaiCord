#include "maicord/app/account_profile_edit.hpp"

#include "maicord/app/original_md5.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

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
    // X-Discord-Original-MD5 of a new avatar or banner, as the official client reports it.
    u8 md5_value[80];
    u8 headers[128];
    const u32 md5_bytes = profile_edit_write_md5(*request, {md5_value, sizeof(md5_value)});
    u32 header_bytes = 0;
    if (md5_bytes) {
        header_bytes = rest_append_extra_header({headers, sizeof(headers)}, 0, utf8_literal("X-Discord-Original-MD5"),
                                                {md5_value, md5_bytes});
        if (header_bytes == ~0u)
            return {RestClientSubmitError::invalid_input};
    }
    if (capacity > kRestClientMaximumLargeRequestBodyBytes + 6 * 1024)
        return {RestClientSubmitError::body_capacity};
    // Demand-backed body bank; cleared before release (bodies hold the
    // account's text and images).
    auto* body = static_cast<u8*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!body)
        return {RestClientSubmitError::memory};
    const auto written = profile_edit_write_body(*request, {body, capacity});
    const bool large = written.ok && written.bytes > kRestClientMaximumRequestBodyBytes;
    RestClientRequest submit{{target, path.bytes},
                                   {body, written.ok ? written.bytes : 0},
                                   profile_edit_route(*request),
                                   request_id,
                                   large ? kLargeTimeoutMs : kRestClientDefaultTimeoutMs,
                                   true};
    submit.extra_headers = {headers, header_bytes};
    const auto result = !written.ok || written.bytes > kRestClientMaximumLargeRequestBodyBytes
                            ? RestClientSubmitResult{RestClientSubmitError::body_capacity}
                        : large ? rest_client_submit_large(rest, submit)
                                : rest_client_submit(rest, submit);
    secure_clear_bytes(body, written.ok ? written.bytes : 0);
    VirtualFree(body, 0, MEM_RELEASE);
    return result;
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
    request->has_original_md5 = ok && original_md5_hex(request->source, request->original_md5);
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
