#pragma once

#include "maicord/ui/scene.hpp"

namespace maicord {

// Standalone text/overlay probes have no account navigation, member pane, or
// composer. Keep the shell's rail and title geometry, without reserving space
// for account surfaces that these fixtures never bind or draw.
inline constexpr UiShellLayoutMetrics kStandaloneTitleLayoutMetrics = []() constexpr {
    UiShellLayoutMetrics metrics = kGuildChannelShellLayoutMetrics;
    metrics.revision = 2;
    metrics.flags = 0;
    metrics.navigation_width = 0.0f;
    metrics.member_sidebar_width = 0.0f;
    metrics.navigation_min_viewport_width = 0.0f;
    metrics.member_min_viewport_width = 0.0f;
    metrics.navigation_gutter_before = 0.0f;
    metrics.content_gutter_before = 0.0f;
    metrics.navigation_header_height = 0.0f;
    metrics.navigation_footer_height = 0.0f;
    metrics.member_sidebar_top = 0.0f;
    metrics.message_bottom_margin = 0.0f;
    metrics.composer_height = 0.0f;
    metrics.composer_margin = UiInsets{};
    metrics.navigation_header_padding = UiInsets{};
    return metrics;
}();

inline bool standalone_title_layout(UiScene* scene, float width_dips, float height_dips) noexcept {
    return ui_scene_layout_with_metrics(scene, UiShellView::guild_channel,
                                       kStandaloneTitleLayoutMetrics, width_dips, height_dips);
}

} // namespace maicord
