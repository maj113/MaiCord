#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/guild_moderation.hpp"

namespace maicord {
// Members search, bulk ban and ban search on the authenticated DETAILS owner.
// Target, body and reason are copied synchronously; the response is taken
// later through rest_take.
RestClientSubmitResult account_guild_moderation_submit(RestClient*, const GuildModerationRequest*,
                                                       u64 request_id) noexcept;
// Exact request ID only; a superseded search is cancelled this way.
RestClientSubmitResult account_guild_moderation_cancel(RestClient*, u64 request_id) noexcept;
} // namespace maicord
