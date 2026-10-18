#include "maicord/app/account_forum_post.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
static_assert(kForumPostMaximumTargetBytes <= kRestClientMaximumTargetBytes);
static_assert(kForumPostMaximumBodyBytes <= kRestClientMaximumRequestBodyBytes);
namespace {
// Demand-backed metadata workspace. Submit copies before this bank is cleared
// and released; no upload references or large serialization stack survive.
__declspec(noinline) RestClientSubmitResult submit_body(RestClient* rest,
                                                        const ForumPostRequest& request,
                                                        ConstSlice<u8> target,
                                                        u64 request_id) noexcept {
    const auto size = forum_post_body_bytes(request);
    if (!size.ok() || !size.bytes)
        return {RestClientSubmitError::body_capacity};
    auto* body =
        static_cast<u8*>(VirtualAlloc(nullptr, size.bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!body)
        return {RestClientSubmitError::memory};
    const auto written = forum_post_write_body(request, {body, size.bytes});
    const auto route =
        request.operation == ForumPostOperation::attachment_url
            ? attachment_upload_route(request.parent_id)
            : RestRouteKey{kForumPostRouteId, request.parent_id, RestMethod::post,
                           RestMajorParameter::channel};
    const auto result = written.ok()
                            ? rest_client_submit(rest, target, {body, written.bytes}, route,
                                                 request_id, true)
                            : RestClientSubmitResult{RestClientSubmitError::body_capacity};
    secure_clear_bytes(body, size.bytes);
    VirtualFree(body, 0, MEM_RELEASE);
    return result;
}
} // namespace
RestClientSubmitResult account_forum_post_submit(RestClient* rest,
                                                 const ForumPostRequest* request,
                                                 u64 request_id) noexcept {
    if (!rest || !request || !request_id || !forum_post_request_valid(*request))
        return {RestClientSubmitError::invalid_input};
    u8 target[kForumPostMaximumTargetBytes];
    const auto path = forum_post_write_target(*request, {target, sizeof(target)});
    if (!path.ok())
        return {RestClientSubmitError::target_capacity};
    if (request->operation != ForumPostOperation::parent_metadata)
        return submit_body(rest, *request, {target, path.bytes}, request_id);
    return rest_client_submit(rest, {target, path.bytes},
                              {},
                              {kForumPostRouteId, request->parent_id, RestMethod::get,
                               RestMajorParameter::channel},
                              request_id,
                              false);
}
RestClientSubmitResult account_forum_post_cancel(RestClient* rest, u64 request_id) noexcept {
    return rest_client_cancel_result(rest, request_id);
}
} // namespace maicord
