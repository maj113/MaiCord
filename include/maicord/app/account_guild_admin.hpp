#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/guild_admin.hpp"

namespace maicord {
// Uses the existing authenticated DETAILS owner. Target, body and audit reason
// are copied synchronously; the response is taken later through rest_take.
RestClientSubmitResult account_guild_admin_submit(RestClient*, const GuildAdminRequest*,
                                                  u64 request_id) noexcept;
} // namespace maicord
