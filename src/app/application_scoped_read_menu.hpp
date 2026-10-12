#pragma once

#include "maicord/app/read_scope.hpp"
#include "maicord/ui/geometry.hpp"

namespace maicord::app_detail {
// Native scope menus share the production popup/input widgets. The controller
// owns all request and progress state; closing a menu does not cancel its work.
bool application_scoped_read_menu_open(ReadScopeKind kind, u64 id, UiRect anchor) noexcept;
bool application_scoped_read_menu_action(u32 action) noexcept;
bool application_scoped_read_menu_activate(u32 action) noexcept;
bool application_scoped_read_menu_refresh() noexcept;
void application_scoped_read_menu_clear() noexcept;
u32 application_scoped_read_menu_owner_bytes() noexcept;
// Called after modal/editor input priority and modifier/repeat checks.
bool application_scoped_read_shortcut() noexcept;
} // namespace maicord::app_detail
