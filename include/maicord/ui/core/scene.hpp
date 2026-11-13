#pragma once

#include "maicord/ui/core/widget_storage.hpp"
#include "maicord/ui/layout/surface_geometry.hpp"

namespace maicord {

struct UiNoticeBand;
inline constexpr u32 kMaximumUiNodes = 32;
// The guild header dropdown: a chevron (X while open) at the trailing edge of
// the 32-DIP button, with 8-DIP rounded corners.
inline constexpr float kUiShellGuildMenuChevron = 12.0f;
inline constexpr float kUiShellGuildMenuRadius = 8.0f;
// Invite People beside the server name: a 32-DIP button with an 18-DIP glyph.
inline constexpr float kUiShellGuildInvite = 32.0f, kUiShellGuildInviteGlyph = 18.0f;
inline constexpr float kUiShellMinimumMessageWidthDips = 320.0f;

enum class UiSemantic : u8 {
    root,
    guild_rail,
    channel_sidebar,
    channel_header,
    channel_navigation,
    channel_footer,
    content,
    title_bar,
    title_label,
    member_toggle,
    message_view,
    composer,
    member_sidebar,
    home_button,
    guild_navigation,
    settings,
    friends_button,
    pins_toggle,
    threads_toggle,
    thread_header,
    thread_status,
    thread_primary,
    thread_overflow,
    thread_feedback,
    thread_cancel,
    private_create,
    private_add,
    private_call,
    // A one-to-one DM's header identity (avatar and name): opens the full profile, shows the other names.
    private_identity,
    inbox_toggle,
    // The server name and chevron above the channel list (guild views only):
    // opens the server header popout.
    guild_menu,
    // Invite People at the end of the server header (with the
    // ui_shell_layout_guild_invite flag).
    guild_invite,
    // Home's header: the "Find or start a conversation" bar, opening the quick switcher.
    private_search,
    // Home's "Direct Messages" heading above the conversations, holding private_create.
    private_heading,
    count,
};

enum class UiShellView : u8 {
    guild_channel,
    friends,
    private_channels,
    count,
};

enum UiShellLayoutFlags : u16 {
    ui_shell_layout_navigation = 1u << 0,
    ui_shell_layout_members = 1u << 1,
    ui_shell_layout_composer = 1u << 2,
    ui_shell_layout_pins = 1u << 3,
    ui_shell_layout_threads = 1u << 4,
    // A DM or group DM is open: the title bar offers add people and call.
    ui_shell_layout_private = 1u << 5,
    // The selected server has a channel the account may invite people to.
    ui_shell_layout_guild_invite = 1u << 6,
};

struct UiShellLayoutMetrics {
    u32 revision;
    u16 flags;
    float guild_rail_width;
    float navigation_width;
    float member_sidebar_width;
    float navigation_min_viewport_width;
    float member_min_viewport_width;
    float navigation_gutter_before;
    float content_gutter_before;
    float title_bar_height;
    float navigation_header_height;
    float navigation_footer_height;
    float member_sidebar_top;
    float message_bottom_margin;
    float composer_height;
    UiInsets composer_margin;
    UiInsets navigation_header_padding;
    UiInsets title_bar_padding;
    // Space kept free above the account panel (voice connection card). The
    // panel spans the server rail and the channel list, which both end above it.
    float navigation_footer_reserve;
    // The app-wide notice band's shown height (ui/shell/notice_band.hpp), 0 without one: the whole shell lays out
    // below it, so its panes and their hit-tests shift down by it.
    float notice_height;
    // The top part of composer_height (its typing strip) that draws over the bottom of the message list: the
    // list reaches that far below the composer's top, so the strip takes none of the list's bottom spacing.
    float composer_overlap;
    // A one-to-one DM header's identity (avatar and name) as one focusable button of this width, 0 without one
    // (UiSemantic::private_identity; ui_private_header_identity).
    float private_identity_width;
};

inline constexpr UiShellLayoutMetrics kGuildChannelShellLayoutMetrics{
    1,
    static_cast<u16>(ui_shell_layout_navigation | ui_shell_layout_members | ui_shell_layout_composer),
    64.0f,
    240.0f,
    240.0f,
    640.0f,
    1050.0f,
    0.0f,
    0.0f,
    48.0f,
    48.0f,
    48.0f,
    48.0f,
    10.0f,
    48.0f,
    UiInsets{kUiFloatingSurfaceInset, kUiFloatingSurfaceInset, kUiFloatingSurfaceInset,
             kUiFloatingSurfaceInset},
    UiInsets{16.0f, 4.0f, 16.0f, 4.0f},
    UiInsets{16.0f, 4.0f, 16.0f, 4.0f},
};

inline constexpr UiShellLayoutMetrics kFriendsShellLayoutMetrics{
    1,
    ui_shell_layout_navigation,
    64.0f,
    240.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    48.0f,
    48.0f,
    48.0f,
    0.0f,
    0.0f,
    0.0f,
    UiInsets{},
    UiInsets{16.0f, 4.0f, 16.0f, 4.0f},
    UiInsets{16.0f, 4.0f, 16.0f, 4.0f},
};

struct UiScene {
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    UiWidgetHandle semantics[static_cast<u32>(UiSemantic::count)];
    // Widgets of the last successful layout; zero before the first.
    u32 count;
    u32 layout_rebuilds;
    u32 layout_passes;
    float width;
    float height;
    u32 layout_metrics_revision;
    u8 layout_variant;
    // The metrics flags of the last layout.
    u16 layout_flags;
    UiShellView view;
    // Host state: the server header popout is open (chevron becomes an X).
    bool guild_menu_expanded;
    // Host state: the member toggle switches the DM profile (a one-to-one DM),
    // which the window is too narrow for while unavailable.
    bool side_panel_profile, side_panel_unavailable;
    // Host state: the app-wide notice band (ui/shell/notice_band.hpp) drawn over the root's top inset
    // (UiShellLayoutMetrics::notice_height); null without one.
    const UiNoticeBand* notice;
};

struct UiVirtualRange {
    u32 first;
    u32 count;
    float first_offset;
};

// Shared trailing header controls, in the shell's actual DIP coordinates.
// These rectangles also describe the retained member button's hit target.
struct UiShellHeaderGeometry {
    UiRect title;
    UiRect label;
    UiRect member_toggle;
    UiRect search_field;
    UiRect pins;
    UiRect threads;
    UiRect inbox;
};

// The height a message row is given before it is measured.
inline constexpr float kUiBootstrapMessageRowHeight = 68.0f;

bool ui_scene_initialize_bootstrap(UiScene* scene) noexcept;
void ui_scene_release(UiScene* scene) noexcept;
bool ui_scene_layout_with_metrics(UiScene* scene, UiShellView view, const UiShellLayoutMetrics& metrics,
                                  float width, float height) noexcept;
// The flags that decide the header's controls.
inline constexpr u16 kUiShellHeaderFlags = static_cast<u16>(ui_shell_layout_pins | ui_shell_layout_threads |
                                                             ui_shell_layout_private | ui_shell_layout_guild_invite);
// The last layout used other header flags than `flags` (a channel's type or
// permissions arrived since): the host lays the shell out again.
bool ui_scene_header_stale(const UiScene& scene, u16 flags) noexcept;
// Composer content width using layout's pane visibility and margins, without
// constructing widgets or running a layout pass.
float ui_scene_composer_width(const UiShellLayoutMetrics& metrics, float width) noexcept;
// The visible, laid-out node of `semantic`, or null.
const UiLayoutNode* ui_scene_node(const UiScene& scene, UiSemantic semantic) noexcept;
// The widget behind `semantic`'s node.
inline UiWidgetHandle ui_scene_widget(const UiScene& scene, UiSemantic semantic) noexcept {
    return scene.semantics[static_cast<u32>(semantic)];
}
UiRect ui_scene_content_rectangle(const UiScene& scene, UiSemantic semantic) noexcept;
UiShellHeaderGeometry ui_scene_header_geometry(const UiScene& scene) noexcept;

} // namespace maicord
