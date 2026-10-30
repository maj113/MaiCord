#pragma once
#include "maicord/ui/widget_manager.hpp"

namespace maicord::app_detail {
bool application_account_menu_open(UiWidgetHandle source, UiRect anchor) noexcept;
bool application_account_menu_action(u32 action) noexcept;
bool application_account_menu_key(u32 key) noexcept;
bool application_account_menu_synchronize() noexcept;
void application_account_menu_clear() noexcept;
void application_account_menu_release() noexcept;
} // namespace maicord::app_detail
