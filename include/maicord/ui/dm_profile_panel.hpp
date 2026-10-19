#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/ui/geometry.hpp"

namespace maicord {
// The side slot beside a conversation: a server channel or group DM shows the
// member list there, a one-to-one DM the recipient's profile (the official
// client's user profile sidebar: 340 DIPs, only in windows at least 1132 DIPs
// wide). The header toggle and Ctrl+U switch whichever applies; each choice is
// remembered on its own.
inline constexpr float kUiDmProfilePanelWidth = 340.f;
inline constexpr float kUiDmProfilePanelMinimumWindowWidth = 1132.f;
enum class UiSidePanel : u8 { none, members, profile };
inline constexpr u8 kUiSidePanelNoChannel = 0;
struct UiSidePanelInput {
    // The selected channel's type (1 DM, 3 group DM, else a server channel);
    // kUiSidePanelNoChannel when nothing is selected.
    u8 channel_type;
    bool home, friends, members_visible, profile_visible;
    // The resized member pane width (0: the default) and the window's width.
    float member_width, window_width;
};
struct UiSidePanelPlan {
    // The slot's content (the shell still drops a slot that would leave the
    // chat narrower than kUiShellMinimumMessageWidthDips).
    UiSidePanel panel;
    // Slot width; nonzero whenever the header offers the toggle.
    float width;
    bool toggle;
    // The profile cannot show at this window width: the toggle is disabled.
    bool unavailable;
};
UiSidePanelPlan ui_side_panel_plan(const UiSidePanelInput&) noexcept;
// The header toggle's tooltip: for the DM profile or the member list, which
// is shown now.
Utf8View ui_side_panel_toggle_label(bool profile, bool unavailable, bool shown) noexcept;
// Profile fetches (the official client's policy): due when never fetched, or
// a minute after the last one ended, whether it loaded or failed (a 404 or a
// 429 waits the same minute). Times are monotonic milliseconds, zero: never.
inline constexpr u64 kUiProfileFreshMs = 60'000;
bool ui_profile_fetch_due(u64 loaded_at, u64 failed_at, u64 now) noexcept;
// Smallest client width in DIPs for the account shell: rail, channel list, a
// shown member list and a readable chat. The DM profile adds nothing: below
// its breakpoint it gives way.
float ui_side_panel_minimum_width(float rail, float navigation, const UiSidePanelInput&) noexcept;
} // namespace maicord
