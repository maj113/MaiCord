#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
struct RestClientResponse;
}

namespace maicord::app_detail {
// Opens the native join dialog for an invite URL; false when url is not an invite.
bool application_guild_invite_open_url(Utf8View url) noexcept;
bool application_guild_invite_open(Utf8View code) noexcept;
// DETAILS lane: resolve then accept, one request at a time.
bool application_guild_invite_pending(u64 now_ms) noexcept;
bool application_guild_invite_submit(u64 request_id, u64 now_ms) noexcept;
bool application_guild_invite_response(const RestClientResponse& response, Utf8View body) noexcept;
// Frame time: switches to the joined guild once its GUILD_CREATE arrives.
void application_guild_invite_synchronize() noexcept;
void application_guild_invite_release() noexcept;
} // namespace maicord::app_detail
