#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/user_social.hpp"

namespace maicord {
// Notes, report menus/submissions and message flag edits on the authenticated
// DETAILS owner. Target and body are copied synchronously; the response is
// taken later through rest_take.
RestClientSubmitResult account_user_social_submit(RestClient*, const UserSocialRequest*,
                                                  u64 request_id) noexcept;
} // namespace maicord
