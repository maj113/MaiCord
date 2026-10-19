#pragma once

#include "../src/app/application_dm_profile.hpp"

// These fixtures exercise other request owners without a DM profile fetch queued.
namespace maicord::app_detail {
bool application_dm_profile_pending(u64) noexcept {
    return false;
}
bool application_dm_profile_submit(u64, u64) noexcept {
    return false;
}
bool application_dm_profile_response(const RestClientResponse&, Utf8View) noexcept {
    return false;
}
} // namespace maicord::app_detail
