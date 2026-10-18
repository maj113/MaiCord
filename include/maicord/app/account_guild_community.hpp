#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/guild_community.hpp"

namespace maicord {
// Membership screening, onboarding and scheduled event requests on the
// authenticated DETAILS owner. Target and body are copied synchronously; the
// response is taken later through rest_take.
// session_id is the Gateway session a guild_join names (empty omits it).
RestClientSubmitResult account_guild_community_submit(RestClient*, const GuildCommunityRequest*,
                                                      u64 request_id, Utf8View session_id = {}) noexcept;
} // namespace maicord
