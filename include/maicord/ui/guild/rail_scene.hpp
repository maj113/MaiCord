#pragma once

#include "maicord/ui/shell/navigation_view.hpp"
#include "maicord/ui/shell/navigation_drag.hpp"
#include "maicord/ui/layout/surface_geometry.hpp"
#include "maicord/ui/core/widget_manager.hpp"
#include "maicord/ui/core/transition.hpp"

namespace maicord {

struct UiVirtualRowStorageOwner;

// Vertical gap between rail rows (drop lines sit in its middle).
inline constexpr float kGuildRailRowGapDips = kUiGuildRailGap;

// Guild indicator transition: 8/20/40px, 200ms CSS ease-out.
struct GuildRailIndicatorMotion {
    struct Scope { u64 owner, generation; } scope;
    u64 started_ms, sampled_ms;
    float value, from, target;
    bool active;
};
struct GuildRailIndicator {
    GuildRailIndicatorMotion height, visibility;
};
bool guild_rail_indicator_tick(GuildRailIndicator* pill, UiMotionScope scope,
                               bool selected, bool hovered, bool unread,
                               u64 now_ms, bool animate) noexcept;
u64 guild_rail_indicator_next_wake(const GuildRailIndicator& pill) noexcept;
// Rail tiles (server icons, text tiles, Home, Add a Server, folders) are
// rounded squares at every state; `radius` is the theme's tile radius for a
// full-size tile and scales with a tile that a folder transition shrinks.
inline float guild_rail_tile_radius(UiRect tile, float radius) noexcept {
    const float extent = minimum(ui_rect_width(tile), ui_rect_height(tile));
    return minimum(radius * extent / kUiGuildRailTileExtent, extent * .5f);
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

inline constexpr u32 kGuildRailFolderMotionSlots = 8;
// Plain fields keep GuildRailScene constant-initializable.
struct GuildRailFolderMotion {
    u64 folder_id;
    u64 started_ms, sampled_ms;
    float value, from, target;
    bool active;
};

// Plugin tiles (kUiPluginSlotServerTop above the first server, kUiPluginSlotServerBottom after "Add a server").
inline constexpr u32 kGuildRailPluginTiles = 4;

struct GuildRailHit {
    u64 id;
    UiWidgetHandle widget;
    u32 source_index;
    bool selectable;
    bool folder; // id is a folder header row id
    bool add_server; // the "Add a server" button after the last row (id is zero)
    bool conversation; // id is a pinned conversation's channel
    // A plugin tile: 1 above the servers, 2 after "Add a server" (0 none), and its index within that end.
    u8 plugin_end = 0, plugin_index = 0;
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
    // Plugin tiles: how many each end takes (read from the plugin query on every update, so the list's height
    // follows), how many are materialized, and their widgets (top first, then bottom).
    u8 plugin_top, plugin_bottom, materialized_top, materialized_bottom;
    UiWidgetHandle plugin_tiles[2 * kGuildRailPluginTiles];
};

// The add button's laid-out rectangle; empty while it is not materialized.
UiRect guild_rail_scene_add_rectangle(const GuildRailScene& scene) noexcept;
// A plugin tile's laid-out rectangle (end 1 top, 2 bottom); empty while it is not materialized.
UiRect guild_rail_scene_plugin_rectangle(const GuildRailScene& scene, u32 end, u32 index) noexcept;
bool guild_rail_scene_initialize(GuildRailScene* scene, Slice<GuildRailRowWidget> row_storage,
                                 Slice<UiWidget> widget_storage,
                                 Slice<UiLayoutNode> layout_storage) noexcept;
GuildRailSceneResult guild_rail_scene_update(GuildRailScene* scene, const NavigationView* view,
                                             const UiRect& viewport, float requested_scroll_dips) noexcept;
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
u64 guild_rail_scene_next_wake(const GuildRailScene& scene) noexcept;
// Collapsed folder preview: 2x2 member icons with 3 DIP padding and 2 DIP gap
// (16-DIP icons in a 40-DIP tile).
inline UiRect guild_rail_folder_preview_rectangle(const UiRect& tile, u32 index) noexcept {
    const float extent = (ui_rect_width(tile) - 8.0f) * .5f;
    const float left = tile.left + 3.0f + static_cast<float>(index % 2) * (extent + 2.0f);
    const float top = tile.top + 3.0f + static_cast<float>(index / 2) * (extent + 2.0f);
    return {left, top, left + extent, top + extent};
}
// Icons stay square and centered while a row's extent animates.
inline UiRect guild_rail_icon_rectangle(const UiRect& row) noexcept {
    const float extent = minimum(ui_rect_width(row), ui_rect_height(row));
    const float x = (row.left + row.right - extent) * .5f, y = (row.top + row.bottom - extent) * .5f;
    return {x, y, x + extent, y + extent};
}

} // namespace maicord
