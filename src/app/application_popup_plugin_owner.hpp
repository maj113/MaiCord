#pragma once
#include "application_state.hpp"
#include "application_plugins.hpp"

namespace maicord::app_detail {
// Menu builders capture the new plugin rows before selecting their popup
// owner. Preserve only owners the binder understands; it still validates the
// exact account, target, popup and menu revision.
inline void application_popup_plugin_owner_transition(PopupOwnerKind owner) noexcept {
    switch (owner) {
    case PopupOwnerKind::member:
    case PopupOwnerKind::friends:
    case PopupOwnerKind::message:
    case PopupOwnerKind::channel:
    case PopupOwnerKind::private_channel:
    case PopupOwnerKind::guild:
        return;
    default:
        application_plugins_menu_dismissed();
    }
}
} // namespace maicord::app_detail
