#pragma once

#include "../src/app/application_private_channel.hpp"

// These fixtures exercise other host owners without group DM actions.
namespace maicord::app_detail {
bool application_private_channel_pending(u64) noexcept {
    return false;
}
bool application_private_channel_submit(u64, u64) noexcept {
    return false;
}
bool application_private_channel_response(const RestClientResponse&, Utf8View) noexcept {
    return false;
}
} // namespace maicord::app_detail
