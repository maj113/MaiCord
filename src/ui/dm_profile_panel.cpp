#include "maicord/ui/dm_profile_panel.hpp"
#include "maicord/ui/scene.hpp"

namespace maicord {
namespace {
float member_width(const UiSidePanelInput& in) noexcept {
    return in.member_width > 0 ? in.member_width : kGuildChannelShellLayoutMetrics.member_sidebar_width;
}
bool one_to_one(const UiSidePanelInput& in) noexcept {
    return in.home && in.channel_type == 1;
}
} // namespace
UiSidePanelPlan ui_side_panel_plan(const UiSidePanelInput& in) noexcept {
    if (in.friends || (in.home && in.channel_type != 1 && in.channel_type != 3))
        return {};
    if (one_to_one(in)) {
        const bool unavailable = in.window_width < kUiDmProfilePanelMinimumWindowWidth;
        return {in.profile_visible && !unavailable ? UiSidePanel::profile : UiSidePanel::none, kUiDmProfilePanelWidth,
                true, unavailable};
    }
    return {in.members_visible ? UiSidePanel::members : UiSidePanel::none, member_width(in), true, false};
}
Utf8View ui_side_panel_toggle_label(bool profile, bool unavailable, bool shown) noexcept {
    if (profile)
        return unavailable ? utf8_literal("Show User Profile (Unavailable)")
               : shown     ? utf8_literal("Hide User Profile")
                           : utf8_literal("Show User Profile");
    return shown ? utf8_literal("Hide member list") : utf8_literal("Show member list");
}
bool ui_profile_fetch_due(u64 loaded_at, u64 failed_at, u64 now) noexcept {
    const u64 at = loaded_at > failed_at ? loaded_at : failed_at;
    return !at || now < at || now - at >= kUiProfileFreshMs;
}
float ui_side_panel_minimum_width(float rail, float navigation, const UiSidePanelInput& in) noexcept {
    return rail + navigation + (in.members_visible ? member_width(in) : 0.f) + kUiShellMinimumMessageWidthDips;
}
} // namespace maicord
