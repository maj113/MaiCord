#pragma once

#include "../src/app/application_guild_invite.hpp"

// These fixtures exercise other host owners without an invite flow.
namespace maicord::app_detail {
bool application_guild_invite_pending(u64) noexcept {
    return false;
}
bool application_guild_invite_submit(u64, u64) noexcept {
    return false;
}
bool application_guild_invite_response(const RestClientResponse&, Utf8View) noexcept {
    return false;
}
} // namespace maicord::app_detail
