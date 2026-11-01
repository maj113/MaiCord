// What the scenes do with plugin surfaces, against a fixed slot query (the runtime's own verdicts are tested
// beside the plugin host): server list tiles at both ends of the rail, their hit testing and state, the count
// badge geometry, and the member list header label.
#include "maicord/ui/guild/rail_scene.hpp"
#include "maicord/ui/plugins/slot_surfaces.hpp"

namespace maicord {
namespace {

void (*g_check)(bool, const char*) noexcept;
void check(bool value, const char* name) noexcept { g_check(value, name); }

constexpr u64 kOwner = 0x5343454E45534554ull;
u32 g_queries;

UiPluginSlotNode button(u64 action, u16 badge, u8 state) noexcept {
    UiPluginSlotNode node{};
    node.owner_id = kOwner;
    node.action_id = action;
    node.text = {"Tile", 4};
    node.icon = static_cast<u16>(UiIcon::puzzle);
    node.kind = ui_plugin_node_button;
    node.badge = badge;
    node.state = state;
    return node;
}

u32 query(u32 surface, u64, u64, u64, u64, Slice<UiPluginSlotNode> out) noexcept {
    ++g_queries;
    UiPluginSlotNode nodes[3]{};
    u32 count = 0;
    switch (surface) {
    case kUiPluginSlotServerTop:
        nodes[count++] = button(1, 7, 0);
        nodes[count++] = button(2, 0, kUiPluginSlotActive);
        break;
    case kUiPluginSlotServerBottom: nodes[count++] = button(3, 0, kUiPluginSlotDisabled); break;
    case kUiPluginSlotMemberListHeader: {
        UiPluginSlotNode label{};
        label.owner_id = kOwner;
        label.text = {"12 online", 9};
        nodes[count++] = label;
        break;
    }
    default: return 0;
    }
    const u32 used = minimum(count, out.count);
    for (u32 i = 0; i < used; ++i) out[i] = nodes[i];
    return used;
}

} // namespace

void test_plugin_surface_scenes(void (*checker)(bool, const char*) noexcept) noexcept {
    g_check = checker;
    const auto saved_query = g_ui_plugin_slot_query;
    const u32 saved_surfaces = g_ui_plugin_slot_surfaces;
    g_ui_plugin_slot_query = nullptr;

    GuildViewRow guilds[3]{};
    for (u32 i = 0; i < 3; ++i) {
        guilds[i].id = 100 + i;
        guilds[i].generation = 1;
    }
    NavigationView view{};
    view.guild_rows = guilds;
    view.guild_capacity = view.guild_count = 3;
    view.selected_guild_id = 100;
    view.guild_revision = view.revision = 1;
    GuildRailScene rail{};
    GuildRailRowWidget rows[8]{};
    UiWidget widgets[3 + 8 + 2 * kGuildRailPluginTiles + 8]{};
    UiLayoutNode nodes[3 + 8 + 2 * kGuildRailPluginTiles + 8]{};
    const UiRect viewport{0, 0, 72, 900};
    check(guild_rail_scene_initialize(&rail, {rows, 8}, {widgets, sizeof(widgets) / sizeof(widgets[0])},
                                      {nodes, sizeof(nodes) / sizeof(nodes[0])}) &&
              guild_rail_scene_update(&rail, &view, viewport, 0).ok(),
          "the rail builds without plugins");
    const float plain_height = rail.content_height;
    check(rail.plugin_top == 0 && rail.plugin_bottom == 0 && rail.materialized_top == 0 &&
              rail.materialized_bottom == 0 && !g_queries,
          "no plugin, no tiles and no query");

    g_ui_plugin_slot_query = query;
    g_ui_plugin_slot_surfaces = (1u << kUiPluginSlotServerTop) | (1u << kUiPluginSlotServerBottom) |
                                (1u << kUiPluginSlotMemberListHeader);
    view.guild_revision = 2;
    check(guild_rail_scene_update(&rail, &view, viewport, 0).ok() && rail.plugin_top == 2 && rail.plugin_bottom == 1 &&
              rail.materialized_top == 2 && rail.materialized_bottom == 1,
          "the rail reads the tile counts from the plugin query");
    check(rail.content_height > plain_height + 2.0f * 48.0f && rail.content_height < plain_height + 3.0f * 72.0f,
          "three tiles make the list taller by about three server rows");

    // The tiles sit above the first server and below "Add a server".
    const UiRect top0 = guild_rail_scene_plugin_rectangle(rail, 1, 0), top1 = guild_rail_scene_plugin_rectangle(rail, 1, 1),
                 bottom0 = guild_rail_scene_plugin_rectangle(rail, 2, 0);
    const UiRect first_guild = ui_widget_manager_layout_node(rail.widgets, rail.rows[0].widget)->rectangle;
    const UiRect add = guild_rail_scene_add_rectangle(rail);
    check(ui_rect_has_area(top0) && ui_rect_has_area(top1) && ui_rect_has_area(bottom0) && top0.top < top1.top &&
              top1.bottom <= first_guild.top && bottom0.top >= add.bottom,
          "tiles sit above the first server and under the add button");

    const auto hit = [&](UiRect r) noexcept {
        return guild_rail_scene_hit_test(rail, (r.left + r.right) * .5f, (r.top + r.bottom) * .5f);
    };
    const GuildRailHit top_hit = hit(top1), bottom_hit = hit(bottom0), guild_hit = hit(first_guild);
    check(top_hit.plugin_end == 1 && top_hit.plugin_index == 1 && bottom_hit.plugin_end == 2 &&
              bottom_hit.plugin_index == 0 && guild_hit.plugin_end == 0 && guild_hit.id == 100,
          "a tile hit names its end and index; servers are unaffected");

    GuildRailPluginTile tile{};
    check(guild_rail_plugin_tile(rail, 1, 0, &tile) && tile.node.action_id == 1 && tile.node.badge == 7 &&
              guild_rail_plugin_tile(rail, 1, 1, &tile) && tile.node.state == kUiPluginSlotActive &&
              guild_rail_plugin_tile(rail, 2, 0, &tile) && tile.node.state == kUiPluginSlotDisabled &&
              !guild_rail_plugin_tile(rail, 2, 1, &tile),
          "tile nodes carry their action, count and state");

    // The count badge hangs off the tile's top right corner.
    const UiRect badge = ui_plugin_slot_count_rectangle(top0, 7);
    check(badge.right > top0.right && badge.top < top0.top && ui_rect_width(badge) >= kUiMentionBadgeHeight,
          "the count badge overhangs the tile's corner");

    UiPluginSlotNode header{};
    check(ui_plugin_member_list_header(&header) && header.text.bytes == 9 && kUiPluginMemberListHeaderHeight > 0,
          "the member list header is the first label node");

    // Removing the plugins removes the tiles at the next update.
    g_ui_plugin_slot_query = nullptr;
    view.guild_revision = 3;
    check(guild_rail_scene_update(&rail, &view, viewport, 0).ok() && rail.plugin_top == 0 && rail.plugin_bottom == 0 &&
              rail.content_height == plain_height && !ui_rect_has_area(guild_rail_scene_plugin_rectangle(rail, 1, 0)),
          "the tiles go with the plugins");
    g_ui_plugin_slot_query = saved_query;
    g_ui_plugin_slot_surfaces = saved_surfaces;
}

} // namespace maicord
