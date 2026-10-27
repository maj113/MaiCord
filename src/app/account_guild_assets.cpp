#include "maicord/app/account_guild_assets.hpp"
#include "maicord/app/guild_image.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
static_assert(kGuildAssetMaximumTargetBytes <= kRestClientMaximumTargetBytes);
namespace {

void report(const GuildAssetRequest& request, const GuildImageInfo& info) noexcept {
    if (request.info)
        *request.info = info;
}

// Prepared upload for an image operation; an empty source clears the image.
bool prepare(const GuildAssetRequest& request, GuildImageBuffer* buffer, GuildPreparedImage* image) noexcept {
    reset_value(buffer);
    reset_value(image);
    if (!guild_asset_has_image(request.operation) || !request.image.count)
        return true;
    const auto info = guild_image_prepare(guild_asset_profile(request), request.image, buffer, image);
    report(request, info);
    return info.error == GuildImageError::none;
}

__declspec(noinline) RestClientSubmitResult submit_with_body(RestClient* rest,
                                                             const GuildAssetRequest& request,
                                                             GuildPreparedImage image, ConstSlice<u8> target,
                                                             ConstSlice<u8> reason, u64 request_id) noexcept {
    u8 boundary[kGuildAssetBoundaryBytes];
    const bool multipart = request.operation == GuildAssetOperation::sticker_create;
    if (multipart && !guild_asset_choose_boundary(request, image, request_id ^ (request.guild_id << 1),
                                                  {boundary, sizeof(boundary)}))
        return {RestClientSubmitError::invalid_input};
    const ConstSlice<u8> chosen{boundary, multipart ? kGuildAssetBoundaryBytes : 0};
    const u32 bytes = guild_asset_body_bytes(request, image, chosen);
    if (!bytes)
        return {RestClientSubmitError::invalid_input};
    if (bytes > kRestClientMaximumLargeRequestBodyBytes)
        return {RestClientSubmitError::body_capacity};
    auto* body = static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!body)
        return {RestClientSubmitError::memory};
    const auto written = guild_asset_write_body(request, image, chosen, {body, bytes});
    RestClientSubmitResult result{RestClientSubmitError::body_capacity};
    if (written.ok() && written.bytes == bytes) {
        const bool large = bytes > kRestClientMaximumRequestBodyBytes;
        RestClientRequest submit{target,
                                 {body, bytes},
                                 guild_asset_route(request),
                                 request_id,
                                 large ? kGuildAssetLargeTimeoutMs : kRestClientDefaultTimeoutMs,
                                 true,
                                 false,
                                 reason};
        submit.multipart_boundary = chosen;
        result = large ? rest_client_submit_large_owned(rest, submit, &body)
                       : rest_client_submit(rest, submit);
    }
    if (body) {
        secure_clear_bytes(body, bytes);
        VirtualFree(body, 0, MEM_RELEASE);
    }
    return result;
}

} // namespace

RestClientSubmitResult account_guild_assets_submit(RestClient* rest, const GuildAssetRequest* request,
                                                   u64 request_id) noexcept {
    if (!rest || !request || !request_id || !guild_asset_request_valid(*request) ||
        request->operation == GuildAssetOperation::image_probe)
        return {RestClientSubmitError::invalid_input};
    u8 target[kGuildAssetMaximumTargetBytes];
    const auto path = guild_asset_write_target(*request, {target, sizeof(target)});
    if (!path.ok())
        return {RestClientSubmitError::target_capacity};
    u8 reason[kRestAuditReasonMaximumEncodedBytes];
    const auto encoded = guild_asset_write_reason(*request, {reason, sizeof(reason)});
    if (!encoded.ok())
        return {RestClientSubmitError::invalid_input};
    GuildImageBuffer buffer{};
    GuildPreparedImage image{};
    RestClientSubmitResult result{RestClientSubmitError::invalid_input};
    if (prepare(*request, &buffer, &image)) {
        if (guild_asset_has_body(request->operation))
            result = submit_with_body(rest, *request, image, {target, path.bytes}, {reason, encoded.bytes}, request_id);
        else
            result = rest_client_submit(rest, {{target, path.bytes},
                                               {},
                                               guild_asset_route(*request),
                                               request_id,
                                               kRestClientDefaultTimeoutMs,
                                               false,
                                               false,
                                               {reason, encoded.bytes}});
    }
    guild_image_release(&buffer);
    secure_clear_bytes(reason, encoded.bytes);
    return result;
}

bool account_guild_assets_probe(const GuildAssetRequest* request) noexcept {
    if (!request || request->operation != GuildAssetOperation::image_probe || !guild_asset_request_valid(*request))
        return false;
    GuildImageBuffer buffer{};
    GuildPreparedImage image{};
    const auto info = guild_image_prepare(guild_asset_profile(*request), request->image, &buffer, &image);
    bool ok = info.error == GuildImageError::none;
    // The caller sizes the URI from a first probe's info; the same file
    // prepares to the same bytes.
    if (ok && request->data_uri.count) {
        const u32 needed = guild_image_data_uri_bytes(image.kind, image.bytes.count);
        ok = needed && needed == request->data_uri.count &&
             guild_image_write_data_uri(image.kind, image.bytes, request->data_uri) == needed;
        if (!ok)
            secure_clear_bytes(request->data_uri.data, request->data_uri.count);
    }
    guild_image_release(&buffer);
    report(*request, info);
    return ok;
}

} // namespace maicord
