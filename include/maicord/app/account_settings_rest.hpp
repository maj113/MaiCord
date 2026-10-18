#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/account_settings.hpp"

namespace maicord {
// Account settings on the authenticated coordinator: settings-proto/1 on the
// PRIMARY client, account pages on DETAILS. The request is serialized
// synchronously; rest_take returns the response.
RestClientSubmitResult account_settings_submit(RestClient*, const AccountSettingsRequest&,
                                               u64 request_id) noexcept;
} // namespace maicord
