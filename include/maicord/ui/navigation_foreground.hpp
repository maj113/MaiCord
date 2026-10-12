#pragma once

#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/navigation_view.hpp"

namespace maicord {

// Navigation remains readable for admitted destinations, including read-only
// threads and thread browser parents. Sending authority does not affect location.
inline Color ui_navigation_foreground(const Theme& theme, const ChannelViewRow& row,
                                      bool selected) noexcept {
    if (selected || row.unread) return theme.text_primary;
    if (row.muted) {
        auto dimmed = ui_background_muted_text_color(theme);
        dimmed.alpha *= .6f;
        return dimmed;
    }
    if (row.selectable && (row.actions & channel_action_view)) return theme.text_secondary;
    return ui_background_muted_text_color(theme);
}

} // namespace maicord
