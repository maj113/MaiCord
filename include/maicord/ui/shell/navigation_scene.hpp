#pragma once

#include "maicord/ui/shell/navigation_view.hpp"
#include "maicord/ui/shell/navigation_drag.hpp"
#include "maicord/ui/core/widget_manager.hpp"

namespace maicord {

struct UiVirtualRowStorageOwner;

inline constexpr u32 kShellNavigationOverscanRows = 2;
// Guild channel rows (the official client's compact density): a 30-DIP head
// whose 28-DIP highlight runs from 6 DIPs in to 8 DIPs before the edge, a
// 20-DIP glyph 6 DIPs inside the highlight and the name 6 DIPs after it; each
// depth indents 12 DIPs. Voice channels grow by a 32-DIP row per connected user.
inline constexpr float kShellNavigationVoiceHeadHeight = 30.0f;
inline constexpr float kShellNavigationVoiceUserHeight = 32.0f;
inline constexpr float kShellChannelHighlightLeft = 6.0f, kShellChannelHighlightRight = 8.0f;
inline constexpr float kShellChannelIcon = 20.0f, kShellChannelIconLeft = 12.0f;
inline constexpr float kShellChannelLabelLeft = 38.0f, kShellChannelIndent = 12.0f;
// A category is 32 DIPs: 8 above its 24-DIP line, the name 12 DIPs in and the
// 12-DIP chevron 4 DIPs after the name.
inline constexpr float kShellCategoryHeight = 32.0f, kShellCategoryLineTop = 8.0f;
inline constexpr float kShellCategoryNameLeft = 12.0f, kShellCategoryChevron = 12.0f, kShellCategoryGap = 4.0f;
// Row controls (Create Invite and Edit Channel on a hovered or selected
// channel, a category's Create Channel): 16-DIP buttons 4 DIPs apart, ending
// 4 DIPs inside the highlight (a category's 6 DIPs from the edge).
inline constexpr float kShellChannelControl = 16.0f, kShellChannelControlGap = 4.0f;
// The rows leading a server's list (events) end with a 9-DIP divider band whose
// 1-DIP line runs from 12 DIPs in to 3 DIPs before the edge.
inline constexpr float kShellNavigationDividerBand = 9.0f;
// A live stage's topic or a voice channel's status takes one line under the
// head; "Invite to Voice" follows the connected channel's users.
inline constexpr float kShellNavigationStageTopicHeight = 20.0f;
inline constexpr float kShellNavigationVoiceInviteHeight = 32.0f;
constexpr float shell_navigation_voice_top(const ChannelViewRow& row) noexcept {
    return kShellNavigationVoiceHeadHeight +
           (row.stage_live || row.voice_subtitle ? kShellNavigationStageTopicHeight : 0.0f);
}

// Threads draw with a spine instead of an icon: a rail under the
// parent channel icon that curves into each thread row and continues down
// while sibling threads follow. Points are rail centre lines in DIPs.
inline constexpr float kShellThreadSpineWidth = 2.0f;
inline constexpr float kShellThreadSpineRadius = 8.0f;
constexpr bool shell_navigation_thread_row(const ChannelViewRow& row) noexcept {
    return row.type >= 10 && row.type <= 12;
}
struct ShellThreadSpine {
    UiPoint rail_top, arc_start, arc_end, arm_end;
    float continue_bottom; // rail end below the elbow; arc_start.y when it stops
};
// icon is the thread row's (unpainted) icon slot; the parent icon sits one
// indent (12 DIP) to its left. first starts the rail just below that icon.
ShellThreadSpine shell_navigation_thread_spine(UiRect row, float row_height, UiRect icon, bool first,
                                               bool continues) noexcept;

enum class ShellNavigationSceneError : u8 {
    none,
    invalid_input,
    capacity,
    widget,
    layout,
};

struct ShellNavigationSceneResult {
    ShellNavigationSceneError error;
    u32 first;
    u32 count;
    float scroll_dips;
    bool changed;

    constexpr bool ok() const noexcept {
        return error == ShellNavigationSceneError::none;
    }
};

struct ShellNavigationRowWidget {
    ChannelViewRow view;
    UiWidgetHandle widget;
    UiWidgetHandle icon;
    UiWidgetHandle label;
    UiWidgetHandle mention_badge;
    u32 source_index;
};

struct ShellNavigationHit {
    u64 id;
    UiWidgetHandle widget;
    u32 source_index;
    bool selectable;
};

struct ShellNavigationScene {
    UiWidgetManager widgets;
    Slice<ShellNavigationRowWidget> rows;
    UiVirtualRowStorageOwner* row_storage;
    UiWidgetHandle root;
    UiRect viewport;
    const ChannelViewRow* source_rows;
    u64 focused_channel_id;
    float content_height;
    float scroll_dips;
    u32 source_revision;
    u32 source_count;
    u32 first;
    u32 count;
    u32 rebuilds;
    u32 layout_passes;
    u32 rejected;
    bool active;
    // Drag-and-drop reorder: the dimmed source and the drop line or ring.
    NavigationDragVisual drag;
    // Host-set through shell_navigation_scene_set_banner: the list's extra top
    // inset under the server banner, whether the pointer is over the header, and
    // a revision the presentation stamp follows for banner media and hover changes.
    float banner_inset;
    bool banner_header_hovered;
    u32 banner_revision;
};

float shell_navigation_row_height(const ChannelViewRow& row) noexcept;
// The controls a row shows now: Create Invite and Edit Channel while the
// channel is hovered or selected, a category's Create Channel always.
u8 shell_navigation_row_controls(const ChannelViewRow& row, bool hovered, bool selected) noexcept;
// A control's button in the row laid out at `row` (right to left: Edit
// Channel, Create Invite, both before a mention badge); empty when the row
// lacks the control.
UiRect shell_navigation_control_rectangle(const UiRect& row, const ChannelViewRow& view, ChannelRowControl control) noexcept;
// Where a row's name must end: before its leftmost shown control, else `right`.
float shell_navigation_controls_left(const UiRect& row, const ChannelViewRow& view, u8 shown, float right) noexcept;
// Rows plus the 8 DIP insets above and below them (banner inset excluded).
float shell_navigation_content_height(const NavigationView* view) noexcept;
// The banner state for the selected server: inset 0 (no banner) or
// kUiGuildBannerListInset. A changed inset rebuilds the list on the next update.
bool shell_navigation_scene_set_banner(ShellNavigationScene* scene, float inset, bool header_hovered,
                                       u32 revision) noexcept;
constexpr float shell_navigation_top_inset(const ShellNavigationScene& scene) noexcept {
    return 8.0f + scene.banner_inset;
}
bool shell_navigation_scene_initialize(ShellNavigationScene* scene,
                                       Slice<ShellNavigationRowWidget> row_storage,
                                       Slice<UiWidget> widget_storage,
                                       Slice<UiLayoutNode> layout_storage) noexcept;
ShellNavigationSceneResult shell_navigation_scene_update(ShellNavigationScene* scene,
                                                         const NavigationView* view, const UiRect& viewport,
                                                         float requested_scroll_dips) noexcept;
ShellNavigationSceneResult shell_navigation_scene_scroll(ShellNavigationScene* scene,
                                                         const NavigationView* view,
                                                         float delta_dips) noexcept;
const ShellNavigationRowWidget* shell_navigation_scene_resolve(const ShellNavigationScene& scene,
                                                               UiWidgetHandle widget) noexcept;
const UiLayoutNode* shell_navigation_scene_layout_node(const ShellNavigationScene& scene,
                                                       const ShellNavigationRowWidget& row) noexcept;
ShellNavigationHit shell_navigation_scene_hit_test(const ShellNavigationScene& scene, float x,
                                                   float y) noexcept;
bool shell_navigation_scene_focus_channel(ShellNavigationScene* scene, u64 channel_id) noexcept;
bool shell_navigation_scene_reveal_channel(ShellNavigationScene* scene, const NavigationView& view,
                                           u64 channel_id) noexcept;
ShellNavigationHit shell_navigation_scene_page(ShellNavigationScene* scene, const NavigationView& view,
                                              bool reverse) noexcept;

} // namespace maicord
