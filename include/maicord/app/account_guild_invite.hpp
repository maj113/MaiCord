#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/guild_invite.hpp"

namespace maicord {
// Invite resolve/accept on the authenticated DETAILS coordinator. The code is
// serialized synchronously; rest_take returns the response.
// Accept sends the Gateway session ID and the invite's context header.
RestClientSubmitResult account_guild_invite_submit(RestClient*, const GuildInviteRequest*,
                                                   u64 request_id, Utf8View session_id = {}) noexcept;
} // namespace maicord
