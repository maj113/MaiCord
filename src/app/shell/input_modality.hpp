#pragma once
#include "maicord/core/base.hpp"
#include "maicord/platform/window/event.hpp"
#include "maicord/platform/window/key.hpp"

namespace maicord::app_detail {
// Whether keyboard focus shows after this event: a pointer press hides it, a navigation key shows it, anything
// else leaves it as it is. The second press of a quick pair (a double click) is not a press here, as it was not
// for the system's own WM_xBUTTONDBLCLK.
inline bool keyboard_focus_after_event(bool visible, const WindowEvent& event, bool shift) noexcept {
    if (event.type == EventType::pointer_down) {
        const bool primary = event.button == static_cast<u8>(PointerButton::left);
        if (primary || (event.flags & ev_double_click) == 0)
            return false;
        return visible;
    }
    if (event.type != EventType::key_down)
        return visible;
    const u32 key = event.key.key;
    return visible || key == key::tab || key == key::f6 || key == key::menu || key == key::up || key == key::down ||
        key == key::left || key == key::right || key == key::home || key == key::end || key == key::page_up ||
        key == key::page_down || key == key::enter || key == key::space || (key == key::f10 && shift);
}
}
