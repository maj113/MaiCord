#pragma once

#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/list_navigation.hpp"

namespace maicord {

enum class NavigationMode : u8 { none, channel, guild, unread, mention };
struct NavigationShortcut { NavigationMode mode; bool reverse; };
struct NavigationTarget { u64 id; u32 index; bool guild; bool home; };

NavigationShortcut navigation_shortcut(u32 key, bool control, bool alt, bool shift) noexcept;
NavigationTarget navigation_target(const NavigationView& view, NavigationShortcut shortcut) noexcept;
NavigationTarget navigation_focus_target(const NavigationView& view, bool guild,
                                         u64 focused_id, UiListMove move) noexcept;

} // namespace maicord
