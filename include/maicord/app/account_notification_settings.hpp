#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/user_guild_settings.hpp"

namespace maicord {
// User guild notification settings PATCH and recent mentions GET on the
// borrowed DETAILS owner, which copies target and body synchronously.
RestClientSubmitResult account_guild_settings_submit(RestClient*, const UserGuildSettingsPatch*,
                                                     u64 request_id) noexcept;
RestClientSubmitResult account_mentions_submit(RestClient*, const MentionsQuery*,
                                               u64 request_id) noexcept;
// Exact ID only; an already published response remains available to rest_take.
RestClientSubmitResult account_notification_request_cancel(RestClient*, u64 request_id) noexcept;
} // namespace maicord
