#pragma once

#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/navigation_drag.hpp"
#include "maicord/ui/widget_manager.hpp"
#include "maicord/ui/widget_storage.hpp"
#include "maicord/ui/transition.hpp"

namespace maicord {

inline constexpr u32 kGuildRailOverscanRows = 2;
// Vertical gap between rail rows (drop lines sit in its middle).
inline constexpr float kGuildRailRowGapDips = 18.0f;

// Guild indicator transition: 8/20/40px, 200ms CSS ease-out.
struct GuildRailIndicatorMotion {
    struct Scope { u64 owner, generation; } scope;
    u64 started_ms, sampled_ms;
    float value, from, target;
    bool active;
};
// shape: the icon's circle (0) to rounded square (1) morph while hovered or
// selected, on the pill's 200 ms CSS ease-out.
struct GuildRailIndicator {
    GuildRailIndicatorMotion height, visibility, shape;
};
inline UiMotionTransition guild_rail_indicator_transition(const GuildRailIndicatorMotion& motion) noexcept {
    return {{motion.scope.owner, motion.scope.generation}, motion.started_ms, motion.sampled_ms,
            motion.value, motion.from, motion.target, {200, UiMotionCurve::css_ease_out}, motion.active};
}
inline bool guild_rail_indicator_parameter_tick(GuildRailIndicatorMotion* motion, UiMotionScope scope,
                                                float target, u64 now_ms, bool animate) noexcept {
    auto transition = guild_rail_indicator_transition(*motion);
    if (motion->scope.owner != scope.owner || motion->scope.generation != scope.generation)
        ui_motion_transition_reset(&transition, scope, target);
    const bool changed = ui_motion_transition_tick(&transition, scope, target, now_ms,
        {200, UiMotionCurve::css_ease_out}, !animate, true);
    *motion = {{transition.scope.owner, transition.scope.generation}, transition.started_ms,
               transition.sampled_ms, transition.value, transition.from, transition.target, transition.active};
    return changed;
}
inline bool guild_rail_indicator_tick(GuildRailIndicator* pill, UiMotionScope scope,
                                      bool selected, bool hovered, bool unread,
                                      u64 now_ms, bool animate) noexcept {
    const float height = selected ? 1.f : hovered ? .375f : 0.f;
    const float visibility = selected || hovered || unread ? 1.f : 0.f;
    bool changed = guild_rail_indicator_parameter_tick(&pill->height, scope, height, now_ms, animate);
    changed = guild_rail_indicator_parameter_tick(&pill->shape, scope, selected || hovered ? 1.f : 0.f, now_ms,
                                                  animate) || changed;
    return guild_rail_indicator_parameter_tick(&pill->visibility, scope, visibility, now_ms, animate) || changed;
}
inline u64 guild_rail_indicator_next_wake(const GuildRailIndicator& pill, u64 now_ms) noexcept {
    u64 wake = 0;
    const GuildRailIndicatorMotion* motions[]{&pill.height, &pill.visibility, &pill.shape};
    for (const auto* motion : motions) {
        const auto transition = guild_rail_indicator_transition(*motion);
        const u64 deadline = ui_motion_transition_next_wake(transition, transition.scope, now_ms, false, true);
        if (deadline && (!wake || deadline < wake))
            wake = deadline;
    }
    return wake;
}
// Server icon: a circle (border-radius 50%) that morphs to a
// rounded square (30%) while hovered or selected. The sampled shape motion
// drives it; an unsampled row (no tick yet) takes its settled state.
inline float guild_rail_icon_radius(UiRect icon, const GuildRailIndicator* motion, bool selected,
                                    bool hovered) noexcept {
    const float shape = motion && motion->shape.scope.owner ? motion->shape.value : selected || hovered ? 1.f : 0.f;
    const float extent = minimum(ui_rect_width(icon), ui_rect_height(icon));
    return extent * (.5f - .2f * shape);
}

enum class GuildRailSceneError : u8 {
    none,
    invalid_input,
    capacity,
    widget,
    layout,
};

struct GuildRailSceneResult {
    GuildRailSceneError error;
    u32 first;
    u32 count;
    float scroll_dips;
    bool changed;

    constexpr bool ok() const noexcept {
        return error == GuildRailSceneError::none;
    }
};

struct GuildRailRowWidget {
    GuildViewRow view;
    UiWidgetHandle widget;
    u32 source_index;
    GuildRailIndicator indicator;
    float reveal; // folder member extent while expanding/collapsing (0..1]
};

// Folder expand/collapse: 150 ms CSS ease-out on member row extent.
inline constexpr u32 kGuildRailFolderMotionMs = 150;
inline constexpr u32 kGuildRailFolderMotionSlots = 8;
// Plain fields keep GuildRailScene constant-initializable.
struct GuildRailFolderMotion {
    u64 folder_id;
    u64 started_ms, sampled_ms;
    float value, from, target;
    bool active;
};

struct GuildRailHit {
    u64 id;
    UiWidgetHandle widget;
    u32 source_index;
    bool selectable;
    bool folder; // id is a folder header row id
    bool add_server; // the "Add a server" button after the last row (id is zero)
};

struct GuildRailScene {
    UiWidgetManager widgets;
    Slice<GuildRailRowWidget> rows;
    UiVirtualRowStorageOwner* row_storage;
    UiWidgetHandle root;
    UiRect viewport;
    const GuildViewRow* source_rows;
    u64 focused_guild_id;
    float content_height;
    float scroll_dips;
    u32 source_revision;
    u32 source_count;
    u32 first;
    u32 count;
    u32 rebuilds;
    u32 layout_passes;
    u32 data_updates;
    u32 rejected;
    bool active;
    GuildRailIndicator home_indicator;
    GuildRailFolderMotion folder_motion[kGuildRailFolderMotionSlots];
    float spacer_height;
    // Drag-and-drop reorder: the dimmed source and the drop line or ring.
    NavigationDragVisual drag;
    // "Add a server" button after the last row (list coordinates).
    UiWidgetHandle add_button;
};

// The add button's laid-out rectangle; empty while it is not materialized.
UiRect guild_rail_scene_add_rectangle(const GuildRailScene& scene) noexcept;
bool guild_rail_scene_initialize(GuildRailScene* scene, Slice<GuildRailRowWidget> row_storage,
                                 Slice<UiWidget> widget_storage,
                                 Slice<UiLayoutNode> layout_storage) noexcept;
GuildRailSceneResult guild_rail_scene_update(GuildRailScene* scene, const NavigationView* view,
                                             UiRect viewport, float requested_scroll_dips) noexcept;
GuildRailSceneResult guild_rail_scene_scroll(GuildRailScene* scene, const NavigationView* view,
                                             float delta_dips) noexcept;
const GuildRailRowWidget* guild_rail_scene_resolve(const GuildRailScene& scene,
                                                   UiWidgetHandle widget) noexcept;
const UiLayoutNode* guild_rail_scene_layout_node(const GuildRailScene& scene,
                                                 const GuildRailRowWidget& row) noexcept;
GuildRailHit guild_rail_scene_hit_test(const GuildRailScene& scene, float x, float y) noexcept;
bool guild_rail_scene_focus_guild(GuildRailScene* scene, u64 guild_id) noexcept;
bool guild_rail_scene_reveal_guild(GuildRailScene* scene, const NavigationView& view, u64 guild_id) noexcept;
GuildRailHit guild_rail_scene_page(GuildRailScene* scene, const NavigationView& view,
                                  bool reverse, bool from_home = false) noexcept;
// Starts the extent transition for a folder whose expansion the host just
// changed; the next navigation rebuild carries the new folder_expanded flag.
bool guild_rail_scene_folder_toggled(GuildRailScene* scene, u64 folder_id, bool expanded, u64 now_ms,
                                     bool animate) noexcept;
// Advances folder transitions; true when the host must re-run the update.
bool guild_rail_scene_tick(GuildRailScene* scene, u64 now_ms, bool animate) noexcept;
u64 guild_rail_scene_next_wake(const GuildRailScene& scene, u64 now_ms) noexcept;
// Collapsed folder preview: 2x2 member icons with 4 DIP padding and 2 DIP gap.
inline UiRect guild_rail_folder_preview_rectangle(UiRect tile, u32 index) noexcept {
    const float extent = (ui_rect_width(tile) - 10.0f) * .5f;
    const float left = tile.left + 4.0f + static_cast<float>(index % 2) * (extent + 2.0f);
    const float top = tile.top + 4.0f + static_cast<float>(index / 2) * (extent + 2.0f);
    return {left, top, left + extent, top + extent};
}
// Icons stay square and centered while a row's extent animates.
inline UiRect guild_rail_icon_rectangle(UiRect row) noexcept {
    const float extent = minimum(ui_rect_width(row), ui_rect_height(row));
    const float x = (row.left + row.right - extent) * .5f, y = (row.top + row.bottom - extent) * .5f;
    return {x, y, x + extent, y + extent};
}

} // namespace maicord
