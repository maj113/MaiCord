#pragma once

#include "../src/app/application_member_moderation.hpp"

// These fixtures exercise other host owners without a member moderation flow.
namespace maicord::app_detail {
bool application_member_moderation_action(u32) noexcept {
    return false;
}
u32 application_member_moderation_menu_items(u64, u64, Slice<PopupItem>) noexcept {
    return 0;
}
bool application_member_moderation_activate(u32) noexcept {
    return false;
}
bool application_member_moderation_pending(u64) noexcept {
    return false;
}
bool application_member_moderation_submit(u64, u64) noexcept {
    return false;
}
bool application_member_moderation_response(const RestClientResponse&, Utf8View) noexcept {
    return false;
}
u64 application_member_moderation_next_wake() noexcept {
    return 0;
}
void application_member_moderation_release() noexcept {}
} // namespace maicord::app_detail
