#include "maicord/app/chat/threads/account_forum_post.hpp"

#include "maicord/app/net/request_body_bank.hpp"
#include "maicord/core/memory.hpp"

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
    RequestBodyBank body{size.bytes};
    if (!body.data)
        return {RestClientSubmitError::memory};
    body.used = size.bytes;
    const auto written = forum_post_write_body(request, body.all());
    const auto route =
        request.operation == ForumPostOperation::attachment_url
            ? attachment_upload_route(request.parent_id)
            : RestRouteKey{kForumPostRouteId, request.parent_id, RestMethod::post,
                           RestMajorParameter::channel};
    return written.ok() ? rest_client_submit(rest, target, {body.data, written.bytes}, route, request_id, true)
                        : RestClientSubmitResult{RestClientSubmitError::body_capacity};
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
} // namespace maicord
