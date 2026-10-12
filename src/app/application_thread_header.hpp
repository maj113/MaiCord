#pragma once
#include "application_thread_header_activation.hpp"
#include "maicord/ui/scene.hpp"

namespace maicord::app_detail {
inline constexpr UiThreadMenuActions kApplicationThreadMenuActions{7'000, 7'001, 7'002, 7'003, 7'004, 7'005};
// available starts directly below the channel title and ends above composer.
// Returns the occupied height; caller shifts only message_view.top by it.
float application_thread_header_project(UiThreadHeader*, UiScene*, u64 thread_id,
    UiRect available, UiRect clip) noexcept;
ApplicationThreadHeaderActivationSource application_thread_header_activation_source(
    UiThreadHeader*, const ApplicationThreadActionTicket&) noexcept;
bool application_thread_header_activate_target(UiThreadHeader*, const UiThreadHeaderTarget&) noexcept;
// Implemented alongside ordinary channel-context dispatch, retaining plugin
// rows for sidebar menus and an independent exact thread target ticket.
bool application_thread_actions_menu_open(const ApplicationThreadActionTicket&, UiRect anchor) noexcept;
bool application_refresh_thread_actions_menu() noexcept;
} // namespace maicord::app_detail
