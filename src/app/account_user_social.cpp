#include "maicord/app/account_user_social.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
static_assert(kUserSocialMaximumTargetBytes <= kRestClientMaximumTargetBytes);
static_assert(kUserSocialMaximumBodyBytes <= kRestClientMaximumRequestBodyBytes);

RestClientSubmitResult account_user_social_submit(RestClient* rest, const UserSocialRequest* request,
                                                  u64 request_id) noexcept {
    if (!rest || !request || !request_id || !user_social_request_valid(*request))
        return {RestClientSubmitError::invalid_input};
    u8 target[kUserSocialMaximumTargetBytes];
    const auto path = user_social_write_target(*request, {target, sizeof(target)});
    if (!path.ok())
        return {RestClientSubmitError::target_capacity};
    u8 body[kUserSocialMaximumBodyBytes];
    UserSocialWriteResult written{};
    if (user_social_has_body(request->operation)) {
        written = user_social_write_body(*request, {body, sizeof(body)});
        if (!written.ok())
            return {RestClientSubmitError::body_capacity};
    }
    const auto result = rest_client_submit(rest, {target, path.bytes},
                                           written.bytes ? ConstSlice<u8>{body, written.bytes}
                                                                 : ConstSlice<u8>{},
                                           user_social_route(*request),
                                           request_id,
                                           written.bytes != 0);
    secure_clear_bytes(body, written.bytes);
    return result;
}

RestClientSubmitResult account_user_social_cancel(RestClient* rest, u64 request_id) noexcept {
    return rest_client_cancel_result(rest, request_id);
}
} // namespace maicord
