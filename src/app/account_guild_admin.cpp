#include "maicord/app/account_guild_admin.hpp"

#include "maicord/sys/crypto.hpp"

namespace maicord {
static_assert(kGuildAdminMaximumTargetBytes <= kRestClientMaximumTargetBytes);
namespace {
// Ban list pages: up to kGuildAdminPageMaximum bans with full user objects.
constexpr u32 kGuildAdminBanListResponseBytes = kRestClientMaximumResponseBytes;
// Per-server avatar and banner uploads may need up to two minutes.
constexpr u32 kLargeTimeoutMs = 120 * 1000;
constexpr auto kMd5HeaderName = utf8_literal("X-Discord-Original-MD5");

// Lowercase hex MD5 of an original image file; false when it can't be hashed.
bool md5_hex(ConstSlice<u8> data, char (&hex)[32]) noexcept {
    BCRYPT_HASH_HANDLE hash = nullptr;
    u8 digest[16]{};
    bool ok = BCryptCreateHash(BCRYPT_MD5_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    for (u32 at = 0; ok && at < data.count;) {
        const u32 chunk = data.count - at > 0x10000000u ? 0x10000000u : data.count - at;
        ok = BCryptHashData(hash, const_cast<u8*>(data.data + at), chunk, 0) >= 0;
        at += chunk;
    }
    ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash)
        BCryptDestroyHash(hash);
    if (ok)
        guild_admin_md5_hex(digest, hex);
    return ok;
}

// X-Discord-Original-MD5 of a guild PATCH carrying new images; zero bytes
// when it carries none.
u32 md5_header(const GuildAdminRequest& request, Slice<u8> output) noexcept {
    if (!request.guild || (!request.guild->banner_original.count && !request.guild->splash_original.count))
        return 0;
    char banner[32], splash[32];
    const bool has_banner = request.guild->banner_original.count && md5_hex(request.guild->banner_original, banner);
    const bool has_splash = request.guild->splash_original.count && md5_hex(request.guild->splash_original, splash);
    u8 value[160];
    const u32 bytes = guild_admin_write_md5_header(has_banner ? Utf8View{banner, 32} : Utf8View{},
                                                   has_splash ? Utf8View{splash, 32} : Utf8View{},
                                                   {value, sizeof(value)});
    if (!bytes)
        return 0;
    const u32 used = rest_append_extra_header(output, 0, kMd5HeaderName, {value, bytes});
    return used == ~0u ? 0 : used;
}
// Demand-backed body bank sized to the transport limit plus any image data
// URIs (per-server avatar or banner); cleared before release. Bodies over
// the ordinary ceiling take the large path.
__declspec(noinline) RestClientSubmitResult submit_with_body(RestClient* rest,
                                                             const GuildAdminRequest& request,
                                                             ConstSlice<u8> target,
                                                             ConstSlice<u8> reason, bool empty_reason,
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
    RestClientRequest submit{target,
                             {body, written.ok() ? written.bytes : 0},
                             guild_admin_route(request),
                             request_id,
                             large ? kLargeTimeoutMs : kRestClientDefaultTimeoutMs,
                             true,
                             false,
                             reason};
    submit.send_empty_audit_reason = empty_reason;
    u8 headers[256];
    submit.extra_headers = {headers, md5_header(request, {headers, sizeof(headers)})};
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
    // The official client sends "" as an empty header and null as none.
    const bool empty_reason = !encoded.bytes && guild_admin_reason_header(*request);
    RestClientSubmitResult result{};
    RestClientRequest bodyless{{target, path.bytes},
                               {},
                               guild_admin_route(*request),
                               request_id,
                               kRestClientDefaultTimeoutMs,
                               false,
                               false,
                               {reason, encoded.bytes}};
    bodyless.send_empty_audit_reason = empty_reason;
    if (guild_admin_has_body(request->operation))
        result = submit_with_body(rest, *request, {target, path.bytes}, {reason, encoded.bytes},
                                  empty_reason, request_id);
    else if (request->operation == GuildAdminOperation::ban_list)
        // One page of up to 1000 bans outgrows the ordinary response capacity.
        result = rest_client_submit_large_response(rest, bodyless, kGuildAdminBanListResponseBytes);
    else
        result = rest_client_submit(rest, bodyless);
    SecureZeroMemory(reason, encoded.bytes);
    return result;
}
} // namespace maicord
