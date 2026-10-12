#pragma once

#include "../src/app/application_soundboard.hpp"

// These fixtures exercise other request owners without soundboard work queued.
namespace maicord::app_detail {
bool application_soundboard_pending(u64) noexcept {
    return false;
}
bool application_soundboard_submit(u64, u64) noexcept {
    return false;
}
bool application_soundboard_response(const RestClientResponse&, Utf8View) noexcept {
    return false;
}
} // namespace maicord::app_detail
