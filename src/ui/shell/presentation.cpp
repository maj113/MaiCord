#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/widgets/badge.hpp"
#include "maicord/ui/shell/brand_mark.hpp"
#include "maicord/ui/chat/messages/mention_badge.hpp"
#include "maicord/ui/widgets/presence.hpp"
#include "maicord/ui/shell/presentation.hpp"
#include "maicord/ui/shell/notice_band.hpp"
#include "maicord/ui/layout/surface_geometry.hpp"
#include "maicord/ui/layout/scrollbar.hpp"
#include "maicord/ui/text/measure_cache.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/widgets/button.hpp"
#include "maicord/ui/theme/background_presentation.hpp"
#include "maicord/ui/theme/background_surface.hpp"
#include "maicord/ui/shell/navigation_foreground.hpp"
#include "maicord/ui/voice/channel_rows.hpp"
#include "maicord/ui/voice/panel.hpp"
#include "maicord/model/voice/channel_info_store.hpp"
#include "maicord/ui/widgets/icon.hpp"
#include "maicord/ui/plugins/slots.hpp"
#include "maicord/ui/plugins/slot_surfaces.hpp"
#include "maicord/ui/chat/private_channel_rows.hpp"
#include "maicord/ui/guild/banner.hpp"
#include "maicord/model/guild/member_list.hpp"
#include "maicord/ui/chat/composer/voice_bar.hpp"
#include "maicord/model/guild/member_store.hpp"
#include "maicord/ui/chat/messages/typing_indicator.hpp"
#include "maicord/ui/guild/member_list_scene.hpp"
#include "maicord/ui/chat/composer/message_composer_scene.hpp"
#include "maicord/ui/theme/styles.hpp"
#include "maicord/ui/widgets/activity_status.hpp"

namespace maicord {
namespace {
UiIcon channel_row_icon(const ChannelViewRow& row) noexcept {
    if (row.locked)
        return UiIcon::lock;
    switch (row.type) {
    case 2: return UiIcon::speaker;
    case 5: return UiIcon::megaphone;
    case 10:
    case 11:
    case 12: return UiIcon::thread;
    case 13: return UiIcon::podcast;
    case 15: return UiIcon::message_text;
    case 16: return UiIcon::image;
    case kNavigationEventsRowType: return UiIcon::calendar;
    case kNavigationRequestsRowType: return UiIcon::inbox;
    default: return UiIcon::channel;
    }
}

// Keep fractional DIPs here; the renderer maps them to physical pixels.
bool paint_guild_indicator(PaintStream* stream, float left, UiRect row, const GuildRailIndicator* motion,
                           bool selected, bool hovered, bool unread, Color color) noexcept {
    const bool sampled = motion && motion->height.scope.owner;
    const float height = sampled ? 8 + 32 * motion->height.value : selected ? 40 : hovered ? 20 : 8;
    const float visible = sampled ? motion->visibility.value : selected || hovered || unread ? 1.f : 0.f;
    if (visible <= 0)
        return true;
    const float center = (row.top + row.bottom) * .5f;
    const float right = left + 4 * visible;
    color.alpha *= visible;
    return paint_stream_push_rounded(stream, right - 8, center - height * .5f, right, center + height * .5f, color, 4);
}

bool paint_thread_spine(PaintStream* stream, const ShellThreadSpine& spine, Color color) noexcept {
    constexpr float w = kShellThreadSpineWidth, r = kShellThreadSpineRadius;
    // Quarter arc from the rail into the arm as four round-capped segments.
    static constexpr float cosines[]{1.0f, 0.9238795f, 0.7071068f, 0.3826834f, 0.0f};
    const UiPoint centre{spine.arc_end.x, spine.arc_start.y};
    UiPoint previous = spine.arc_start;
    for (u32 i = 1; i < 5; ++i) {
        const UiPoint next{centre.x - r * cosines[i], centre.y + r * cosines[4 - i]};
        if (!paint_stream_push_line(stream, previous, next, w, color))
            return false;
        previous = next;
    }
    return paint_stream_push_line(stream, spine.rail_top, spine.arc_start, w, color) &&
           paint_stream_push_line(stream, spine.arc_end, spine.arm_end, w, color) &&
           (spine.continue_bottom <= spine.arc_start.y ||
            paint_stream_push_line(stream, spine.arc_start, {spine.arc_start.x, spine.continue_bottom}, w, color));
}

// Drag-and-drop reorder: a 2-DIP accent drop line between rows, or an accent
// ring around the row a drop combines with.
bool paint_drag_visual(PaintStream* stream, const NavigationDragVisual& drag, const Theme& theme,
                       float ring_radius) noexcept {
    if (!drag.active || !ui_rect_has_area(drag.indicator))
        return true;
    const UiRect& r = drag.indicator;
    if (!drag.ring)
        return paint_stream_push_rounded(stream, r, theme.accent, kNavigationDragLine * .5f);
    constexpr float inset = 3.0f;
    Color clear = theme.accent;
    clear.alpha = 0;
    return paint_stream_push_rounded_border(stream, r.left - inset, r.top - inset,
                                            r.right + inset, r.bottom + inset, clear,
                                            theme.accent, ring_radius + inset, 2.0f);
}

float rail_tile_radius(const UiRect& tile, const Theme& theme) noexcept {
    return guild_rail_tile_radius(tile, theme.scale.radius_lg);
}

// The square `half` DIPs either side of the centre of `r`: where a glyph sits on its button.
UiRect centered_square(const UiRect& r, float half) noexcept {
    const float x = (r.left + r.right) * .5f, y = (r.top + r.bottom) * .5f;
    return {x - half, y - half, x + half, y + half};
}

// A rail tile without artwork (Home, Add a Server, a server's initials): the
// raised surface at rest, the accent while hovered or selected. Its surface
// follows the theme like the other raised controls.
bool paint_rail_tile(PaintStream* stream, const UiRect& tile, bool accent, const Theme& theme) noexcept {
    const bool background = ui_background_active(theme);
    Color fill = accent ? theme.tokens.emphasis.fill : theme.surface_high;
    if (background && !accent)
        fill.alpha = 1;
    return ui_background_paint_surface(stream, tile, theme, fill, fill, rail_tile_radius(tile, theme),
                                       background && !accent ? .75f : 0, accent ? 1.f : .80f,
                                       accent ? UiBackgroundSurfaceMode::tinted : UiBackgroundSurfaceMode::gradient,
                                       UiBackgroundTier::highest, UiBackgroundBorderTone::palette);
}
Color rail_tile_ink(bool accent, const Theme& theme) noexcept {
    return accent ? theme.tokens.emphasis.text : theme.text_primary;
}

// "Add a server": a rail tile with a 20-DIP plus.
bool paint_add_server(PaintStream* stream, const GuildRailScene& scene, const Theme& theme) noexcept {
    const UiRect row = guild_rail_scene_add_rectangle(scene);
    if (!ui_rect_has_area(row))
        return true;
    const UiRect tile = guild_rail_icon_rectangle(row);
    const bool hovered = ui_widget_handle_matches(scene.add_button, scene.widgets.hovered);
    return paint_rail_tile(stream, tile, hovered, theme) &&
           ui_icon_paint(stream, UiIcon::plus, centered_square(tile, kUiGuildRailGlyphExtent * .5f),
                         rail_tile_ink(hovered, theme));
}

// The Home tile: in the built-in flat themes its hovered or selected state is the brand gradient (the app icon's tile)
// under a white mark; in every other theme it is the rail's accent tile.
bool home_brand_tile(bool accent, const Theme& theme) noexcept {
    return accent && theme.brand_marks;
}
bool paint_home_tile(PaintStream* stream, const UiRect& tile, bool accent, const Theme& theme) noexcept {
    if (!home_brand_tile(accent, theme))
        return paint_rail_tile(stream, tile, accent, theme);
    return paint_stream_push_gradient(stream, tile, color_from_rgb24(kBrandMarkTop),
                                      color_from_rgb24(kBrandMarkBottom), rail_tile_radius(tile, theme));
}
Color home_mark_ink(bool accent, const Theme& theme) noexcept {
    return home_brand_tile(accent, theme) ? color_from_rgb24(0xFFFFFF) : rail_tile_ink(accent, theme);
}

// MaiCord's mark stroked across a rail tile, the Home button's glyph.
bool paint_brand_mark(PaintStream* stream, const UiRect& tile, Color color) noexcept {
    const float extent = ui_rect_width(tile);
    const auto point = [&](u32 i) noexcept {
        return UiPoint{tile.left + kBrandMarkPoints[i][0] * extent, tile.top + kBrandMarkPoints[i][1] * extent};
    };
    for (u32 i = 0; i < 4; ++i)
        if (!paint_stream_push_line(stream, point(i), point(i + 1), extent * kBrandMarkStroke, color))
            return false;
    return true;
}

// A count badge cut out of the rail like a server badge.
bool paint_rail_badge(PaintStream* stream, const UiRect& badge, const Theme& theme) noexcept {
    const float cutout = ui_background_active(theme) ? 0.0f : theme.scale.space_1;
    return paint_stream_push_rounded_border(stream, badge.left - cutout, badge.top - cutout, badge.right + cutout,
                                            badge.bottom + cutout, theme.notification, theme.base_lowest,
                                            ui_rect_height(badge) * 0.5f + cutout, cutout);
}

// Plugin tiles at both ends of the server list: rail tiles holding the plugin's icon; a toggle that is on (or
// a hover) takes the accent, a disabled one is dimmed, a count becomes a badge cut out of the rail.
bool paint_plugin_tiles(PaintStream* stream, const GuildRailScene& scene, const Theme& theme) noexcept {
    if (!scene.materialized_top && !scene.materialized_bottom)
        return true;
    for (u32 end = 1; end <= 2; ++end)
        for (u32 index = 0; index < kGuildRailPluginTiles; ++index) {
            GuildRailPluginTile tile;
            if (!guild_rail_plugin_tile(scene, end, index, &tile))
                continue;
            const bool hovered = ui_widget_handle_matches(tile.widget, scene.widgets.hovered);
            const bool disabled = (tile.node.state & kUiPluginSlotDisabled) != 0;
            const bool accent = !disabled && (hovered || (tile.node.state & kUiPluginSlotActive));
            if (!paint_rail_tile(stream, tile.tile, accent, theme) ||
                !ui_icon_paint(stream, ui_plugin_slot_icon(tile.node),
                               centered_square(tile.tile, kUiGuildRailGlyphExtent * .5f),
                               disabled ? ui_background_muted_text_color(theme)
                               : accent ? rail_tile_ink(true, theme)
                                        : ui_plugin_slot_ink(theme, tile.node.tone, theme.text_primary)) ||
                (tile.node.badge && !paint_rail_badge(stream, ui_mention_badge_guild_rectangle(tile.tile, tile.node.badge), theme)))
                return false;
        }
    return true;
}

bool paint_voice_participants(PaintStream* stream, const ShellNavigationRowWidget& row_widget,
                              UiRect row_rectangle, const VoiceStateStore& voice_states, UiRect viewport,
                              const Theme& theme) noexcept {
    UiVoiceParticipantRow user{};
    for (u32 i = 0; ui_voice_participant_row(row_widget.view, row_rectangle, voice_states, i, &user); ++i) {
        if (user.row.bottom <= viewport.top || user.row.top >= viewport.bottom)
            continue;
        // Speaking ring: a success-coloured disc 2 DIP larger than the avatar, drawn beneath it.
        if (user.speaking && !paint_stream_push_rounded(stream, ui_rect_shrink(user.avatar, -2),
                                                        ui_icon_role_color(theme, UiIconRole::success),
                                                        ui_rect_width(user.avatar) * 0.5f + 2))
            return false;
        if (!paint_stream_push_rounded(stream, user.avatar, theme.avatar_placeholder, ui_rect_width(user.avatar) * 0.5f))
            return false;
        const UiRect indicators[]{user.muted, user.deafened};
        const UiIcon icons[]{UiIcon::mic_off, UiIcon::headphones_off};
        for (u32 k = 0; k < 2; ++k)
            if (const auto& indicator = indicators[k];
                ui_rect_has_area(indicator) && !ui_icon_paint(stream, icons[k], indicator, theme.notification))
                return false;
        // Camera on; clicking the row opens the call view focused on this user.
        if (ui_rect_has_area(user.video) &&
            !ui_icon_paint(stream, UiIcon::video, user.video, ui_icon_role_color(theme, UiIconRole::muted)))
            return false;
        // Go Live badge; its label is emitted with the row text. Hovering the
        // row previews the stream, clicking the badge watches it.
        if (const auto& live = user.live; ui_rect_has_area(live) &&
            !paint_stream_push_rounded(stream, live, theme.tokens.accent.danger, theme.scale.radius_sm))
            return false;
    }
    return true;
}

// The pencil leading "Set a channel status", and the "Invite to Voice" row.
bool paint_voice_extras(PaintStream* stream, const ShellNavigationRowWidget& row_widget, UiRect row_rectangle,
                        const UiVoiceChannelExtras& extras, const Theme& theme) noexcept {
    const auto& row = row_widget.view;
    const auto* info = extras.info ? voice_channel_info_store_find(*extras.info, row.id) : nullptr;
    if (row.voice_subtitle && !(info && info->status_bytes)) {
        const UiRect line = ui_voice_status_rectangle(row, row_rectangle);
        const float middle = (line.top + line.bottom) * 0.5f;
        const UiRect pencil{line.left, middle - 7.0f, line.left + 14.0f, middle + 7.0f};
        const bool hovered = extras.status_hovered && extras.status_channel_id == row.id;
        if (!ui_icon_paint(stream, UiIcon::edit, pencil,
                           hovered ? theme.text_primary : ui_icon_role_color(theme, UiIconRole::muted)))
            return false;
    }
    UiVoiceInviteRow invite{};
    if (!ui_voice_invite_row(row, row_rectangle, &invite))
        return true;
    const auto& r = invite.row;
    if (extras.invite_hovered &&
        !paint_stream_push_rounded(stream, r, ui_background_hover_color(theme, theme.surface_high, .20f), theme.scale.radius_sm))
        return false;
    const UiRect& disc = invite.icon;
    if (!paint_stream_push_rounded(stream, disc, theme.surface_high, ui_rect_width(disc) * 0.5f) ||
        !ui_icon_paint(stream, UiIcon::user_plus, centered_square(disc, 7.0f),
                       extras.invite_hovered ? theme.text_primary : theme.text_secondary))
        return false;
    return !extras.invite_hovered ||
           ui_icon_paint(stream, UiIcon::close, centered_square(invite.close, 8.0f),
                         extras.invite_close_hovered ? theme.text_primary : ui_icon_role_color(theme, UiIconRole::muted));
}

Color folder_color(const Theme& theme, const GuildViewRow& row, float alpha) noexcept {
    Color color = row.folder_color == kGuildFolderNoColor
                      ? theme.tokens.emphasis.outline
                      : color_from_rgb24(row.folder_color);
    color.alpha *= alpha;
    return color;
}

// Expanded (or animating) folders sit on one tinted, outlined plate spanning
// the header and its visible members, 4 DIPs outside their tiles.
bool paint_folder_groups(PaintStream* stream, const GuildRailScene& scene, const Theme& theme) noexcept {
    for (u32 i = 0; i < scene.count;) {
        const auto& first = scene.rows[i];
        const u64 folder = first.view.folder_id;
        u32 end = i + 1;
        bool members = first.view.folder_role == GuildFolderRole::member;
        while (folder && end < scene.count && scene.rows[end].view.folder_id == folder) {
            members = members || scene.rows[end].view.folder_role == GuildFolderRole::member;
            ++end;
        }
        if (folder && members) {
            const auto* top = guild_rail_scene_layout_node(scene, first);
            const auto* bottom = guild_rail_scene_layout_node(scene, scene.rows[end - 1]);
            if (!top || !bottom)
                return false;
            const UiRect plate{top->rectangle.left - 4, top->rectangle.top - 4, top->rectangle.right + 4,
                               bottom->rectangle.bottom + 4};
            if (!paint_stream_push_rounded_border(stream, plate.left, plate.top, plate.right, plate.bottom,
                                                  folder_color(theme, first.view, .15f), theme.tokens.border.subtle,
                                                  theme.scale.radius_xl, 1))
                return false;
        }
        i = end;
    }
    return true;
}

bool paint_folder_header(PaintStream* stream, const NavigationView* navigation,
                         const GuildRailRowWidget& row_widget, UiRect rectangle, bool hovered,
                         const Theme& theme) noexcept {
    const auto& row = row_widget.view;
    if (row.folder_expanded)
        return (!hovered || paint_stream_push_rounded(stream, rectangle, theme.tokens.state.hover,
                                                      rail_tile_radius(rectangle, theme))) &&
               ui_icon_paint(stream, UiIcon::folder, centered_square(rectangle, kUiNavigationIconExtent * .5f),
                             folder_color(theme, row, 1));
    if (!paint_stream_push_rounded(stream, rectangle, folder_color(theme, row, .4f), rail_tile_radius(rectangle, theme)))
        return false;
    // Members without artwork get a neutral tile; icons are queued as media.
    for (u32 k = 0; navigation && k < 4 && k < row.folder_size; ++k) {
        const u32 source = row_widget.source_index + 1 + k;
        if (source >= navigation->guild_count || navigation->guild_rows[source].has_icon)
            continue;
        const UiRect mini = guild_rail_folder_preview_rectangle(rectangle, k);
        if (!paint_stream_push_rounded(stream, mini, theme.surface_high, theme.scale.radius_sm))
            return false;
    }
    return true;
}

// The server header: the dropdown's fill while hovered or open, the chevron
// after the measured name (an X while open), then Invite People.
bool paint_guild_header(PaintStream* stream, const UiScene& scene, const NavigationView* navigation,
                        ShellTextMeasure measure, const Color& hover, const Color& pressed, const Color& ink, const Color& quiet) noexcept {
    const auto* menu = ui_scene_node(scene, UiSemantic::guild_menu);
    if (!menu || !navigation || !navigation->selected_guild_id)
        return true;
    const u64 guild_id = navigation->selected_guild_id;
    const auto* guild = measure.state ? chat_state_store_find_guild(*measure.state, guild_id) : nullptr;
    const Utf8View name = guild ? chat_state_store_guild_name(*measure.state, *guild) : Utf8View{};
    const UiRect r = menu->rectangle;
    const auto geometry = shell_guild_header_geometry(
        r, shell_text_width(measure.cache, guild_id, kShellMeasureGuildName, name, text_style_none,
                            shell_guild_header_name_space(r)));
    const bool hovered = ui_widget_handle_equal(scene.widgets.hovered, ui_scene_widget(scene, UiSemantic::guild_menu));
    if (((hovered || scene.guild_menu_expanded) &&
         !paint_stream_push_rounded(stream, geometry.hover, scene.guild_menu_expanded ? pressed : hover,
                                    kUiShellGuildMenuRadius)) ||
        !ui_icon_paint(stream, scene.guild_menu_expanded ? UiIcon::close : UiIcon::chevron_down, geometry.chevron,
                       ink))
        return false;
    const auto* invite = ui_scene_node(scene, UiSemantic::guild_invite);
    if (!invite)
        return true;
    const bool invite_hovered =
        ui_widget_handle_equal(scene.widgets.hovered, ui_scene_widget(scene, UiSemantic::guild_invite));
    const UiRect b = invite->rectangle;
    return (!invite_hovered || paint_stream_push_rounded(stream, b, hover, kUiShellGuildMenuRadius)) &&
           ui_icon_paint(stream, UiIcon::user_plus, centered_square(b, kUiShellGuildInviteGlyph * .5f),
                         invite_hovered ? ink : quiet);
}

} // namespace

float shell_text_width(UiTextMeasureCache* cache, u64 source_id, u16 item, const Utf8View& text, u32 style_flags,
                       float maximum_width) noexcept {
    const float limit = maximum(0.f, maximum_width);
    if (!text.bytes)
        return 0.f;
    if (!cache || limit <= 0)
        return limit;
    const TextStyleSpan style{0, text.bytes, style_flags};
    UiTextMeasureValue measured{};
    const UiTextMeasureRequest request{{source_id, hash64_bytes(kHash64Seed, text.data, text.bytes), 0, item, 0},
                                       text,
                                       style_flags ? ConstSlice<TextStyleSpan>{&style, 1} : ConstSlice<TextStyleSpan>{},
                                       {limit, 40.f},
                                       true};
    return ui_text_measure_cache_measure(cache, request, &measured).ok() ? minimum(limit, measured.size.width)
                                                                          : limit;
}

float shell_guild_header_name_space(UiRect menu) noexcept {
    return maximum(0.f, ui_rect_width(menu) - 16.f - kUiShellGuildMenuChevron - 4.f);
}

ShellGuildHeaderGeometry shell_guild_header_geometry(const UiRect& menu, float name_width) noexcept {
    const float left = menu.left + 8.f;
    const float width = minimum(maximum(0.f, name_width), shell_guild_header_name_space(menu));
    const float chevron = left + width + 4.f;
    const float top = (menu.top + menu.bottom - kUiShellGuildMenuChevron) * .5f;
    return {{left, menu.top, left + width, menu.bottom},
            {chevron, top, chevron + kUiShellGuildMenuChevron, top + kUiShellGuildMenuChevron},
            {menu.left, menu.top, minimum(menu.right, chevron + kUiShellGuildMenuChevron + 8.f), menu.bottom}};
}

float shell_category_name_space(const UiRect& row, const ChannelViewRow& view) noexcept {
    const float left = row.left + kShellCategoryNameLeft + static_cast<float>(view.depth) * kShellChannelIndent;
    const float right = shell_navigation_controls_left(row, view, view.controls & channel_row_create,
                                                       row.right - kShellChannelHighlightLeft);
    return maximum(0.f, right - left - kShellCategoryGap - kShellCategoryChevron);
}

ShellCategoryGeometry shell_category_geometry(const UiRect& row, const ChannelViewRow& view, float name_width) noexcept {
    const float left = row.left + kShellCategoryNameLeft + static_cast<float>(view.depth) * kShellChannelIndent;
    const float width = minimum(maximum(0.f, name_width), shell_category_name_space(row, view));
    const float top = row.top + kShellCategoryLineTop, bottom = row.top + kShellCategoryHeight;
    const float chevron = left + width + kShellCategoryGap, middle = (top + bottom) * .5f;
    return {{left, top, left + width, bottom},
            {chevron, middle - kShellCategoryChevron * .5f, chevron + kShellCategoryChevron,
             middle + kShellCategoryChevron * .5f}};
}

bool shell_presentation_paint_reply_mention(const MessageComposerScene& scene, const Theme& theme,
                                            PaintStream* stream) noexcept {
    // The reply mention switch: no box or hover fill, only the @ icon and the
    // label tinted (link colour on, muted off), then a subtle divider.
    const auto* mention = message_composer_scene_layout_node(scene, MessageComposerControl::reply_mention);
    const auto icon = message_composer_scene_reply_mention_icon(scene);
    if (!stream || !mention || !ui_rect_has_area(icon))
        return false;
    const float divider = mention->rectangle.right + kUiComposerPadding;
    const float middle = (icon.top + icon.bottom) * .5f;
    return ui_icon_paint(stream, UiIcon::mention, icon,
                         scene.mention_reply_author ? theme.tokens.text.link : theme.tokens.text.muted) &&
           paint_stream_push(stream, divider, middle - 10, divider + 1, middle + 10, ui_separator_color(theme));
}

// The typing label's dots sit on the label's line (kUiTypingLabelDrop below the row's middle).
static UiRect typing_dots_box(const UiLayoutNode& typing) noexcept {
    return ui_typing_dots_row({typing.rectangle.left, typing.rectangle.top + kUiTypingLabelDrop,
                               typing.rectangle.right, typing.rectangle.bottom + kUiTypingLabelDrop});
}

bool shell_presentation_build_chrome(const UiScene& scene, const NavigationView* navigation,
                                     const GuildRailScene* guild_scene,
                                     const ShellNavigationScene* channel_scene,
                                     const MessageComposerScene* composer_scene,
                                     const MemberListScene* member_scene, const Theme& theme,
                                     float viewport_width, float viewport_height, PaintStream* stream,
                                     ShellPresentationRegions* regions,
                                     const MessageComposerPresentationInput* composer_presentation,
                                     u64 visual_selected_channel_id, ShellTextMeasure measure) noexcept {
    if (!stream || !regions || !ui_extent_valid(viewport_width) || !ui_extent_valid(viewport_height) ||
        viewport_width <= 0.0f || viewport_height <= 0.0f) {
        return false;
    }
    const UiLayoutNode* root = ui_scene_node(scene, UiSemantic::root);
    const UiLayoutNode* guild = ui_scene_node(scene, UiSemantic::guild_rail);
    const UiLayoutNode* guild_navigation = ui_scene_node(scene, UiSemantic::guild_navigation);
    const UiLayoutNode* home = ui_scene_node(scene, UiSemantic::home_button);
    const UiLayoutNode* channels = ui_scene_node(scene, UiSemantic::channel_sidebar);
    const UiLayoutNode* channel_header = ui_scene_node(scene, UiSemantic::channel_header);
    const UiLayoutNode* channel_navigation = ui_scene_node(scene, UiSemantic::channel_navigation);
    const UiLayoutNode* channel_footer = ui_scene_node(scene, UiSemantic::channel_footer);
    const UiLayoutNode* title = ui_scene_node(scene, UiSemantic::title_bar);
    const UiLayoutNode* member_toggle = ui_scene_node(scene, UiSemantic::member_toggle);
    const UiLayoutNode* content = ui_scene_node(scene, UiSemantic::content);
    const UiLayoutNode* messages = ui_scene_node(scene, UiSemantic::message_view);
    const UiLayoutNode* composer = ui_scene_node(scene, UiSemantic::composer);
    const UiLayoutNode* members = ui_scene_node(scene, UiSemantic::member_sidebar);
    if (!root || !guild || !guild_navigation || !home || !title || !messages) {
        return false;
    }

    if (!paint_stream_reset(stream, viewport_width, viewport_height)) {
        return false;
    }
    *regions = ShellPresentationRegions{messages->rectangle, members ? members->rectangle : UiRect{},
                                        members != nullptr};
    const bool background = ui_background_active(theme);
    if (!ui_background_paint_canvas(stream, root->rectangle, theme, theme.base_low))
        return false;
    if (background && stream->count) {
        stream->quads[0].border_width = -3;
        // One contrast plate covers the whole message column, including typing,
        // composer margins and the bottom inset. The message clip stays unchanged.
        UiRect canvas = messages->rectangle;
        canvas.bottom = content ? content->rectangle.bottom : root->rectangle.bottom;
        if (!ui_background_paint_canvas(stream, canvas, theme, theme.base_low))
            return false;
    }
    if (!ui_background_paint_canvas(stream, guild->rectangle, theme, theme.base_lowest,
                                    UiBackgroundTier::app_frame)) {
        return false;
    }
    // The app-wide notice band over the root's top inset.
    if (scene.notice &&
        !ui_notice_band_paint(stream, *scene.notice,
                              ui_notice_band_geometry(*scene.notice, root->rectangle, measure.cache), theme))
        return false;

    // Home: the MaiCord mark on a rail tile that takes the accent (the brand gradient in the built-in flat themes)
    // while hovered or selected, then the separator one gap below it.
    const bool home_hovered = ui_widget_handle_equal(scene.widgets.hovered, ui_scene_widget(scene, UiSemantic::home_button));
    const bool home_selected = navigation && !navigation->selected_guild_id;
    const bool home_accent = home_selected || home_hovered;
    if (!paint_guild_indicator(stream, guild->rectangle.left, home->rectangle,
                               guild_scene ? &guild_scene->home_indicator : nullptr, home_selected,
                               home_hovered, false, theme.text_primary) ||
        !paint_home_tile(stream, home->rectangle, home_accent, theme) ||
        !paint_brand_mark(stream, home->rectangle, home_mark_ink(home_accent, theme)))
        return false;
    const float separator_center = (home->rectangle.left + home->rectangle.right) * .5f;
    const float separator_top = home->rectangle.bottom + kUiGuildRailGap;
    if (!paint_stream_push_rounded(stream, separator_center - kUiGuildRailSeparatorWidth * .5f, separator_top,
                                   separator_center + kUiGuildRailSeparatorWidth * .5f,
                                   separator_top + kUiGuildRailSeparatorHeight, ui_separator_color(theme),
                                   theme.scale.radius_xs * .5f))
        return false;
    // Friend and message requests badge Home, cut out of the rail like a server badge.
    if (navigation && navigation->home_badge &&
        !paint_rail_badge(stream, ui_mention_badge_guild_rectangle(home->rectangle, navigation->home_badge), theme))
        return false;

    if (guild_scene && guild_scene->active) {
        if (!paint_stream_push_clip(stream, guild_navigation->rectangle)) {
            return false;
        }
        if (!paint_folder_groups(stream, *guild_scene, theme))
            return false;
        const GuildRailRowWidget* hovered_row = guild_rail_scene_resolve(*guild_scene, guild_scene->widgets.hovered);
        for (u32 index = 0; index < guild_scene->count; ++index) {
            const GuildRailRowWidget& row_widget = guild_scene->rows[index];
            const GuildViewRow& row = row_widget.view;
            const UiLayoutNode* layout = guild_rail_scene_layout_node(*guild_scene, row_widget);
            if (!layout) {
                return false;
            }
            const bool header = row.folder_role == GuildFolderRole::header;
            const UiRect rectangle = guild_rail_icon_rectangle(layout->rectangle);
            const bool row_hovered = hovered_row == &row_widget;
            if (rectangle.bottom <= guild->rectangle.top || rectangle.top >= guild->rectangle.bottom) {
                continue;
            }
            // An expanded header hands selection and unread state to its members.
            const bool collapsed_header = header && !row.folder_expanded;
            if (!paint_guild_indicator(stream, guild->rectangle.left, rectangle, &row_widget.indicator,
                                       !row.unavailable && row.selected && (!header || collapsed_header),
                                       !row.unavailable && row_hovered,
                                       !row.unavailable && row.unread && (!header || collapsed_header),
                                       theme.text_primary)) {
                return false;
            }
            if (header) {
                if (!paint_folder_header(stream, navigation, row_widget, rectangle, row_hovered, theme))
                    return false;
            }
            // A pinned conversation sits on avatar placeholders like its
            // conversation row; the avatars are media above.
            if (row.folder_role == GuildFolderRole::conversation) {
                if (!ui_private_avatar_placeholder_paint(stream, row.avatar_shape, rectangle, theme))
                    return false;
                continue;
            }
            // The fallback tile belongs to guilds without artwork; it takes
            // the accent while hovered or selected. Painting it below an icon
            // would replace the rail backdrop in transparent pixels.
            const bool accent = !row.unavailable && (row_hovered || row.selected);
            if (!header && !row.has_icon &&
                !paint_stream_push_rounded(stream, rectangle, accent ? theme.tokens.emphasis.fill
                : row.unavailable ? theme.interactive
                // A light rail needs the raised white tile to separate the acronym.
                : theme.appearance == ThemeAppearance::light ? theme.surface
                                                             : theme.surface_high, rail_tile_radius(rectangle, theme))) {
                return false;
            }
            // Mention and plugin badges sit above the icon's media:
            // shell_presentation_build_guild_rail_overlay.
        }
        if (!paint_add_server(stream, *guild_scene, theme) || !paint_plugin_tiles(stream, *guild_scene, theme) ||
            !paint_drag_visual(stream, guild_scene->drag, theme, theme.scale.radius_lg))
            return false;
        if (!paint_stream_pop_clip(stream)) {
            return false;
        }
    }

    if (channels && channel_header && channel_navigation && channel_footer) {
        // The channel column ends with its list; the account panel floats over
        // the app frame below both columns, whose rail part the rail canvas paints.
        UiRect sidebar = channels->rectangle;
        sidebar.bottom = maximum(sidebar.top, minimum(sidebar.bottom, channel_navigation->rectangle.bottom));
        const UiRect frame{sidebar.left, sidebar.bottom, sidebar.right, channels->rectangle.bottom};
        if (!ui_background_paint_canvas(stream, sidebar, theme, theme.base_lower, UiBackgroundTier::high) ||
            (ui_rect_has_area(frame) &&
             !ui_background_paint_canvas(stream, frame, theme, theme.base_lowest, UiBackgroundTier::app_frame)) ||
            !ui_background_paint_canvas(stream, channel_header->rectangle, theme, theme.base_low,
                                        UiBackgroundTier::app_frame) ||
            !ui_background_paint_surface(stream, channel_footer->rectangle, theme, theme.surface_high,
                                         theme.interactive, ui_theme_metrics(theme.styles).edit_radius, 1,
                                         1, UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest,
                                         UiBackgroundBorderTone::palette)) {
            return false;
        }
        // The server banner's container (its picture is media above): the
        // active interactive fill shows until the picture is resident.
        if (channel_scene && channel_scene->banner_inset > 0) {
            const auto banner = ui_guild_banner_geometry(channels->rectangle, channel_header->rectangle,
                                                         channel_scene->scroll_dips, channel_scene->banner_header_hovered);
            const UiRect clip = ui_rect_intersection(banner.clip, channels->rectangle);
            Color placeholder = theme.tokens.state.pressed;
            placeholder.alpha *= banner.opacity;
            if (banner.opacity > 0 && ui_rect_has_area(clip) &&
                !paint_stream_push(stream, clip, placeholder))
                return false;
        }
        if (navigation && channel_scene && channel_scene->active) {
            if (!paint_stream_push_clip(stream, channel_navigation->rectangle)) {
                return false;
            }
            const ShellNavigationRowWidget* hovered_row =
                shell_navigation_scene_resolve(*channel_scene, channel_scene->widgets.hovered);
            for (u32 index = 0; index < channel_scene->count; ++index) {
                const ShellNavigationRowWidget& row_widget = channel_scene->rows[index];
                const ChannelViewRow& row = row_widget.view;
                const bool row_selected =
                    visual_selected_channel_id ? row.id == visual_selected_channel_id : row.selected;
                const UiLayoutNode* layout = shell_navigation_scene_layout_node(*channel_scene, row_widget);
                if (!layout) {
                    return false;
                }
                const UiLayoutNode* icon_layout =
                    ui_widget_manager_layout_node(channel_scene->widgets, row_widget.icon);
                const UiLayoutNode* label_layout =
                    ui_widget_manager_layout_node(channel_scene->widgets, row_widget.label);
                const UiLayoutNode* badge_layout =
                    row_widget.mention_badge.generation != 0
                        ? ui_widget_manager_layout_node(channel_scene->widgets, row_widget.mention_badge)
                        : nullptr;
                if (!icon_layout || !label_layout || (row.mention_count != 0 && !badge_layout)) {
                    return false;
                }
                const UiRect& row_rectangle = layout->rectangle;
                const bool row_hovered = hovered_row == &row_widget;
                const float row_top = row_rectangle.top;
                const bool private_row = channel_is_private(row.type);
                const bool category = row.type == 4;
                // Voice rows extend below their head (and status line) for
                // users and the invite row; the leading events row ends with
                // its divider band.
                const float row_height = row.voice_count || row.stage_live || row.voice_subtitle || row.voice_invite
                                             ? shell_navigation_voice_top(row)
                                         : row.type == kNavigationEventsRowType ? shell_navigation_head_height()
                                                                                : ui_rect_height(row_rectangle);
                const float selected_right = row_rectangle.right - kShellChannelHighlightRight;
                const bool thread = shell_navigation_thread_row(row) && !private_row;
                const auto sibling = [&](u32 at) noexcept {
                    return at < channel_scene->count && shell_navigation_thread_row(channel_scene->rows[at].view) &&
                           channel_scene->rows[at].view.parent_id == row.parent_id;
                };
                const ShellThreadSpine spine =
                    thread ? shell_navigation_thread_spine(row_rectangle, row_height, icon_layout->rectangle,
                                                           index && !sibling(index - 1), sibling(index + 1))
                           : ShellThreadSpine{};
                // A channel's 28-DIP highlight starts 6 DIPs in (indented with
                // its depth); thread highlights start where the spine's arm ends.
                const float selected_left = minimum(
                    selected_right - 1.0f,
                    thread        ? spine.arm_end.x + 2.0f
                    : private_row ? maximum(row_rectangle.left + 8.0f, icon_layout->rectangle.left - 8.0f)
                                  : row_rectangle.left + kShellChannelHighlightLeft +
                                        static_cast<float>(row.depth) * kShellChannelIndent);
                const UiRect selected_rectangle{selected_left, row_top + 1.0f, selected_right,
                                                row_top + row_height - 1.0f};
                if (row_rectangle.bottom > channel_navigation->rectangle.top &&
                    row_rectangle.top < channel_navigation->rectangle.bottom) {
                    // Categories change only their ink on hover. A guild channel's
                    // highlight takes the row tokens; a gradient keeps its glass.
                    const Color highlight =
                        private_row || ui_background_active(theme)
                            ? ui_background_hover_color(theme, row_selected ? theme.interactive : theme.surface_high,
                                                        row_selected ? .28f : .20f)
                        : row_selected ? theme.tokens.state.row_selected
                                       : theme.tokens.state.row_hover;
                    if ((row_selected || (row_hovered && !category)) &&
                        !paint_stream_push_rounded(stream, selected_rectangle, highlight,
                                                   private_row ? theme.scale.radius_sm : theme.scale.radius_md)) {
                        return false;
                    }
                    const UiRect& icon_rectangle = icon_layout->rectangle;
                    const Color row_color = ui_navigation_foreground(theme, row, row_selected);
                    // Unread: a 4 x 8 pill against the list's edge, rounded on its right.
                    const float middle = row_top + row_height * .5f;
                    const UiRect unread_rectangle =
                        private_row ? UiRect{row_rectangle.left - 3.0f, row_top + 10.0f, row_rectangle.left + 3.0f,
                                             row_top + row_height - 10.0f}
                                    : UiRect{row_rectangle.left - 4.0f, middle - 4.0f, row_rectangle.left + 4.0f,
                                             middle + 4.0f};
                    if (row.unread && !row_selected &&
                        !paint_stream_push_rounded(stream, unread_rectangle, theme.text_primary,
                                                   private_row ? theme.scale.radius_full : theme.scale.radius_sm)) {
                        return false;
                    }
                    if (private_row) {
                        const u8 shape = ui_private_avatar_shape(
                            navigation && navigation->private_rows ? navigation->private_rows->state : nullptr,
                            row.id, row.type);
                        if (!ui_private_avatar_placeholder_paint(stream, shape, icon_rectangle, theme))
                            return false;
                    } else if (category) {
                        // The chevron follows the measured name; Create Channel ends the line.
                        const auto* record = measure.state ? chat_state_store_find_channel(*measure.state, row.id)
                                                           : nullptr;
                        const Utf8View name =
                            record ? chat_state_store_channel_name(*measure.state, *record) : Utf8View{};
                        const auto geometry = shell_category_geometry(
                            row_rectangle, row,
                            shell_text_width(measure.cache, row.id, kShellMeasureCategoryName, name,
                                             kTextStyleSizeCodeSm, shell_category_name_space(row_rectangle, row)));
                        const Color ink = row_hovered ? theme.text_primary : row_color;
                        const UiRect create = shell_navigation_control_rectangle(row_rectangle, row, channel_row_create);
                        // A toggled category's chevron turns (100 ms linear, the host's chevron_turn): it draws
                        // pointing down, turned toward right; other rows keep their pixel-snapped resting icons.
                        const bool turning = channel_scene->chevron_category == row.id;
                        if (!(turning ? ui_icon_paint_turned(stream, UiIcon::chevron_down, geometry.chevron, ink,
                                                             -kUiQuarterTurn * channel_scene->chevron_turn)
                                      : ui_icon_paint(stream, row.collapsed ? UiIcon::chevron_right
                                                                            : UiIcon::chevron_down,
                                                      geometry.chevron, ink)) ||
                            (ui_rect_has_area(create) && !ui_icon_paint(stream, UiIcon::plus, create, ink)))
                            return false;
                    } else if (thread ? !paint_thread_spine(stream, spine, ui_background_line_color(theme))
                                      : !ui_icon_paint(stream, channel_row_icon(row), icon_rectangle,
                                                       row.stage_live ? ui_icon_role_color(theme, UiIconRole::success)
                                                                      : row_color)) {
                        return false;
                    }
                    // Create Invite and Edit Channel on a hovered or selected
                    // channel: strong on the selected row, dimmer on a hovered one.
                    const u8 controls = category ? 0 : shell_navigation_row_controls(row, row_hovered, row_selected);
                    Color control_ink = row_selected ? theme.text_primary : theme.text_secondary;
                    if (!row_selected)
                        control_ink.alpha *= .8f;
                    if (((controls & channel_row_invite) &&
                         !ui_icon_paint(stream, UiIcon::user_plus,
                                        shell_navigation_control_rectangle(row_rectangle, row, channel_row_invite),
                                        control_ink)) ||
                        ((controls & channel_row_settings) &&
                         !ui_icon_paint(stream, UiIcon::settings,
                                        shell_navigation_control_rectangle(row_rectangle, row, channel_row_settings),
                                        control_ink)))
                        return false;
                    // The leading rows' divider closes the events row's band.
                    if (row.type == kNavigationEventsRowType && index + 1 < channel_scene->count) {
                        const float line = row_rectangle.bottom - 1.0f;
                        if (!paint_stream_push(stream, row_rectangle.left + kShellCategoryNameLeft, line,
                                               row_rectangle.right - 3.0f, line + 1.0f, ui_separator_color(theme)))
                            return false;
                    }
                    if (row.mention_count != 0 &&
                        !paint_stream_push_rounded(stream, badge_layout->rectangle, theme.notification,
                                                   theme.scale.radius_md)) {
                        return false;
                    }
                    UiPluginSlotBadge plugin{};
                    if (shell_navigation_plugin_badge(*channel_scene, index, &plugin) &&
                        !ui_plugin_slot_badge_paint(plugin, channel_scene->widgets, theme, stream))
                        return false;
                    if (row.voice_count && navigation->voice_states &&
                        !paint_voice_participants(stream, row_widget, row_rectangle, *navigation->voice_states,
                                                  channel_navigation->rectangle, theme))
                        return false;
                    if (navigation->voice_extras &&
                        !paint_voice_extras(stream, row_widget, row_rectangle, *navigation->voice_extras, theme))
                        return false;
                    // The dragged row sits back under a veil of the list colour.
                    if (channel_scene->drag.active && channel_scene->drag.source_id == row.id) {
                        Color veil = theme.base_lower;
                        veil.alpha = .6f;
                        if (!paint_stream_push_rounded(stream, row_rectangle.left, row_top, row_rectangle.right,
                                          row_top + row_height, veil, theme.scale.radius_sm))
                            return false;
                    }
                }
            }
            if (!paint_drag_visual(stream, channel_scene->drag, theme, 4.0f))
                return false;
            if (!paint_stream_pop_clip(stream)) {
                return false;
            }
            if (!ui_scrollbar_paint(stream,
                                    ui_scrollbar_geometry(channel_scene->viewport,
                                                          channel_scene->scroll_dips,
                                                          channel_scene->content_height),
                                    theme)) {
                return false;
            }
        }
    }

    if (!ui_background_paint_canvas(stream, title->rectangle, theme, theme.base_lower,
                                    theme.appearance == ThemeAppearance::light ? UiBackgroundTier::lowest
                                                                               : UiBackgroundTier::lower) ||
        (members && !ui_background_paint_canvas(stream, members->rectangle, theme, theme.base_lower))) {
        return false;
    }
    if (const UiLayoutNode* title_label = ui_scene_node(scene, UiSemantic::title_label)) {
        UiRect line = title_label->rectangle;
        line.top = title->rectangle.top;
        line.bottom = title->rectangle.bottom;
        UiPluginSlotBadge header{};
        if (ui_plugin_slot_line_badge(kUiPluginSlotHeaderToolbar, 0, 0, 0, ui_scene_widget(scene, UiSemantic::title_label), line, nullptr, &header) &&
            !ui_plugin_slot_badge_paint(header, scene.widgets, theme, stream))
            return false;
    }
    if (member_toggle) {
        // A one-to-one DM's toggle shows the recipient's profile.
        if (!ui_button_icon_paint(stream, scene.widgets, ui_scene_widget(scene, UiSemantic::member_toggle),
                                  scene.side_panel_profile ? UiIcon::user_circle : UiIcon::members, theme,
                                  members != nullptr, false, UiButtonSurface::palette)) {
            return false;
        }
    }
    // Toolbar icon buttons, each its own semantic node.
    static constexpr struct {
        UiSemantic semantic;
        UiIcon icon;
        UiButtonSurface surface;
    } toolbar_buttons[]{{UiSemantic::pins_toggle, UiIcon::pin, UiButtonSurface::palette},
                        {UiSemantic::inbox_toggle, UiIcon::inbox, UiButtonSurface::palette},
                        {UiSemantic::settings, UiIcon::settings, UiButtonSurface::opaque},
                        {UiSemantic::private_add, UiIcon::user_plus, UiButtonSurface::palette},
                        {UiSemantic::private_call, UiIcon::phone, UiButtonSurface::palette}};
    for (const auto& item : toolbar_buttons) {
        if (const auto* button = ui_scene_node(scene, item.semantic);
            button && !ui_button_icon_paint(stream, scene.widgets, ui_scene_widget(scene, item.semantic), item.icon, theme, false, false,
                                            item.surface))
            return false;
    }
    if (const auto* search = ui_scene_node(scene, UiSemantic::private_search)) {
        const bool hovered = ui_widget_handle_equal(scene.widgets.hovered, ui_scene_widget(scene, UiSemantic::private_search));
        if (!paint_stream_push_rounded(stream, search->rectangle,
                                       hovered ? ui_background_hover_color(theme, theme.surface_high, .36f)
                                               : ui_background_surface_color(theme, theme.base_lowest, .9f),
                                       theme.scale.radius_sm))
            return false;
    }
    if (const auto* create = ui_scene_node(scene, UiSemantic::private_create)) {
        const bool hovered = ui_widget_handle_equal(scene.widgets.hovered, ui_scene_widget(scene, UiSemantic::private_create));
        if (!ui_icon_paint(stream, UiIcon::plus, ui_rect_shrink(create->rectangle, 4),
                           hovered ? theme.text_primary : theme.text_secondary))
            return false;
    }
    // Server header: the dropdown's hover (and open) fill, the chevron after
    // the name (an X while the popout is open), then Invite People.
    if (!shell_presentation_guild_banner_visible(scene, channel_scene) &&
        !paint_guild_header(stream, scene, navigation, measure,
                            ui_background_hover_color(theme, theme.surface_high, .20f),
                            ui_background_hover_color(theme, theme.surface_high, .28f), theme.text_primary,
                            theme.text_secondary))
        return false;
    if (channel_footer && navigation && navigation->voice_panel) {
        const auto& voice = *navigation->voice_panel;
        const UiRect footer = channel_footer->rectangle;
        if (!ui_voice_panel_paint(stream, voice, footer, theme) ||
            !ui_voice_account_controls_paint(stream, voice, footer, theme))
            return false;
        // Plugin buttons left of the microphone.
        UiPluginSlotNode plugin_nodes[kUiPluginSlotAccountButtons];
        const u32 plugin_count = ui_plugin_slot_account_buttons({plugin_nodes, kUiPluginSlotAccountButtons});
        for (u32 i = 0; i < plugin_count; ++i) {
            const UiRect r = ui_plugin_slot_account_button(footer, i);
            const bool hovered = g_ui_plugin_slot_hover.active &&
                                 g_ui_plugin_slot_hover.surface == kUiPluginSlotAccountPanel &&
                                 g_ui_plugin_slot_hover.index == i;
            const UiPluginSlotNode& node = plugin_nodes[i];
            const bool disabled = (node.state & kUiPluginSlotDisabled) != 0;
            const bool on = !disabled && (node.state & kUiPluginSlotActive) != 0;
            if (((hovered && !disabled) || on) &&
                !paint_stream_push_rounded(stream, r,
                                           on ? ui_background_hover_color(theme, theme.interactive, .28f)
                                              : ui_background_hover_color(theme, theme.surface_high, .20f),
                                           theme.scale.radius_sm))
                return false;
            if (!ui_icon_paint(stream, ui_plugin_slot_icon(node), ui_rect_shrink(r, 7),
                               disabled ? ui_background_muted_text_color(theme)
                                        : ui_plugin_slot_ink(theme, node.tone, theme.text_secondary)) ||
                (node.badge && !ui_plugin_slot_count_paint(stream, r, node.badge, theme)))
                return false;
        }
    }
    if (const auto* friends = ui_scene_node(scene, UiSemantic::friends_button)) {
        const bool hovered = ui_widget_handle_equal(scene.widgets.hovered, ui_scene_widget(scene, UiSemantic::friends_button));
        // The Message Requests page shares the view; its own sidebar row is the selected one then.
        const bool selected = scene.view == UiShellView::friends && !(navigation && navigation->requests_mode);
        if (!paint_stream_push_rounded(stream, friends->rectangle, selected
            ? ui_background_hover_color(theme, theme.interactive, .28f)
        : hovered ? ui_background_hover_color(theme, theme.surface_high, .20f)
                  : ui_background_surface_color(theme, theme.base_lower, .12f), theme.scale.radius_sm))
            return false;
        // The people glyph leads the label, as on the official client's Friends row.
        const UiRect content = ui_scene_content_rectangle(scene, UiSemantic::friends_button);
        const float middle = (content.top + content.bottom) * .5f;
        if (!ui_icon_paint(stream, UiIcon::members, {content.left, middle - 10.f, content.left + 20.f, middle + 10.f},
                           selected || hovered ? theme.text_primary : theme.text_secondary))
            return false;
    }
    if (members && member_scene && member_scene->active) {
        if (!paint_stream_push_clip(stream, members->rectangle)) {
            return false;
        }
        for (u32 index = 0; index < member_scene->count; ++index) {
            const MemberListRowWidget& row = member_scene->rows[index];
            const UiLayoutNode* row_layout = member_list_scene_layout_node(*member_scene, row.widget);
            if (!row_layout) {
                paint_stream_pop_clip(stream);
                return false;
            }

            const bool resident = member_list_scene_row_resident(*member_scene, index);
            if (resident && row.kind == MemberListRowKind::group) {
                continue;
            }
            const UiLayoutNode* avatar_layout = member_list_scene_layout_node(*member_scene, row.avatar);
            if (!avatar_layout ||
                !paint_stream_push_rounded(stream, avatar_layout->rectangle, theme.avatar_placeholder, theme.scale.radius_xl)) {
                paint_stream_pop_clip(stream);
                return false;
            }
            if (!resident) {
                const auto* label = member_list_scene_layout_node(*member_scene, row.label);
                if (!label ||
                    !paint_stream_push_rounded(
                        stream, label->content_rectangle.left, label->content_rectangle.top + 9.0f,
                        minimum(label->content_rectangle.right, label->content_rectangle.left + 92.0f),
                        label->content_rectangle.top + 17.0f, theme.avatar_placeholder, theme.scale.radius_sm)) {
                    paint_stream_pop_clip(stream);
                    return false;
                }
            }
        }
        if (!paint_stream_pop_clip(stream)) {
            return false;
        }
        if (!ui_scrollbar_paint(stream,
                                ui_scrollbar_geometry(member_scene->viewport, member_scene->scroll_dips,
                                                      member_scene->content_height),
                                theme)) {
            return false;
        }
    }
    const ChannelViewRow* selected_row =
        navigation ? navigation_view_find_channel(*navigation, navigation->selected_channel_id)
                   : nullptr;
    const bool composer_enabled =
        selected_row && (selected_row->actions & channel_action_send_messages) != 0;
    if (!composer)
        return true;
    const UiLayoutNode* composer_surface =
        composer_scene && composer_scene->active
            ? ui_widget_manager_layout_node(composer_scene->widgets, composer_scene->surface)
            : nullptr;
    if (composer_scene && composer_scene->active && !composer_surface) {
        return false;
    }
    const UiRect composer_rectangle = composer_surface ? composer_surface->rectangle : composer->rectangle;
    // One rounded input surface: focus lifts the outer border instead of
    // nesting a second outlined field inside.
    const bool composer_focused =
        composer_scene && composer_scene->active && composer_scene->model && composer_scene->model->focused;
    if (!ui_background_paint_surface(
            stream, composer_rectangle, theme, composer_enabled ? theme.surface_high : theme.base_low,
            !composer_enabled ? theme.base_lower : composer_focused ? theme.interactive_hover : theme.interactive,
            ui_theme_metrics(theme.styles).edit_radius, 1, 1, UiBackgroundSurfaceMode::gradient,
            UiBackgroundTier::highest, UiBackgroundBorderTone::palette)) {
        return false;
    }
    if (!composer_scene || !composer_scene->active) {
        return true;
    }
    const UiLayoutNode* attachment =
        message_composer_scene_layout_node(*composer_scene, MessageComposerControl::add_attachment);
    const UiLayoutNode* reply =
        ui_widget_manager_layout_node(composer_scene->widgets, composer_scene->reply);
    const UiLayoutNode* edit =
        message_composer_scene_layout_node(*composer_scene, MessageComposerControl::edit);
    const UiLayoutNode* emoji =
        message_composer_scene_layout_node(*composer_scene, MessageComposerControl::emoji);
    const UiLayoutNode* send =
        message_composer_scene_layout_node(*composer_scene, MessageComposerControl::send);
    const UiLayoutNode* typing =
        message_composer_scene_layout_node(*composer_scene, MessageComposerControl::typing_indicator);
    // Voice messages: the microphone replaces Send on an empty draft; while a
    // recording exists Add deletes it and Emoji stops or plays it.
    const UiComposerVoiceMode voice_mode = message_composer_scene_voice_mode(*composer_scene);
    const bool voice_bar = ui_composer_voice_bar_visible(voice_mode) && composer_scene->voice;
    const UiIcon voice_secondary = voice_mode == UiComposerVoiceMode::recording ? UiIcon::stop
                                   : voice_mode == UiComposerVoiceMode::sending ? UiIcon::sending
                                   : composer_scene->voice && composer_scene->voice->playing ? UiIcon::pause
                                                                                            : UiIcon::play;
    const UiIcon send_icon = voice_mode == UiComposerVoiceMode::available ? UiIcon::mic
                             : voice_mode == UiComposerVoiceMode::sending ? UiIcon::sending
                                                                          : UiIcon::send;
    const bool send_ready = voice_mode == UiComposerVoiceMode::none ? message_composer_scene_can_send(*composer_scene)
                            : voice_mode == UiComposerVoiceMode::available
                                ? composer_enabled
                                : voice_mode != UiComposerVoiceMode::sending;
    if (!attachment || !edit || !emoji || !send || !typing ||
        !paint_stream_push_rounded_border(stream, edit->rectangle, background         ? Color{}
        : composer_enabled ? theme.surface_high
                           : theme.base_low, Color{}, kUiControlCornerRadius, 0.0f) ||
        (voice_bar && !ui_composer_voice_bar_paint(*composer_scene->voice, edit->content_rectangle, theme, stream)) ||
        (composer_scene->command.avatar.kind == MediaAssetKind::none &&
         !ui_button_icon_paint(stream, composer_scene->widgets, composer_scene->add_attachment,
                              composer_scene->command.name_bytes ? UiIcon::command :
                              voice_bar ? UiIcon::trash : UiIcon::attachment, theme)) ||
        (message_composer_scene_control_visible(*composer_scene, MessageComposerControl::gif) &&
         !ui_button_icon_paint(stream, composer_scene->widgets, composer_scene->gif, UiIcon::gif, theme)) ||
        (message_composer_scene_control_visible(*composer_scene, MessageComposerControl::sticker) &&
         !ui_button_icon_paint(stream, composer_scene->widgets, composer_scene->sticker, UiIcon::sticker,
                               theme)) ||
        !ui_button_icon_paint(stream, composer_scene->widgets, composer_scene->emoji,
                              voice_bar ? voice_secondary : UiIcon::emoji, theme) ||
        !ui_button_icon_paint(stream, composer_scene->widgets, composer_scene->send, send_icon, theme,
                              send_ready, false, UiButtonSurface::palette)) {
        return false;
    }
    UiPluginSlotNode plugin_nodes[kMessageComposerPluginSlots];
    const u32 plugin_count = minimum<u32>(
        composer_scene->plugin_count,
        ui_plugin_slot_toolbar(kUiPluginSlotComposerToolbar, {plugin_nodes, kMessageComposerPluginSlots}));
    for (u32 i = 0; i < plugin_count; ++i) {
        const UiPluginSlotNode& node = plugin_nodes[i];
        const UiLayoutNode* cell = ui_widget_manager_layout_node(composer_scene->widgets, composer_scene->plugin_slots[i]);
        if (node.state & kUiPluginSlotDisabled) {
            // A disabled button keeps its place, dimmed, and ignores the pointer.
            if (!cell || !ui_icon_paint(stream, ui_plugin_slot_icon(node), ui_rect_shrink(cell->rectangle, 7),
                                        ui_background_muted_text_color(theme)))
                return false;
        } else if (!ui_button_icon_paint(stream, composer_scene->widgets, composer_scene->plugin_slots[i],
                                         ui_plugin_slot_icon(node), theme, (node.state & kUiPluginSlotActive) != 0)) {
            return false;
        }
        if (node.badge && cell && !ui_plugin_slot_count_paint(stream, cell->rectangle, node.badge, theme))
            return false;
    }
    if (message_composer_scene_attachment_row(*composer_scene) == message_composer_attachment_row_strip) {
        if (!ui_composer_attachment_strip_paint_base(message_composer_scene_attachment_view(*composer_scene, true),
                                                     theme, stream))
            return false;
    } else if (message_composer_scene_attachment_label(*composer_scene).bytes) {
        const auto* attachments =
            message_composer_scene_layout_node(*composer_scene, MessageComposerControl::attachments);
        if (!attachments ||
            !ui_background_paint_surface(stream, attachments->rectangle, theme,
                                         ui_button_background(composer_scene->widgets,
                                                              composer_scene->attachments, theme,
                                                              theme.base_lower),
                                         theme.interactive, theme.scale.radius_sm, 0, 1, UiBackgroundSurfaceMode::gradient,
                                         UiBackgroundTier::highest, UiBackgroundBorderTone::palette))
            return false;
    }
    if (composer_scene->reply_message_id != 0) {
        if (!reply ||
            !ui_background_paint_surface(stream, reply->rectangle, theme, theme.base_lower,
                                         theme.interactive, ui_theme_metrics(theme.styles).compact_radius,
                                         0, 1, UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest,
                                         UiBackgroundBorderTone::palette)) {
            return false;
        }
        const auto* cancel =
            message_composer_scene_layout_node(*composer_scene, MessageComposerControl::cancel_reply);
        if (!cancel ||
            !ui_button_icon_paint(stream, composer_scene->widgets, composer_scene->reply_cancel,
                                  UiIcon::close, theme, false, true, UiButtonSurface::palette))
            return false;
        if (composer_scene->reply_mention_visible &&
            !shell_presentation_paint_reply_mention(*composer_scene, theme, stream))
            return false;
    }
    // The option bar, stacked over the editor like the reply bar,
    // with its close button.
    if (message_composer_scene_command_bar_name(*composer_scene).bytes) {
        const auto* bar = ui_widget_manager_layout_node(composer_scene->widgets, composer_scene->command_bar);
        if (!bar ||
            !ui_background_paint_surface(stream, bar->rectangle, theme, theme.base_lower, theme.interactive,
                                         ui_theme_metrics(theme.styles).compact_radius, 0, 1,
                                         UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest,
                                         UiBackgroundBorderTone::palette) ||
            !ui_button_icon_paint(stream, composer_scene->widgets, composer_scene->command_close, UiIcon::circle_x,
                                  theme, false, true, UiButtonSurface::palette))
            return false;
    }
    // Typing dots animate in the composer overlay; a notice keeps resting ones.
    if (message_composer_scene_typing_label(*composer_scene).bytes != 0 && !composer_scene->typing_dots &&
        !ui_typing_dots_paint(stream, typing_dots_box(*typing),
                              composer_scene->status_is_error || composer_scene->status_tone == 4 ? theme.notification
                              : composer_scene->status_tone == 3 ? theme.tokens.text.warning
                              : composer_scene->status_tone == 2 ? theme.tokens.text.positive
                              : composer_scene->status_tone == 1 ? theme.accent
                                                                 : theme.text_primary,
                              kUiTypingDotsRest))
        return false;
    // The selected command's option chips (1 DIP
    // border): the value segment frames the chip and the name segment sits on
    // its leading end. The active chip's border is the focus colour, a chip
    // that failed its check the danger colour.
    if (composer_presentation && composer_presentation->geometry_active && composer_presentation->chips.count) {
        // A chip is taller than its line (text_pill_extent): its clip reaches into the editor's padding for the overshoot.
        const float overshoot = ui_theme_metrics(theme.styles).composer_padding;
        const UiRect chip_clip{edit->content_rectangle.left, edit->content_rectangle.top - overshoot,
                               edit->content_rectangle.right, edit->content_rectangle.bottom + overshoot};
        if (!paint_stream_push_clip(stream, chip_clip))
            return false;
        const float radius = theme.scale.radius_sm;
        for (u32 i = 0; i < composer_presentation->chips.count; ++i) {
            const auto& chip = composer_presentation->chips[i];
            const bool key = ui_rect_has_area(chip.key), value = ui_rect_has_area(chip.value);
            const UiRect box = !key     ? chip.value
                               : !value ? chip.key
                                        : UiRect{minimum(chip.key.left, chip.value.left),
                                                 minimum(chip.key.top, chip.value.top),
                                                 maximum(chip.key.right, chip.value.right),
                                                 maximum(chip.key.bottom, chip.value.bottom)};
            const Color border = chip.flags & message_composer_chip_error    ? theme.tokens.text.danger
                                 : chip.flags & message_composer_chip_active ? theme.tokens.border.focus
                                                                             : theme.tokens.option.border;
            if (!paint_stream_push_rounded_border(stream, box, theme.tokens.option.fill, border, radius, 1))
                return false;
            if (!key)
                continue;
            const UiRect name{chip.key.left + 1, chip.key.top + 1, value ? chip.key.right : chip.key.right - 1,
                              chip.key.bottom - 1};
            if (!paint_stream_push_rounded(stream, name, theme.tokens.option.key, maximum(0.f, radius - 1)) ||
                (value && !paint_stream_push(stream, minimum(name.right, name.left + radius), name.top, name.right,
                                             name.bottom, theme.tokens.option.key)))
                return false;
        }
        if (!paint_stream_pop_clip(stream))
            return false;
    }
    if (composer_presentation && composer_presentation->geometry_active && composer_scene->model->focused) {
        for (u32 i = 0; i < composer_presentation->composition.count; ++i) {
            auto mark = composer_presentation->composition[i];
            mark.attributes.line_style = TextCompositionLineStyle::none;
            if (!text_composition_paint_mark(stream, mark, edit->content_rectangle, theme.text_primary))
                return false;
        }
    }
    return true;
}

UiRect caret_pixel_rectangle(const UiRect& caret, const UiRect& clip, u32 dpi) noexcept {
    if (!ui_rect_valid_or_empty(caret) || !ui_rect_has_area(clip))
        return {};
    const auto floor_pixel = [](float value) noexcept {
        const auto integer = static_cast<i32>(value);
        return static_cast<float>(integer - (value < integer ? 1 : 0));
    };
    const float scale = maximum(dpi, 1u) / 96.0f;
    const float left_limit = -floor_pixel(-clip.left * scale);
    const float right_limit = floor_pixel(clip.right * scale) - 1;
    if (right_limit < left_limit)
        return {};
    const float x = maximum(left_limit, minimum(right_limit, floor_pixel(caret.left * scale + .5f)));
    const float top = floor_pixel(maximum(caret.top, clip.top) * scale + .5f);
    const float bottom = floor_pixel(minimum(caret.bottom, clip.bottom) * scale + .5f);
    return bottom > top ? UiRect{x / scale, top / scale, (x + 1) / scale, bottom / scale} : UiRect{};
}
// The fade behind the typing strip, painted while the list is scrolled away: the official client's chat
// gradient. Alone it is `kUiTypingFadeIdle` DIPs up to the list's bottom; while someone types it is
// `kUiTypingFadeTyping` DIPs up to the strip's top, followed by a solid band of the background down to the
// list's bottom, so no message shows through behind the label.
static bool paint_typing_fade(const MessageComposerScene& scene, const ShellTypingFade& fade, const Theme& theme,
                              PaintStream* paint) noexcept {
    const UiRect& list = fade.list;
    if (!fade.scrolled_away || !ui_rect_has_area(list) || ui_background_active(theme))
        return true;
    const auto* typing = message_composer_scene_layout_node(scene, MessageComposerControl::typing_indicator);
    const bool typing_now = typing && scene.typing_dots && message_composer_scene_typing_label(scene).bytes != 0;
    const float right = maximum(list.left, list.right - kUiTypingFadeRightInset);
    const float strip_top = typing_now ? minimum(list.bottom, typing->rectangle.top) : list.bottom;
    const UiRect ramp{list.left, maximum(list.top, strip_top - (typing_now ? kUiTypingFadeTyping : kUiTypingFadeIdle)),
                      right, strip_top};
    const Color solid = theme.base_low;
    Color clear = solid;
    clear.alpha = 0;
    return (!ui_rect_has_area(ramp) || paint_stream_push_gradient(paint, ramp, clear, solid)) &&
           (strip_top >= list.bottom || paint_stream_push(paint, {list.left, strip_top, right, list.bottom}, solid));
}
bool shell_presentation_build_composer_overlay(const MessageComposerScene* scene,
                                               const MessageComposerPresentationInput& input,
                                               const Theme& theme, PaintStream* paint, u32 dpi,
                                               UiTypingDotsPaint* dots, const ShellTypingFade* fade) noexcept {
    if (dots)
        reset_value(dots);
    // The fade goes first: the dots (and the label's text beneath this layer) stay clear of it.
    if (scene && scene->active && fade && !paint_typing_fade(*scene, *fade, theme, paint))
        return false;
    // Typing dots: always three quads, so a step patches only their geometry
    // (renderer_step_typing_dots).
    if (scene && scene->active && scene->typing_dots && message_composer_scene_typing_label(*scene).bytes) {
        const auto* typing = message_composer_scene_layout_node(*scene, MessageComposerControl::typing_indicator);
        const UiRect row = typing ? typing_dots_box(*typing) : UiRect{};
        const u32 first = paint ? paint->count : 0;
        if (typing && (!paint_stream_push_clip(paint, {typing->rectangle.left, typing->rectangle.top,
                                                       typing->rectangle.right,
                                                       typing->rectangle.bottom + kUiTypingLabelDrop}) ||
                       !ui_typing_dots_paint(paint, row, theme.text_primary, scene->typing_phase)))
            return false;
        if (typing && dots) {
            const PaintClip& clip = paint->clips[paint->clip_stack[paint->clip_depth - 1]];
            *dots = {row, {clip.left, clip.top, clip.right, clip.bottom}, theme.text_primary, first,
                     paint->count - first, scene->typing_phase};
        }
        if (typing && !paint_stream_pop_clip(paint))
            return false;
    }
    // Spelling marks stay visible without focus; the caret needs it.
    const bool focused = scene && scene->model && scene->model->focused;
    if (!scene || !scene->active || !scene->model || !input.geometry_active || (!focused && !input.composition.count))
        return true;
    const auto* edit = message_composer_scene_layout_node(*scene, MessageComposerControl::edit);
    if (!edit || !paint_stream_push_clip(paint, edit->content_rectangle))
        return false;
    bool ok = true;
    for (u32 i = 0; ok && i < input.composition.count; ++i) {
        const auto& attributes = input.composition[i].attributes;
        // Uncoloured error marks (misspellings) use the danger token.
        const Color fallback =
            attributes.attribute == TextCompositionAttribute::input_error ? theme.tokens.accent.danger : theme.text_primary;
        ok = text_composition_paint_line(
            paint, ui_rect_intersection(input.composition[i].rectangle, edit->content_rectangle), attributes, fallback);
    }
    if (ok && focused) {
        // A hidden caret keeps a transparent quad, so a blink changes one
        // quad's colour rather than the batch structure and the renderer can
        // limit its damage to the caret.
        const auto r = caret_pixel_rectangle(input.caret, edit->content_rectangle, dpi);
        Color color = theme.text_primary;
        if (!input.caret_visible)
            color.alpha = 0;
        if (ui_rect_has_area(r))
            ok = paint_stream_push(paint, r, color);
    }
    return paint_stream_pop_clip(paint) && ok;
}

bool shell_presentation_guild_banner_visible(const UiScene& scene, const ShellNavigationScene* channel_scene) noexcept {
    const UiLayoutNode* channels = ui_scene_node(scene, UiSemantic::channel_sidebar);
    const UiLayoutNode* header = ui_scene_node(scene, UiSemantic::channel_header);
    return channel_scene && channel_scene->banner_inset > 0 && channels && header &&
           ui_guild_banner_geometry(channels->rectangle, header->rectangle, channel_scene->scroll_dips, false).visible;
}

bool shell_presentation_build_guild_banner_overlay(const UiScene& scene, const NavigationView* navigation,
                                                   const ShellNavigationScene* channel_scene, const Theme& theme,
                                                   PaintStream* stream, ShellTextMeasure measure) noexcept {
    if (!stream)
        return false;
    const UiLayoutNode* channels = ui_scene_node(scene, UiSemantic::channel_sidebar);
    const UiLayoutNode* header = ui_scene_node(scene, UiSemantic::channel_header);
    if (!channel_scene || channel_scene->banner_inset <= 0 || !channels || !header)
        return true;
    const auto banner = ui_guild_banner_geometry(channels->rectangle, header->rectangle, channel_scene->scroll_dips,
                                                 channel_scene->banner_header_hovered);
    if (!banner.visible)
        return true;
    const UiRect bar = banner.header;
    // Header shade: a blurred 52% black ellipse behind the header
    // text, fading with the banner; a top-down scrim of the same strength.
    Color shade = theme.tokens.overlay.scrim_light;
    shade.alpha *= banner.scrim;
    Color clear = shade;
    clear.alpha = 0;
    if (shade.alpha > 0 &&
        !paint_stream_push_gradient(stream, {bar.left, bar.top, bar.right, bar.bottom + 4}, shade, clear))
        return false;
    // Header glass: its 1 DIP lower edge comes in with the glass.
    Color edge = ui_separator_color(theme);
    edge.alpha *= banner.glass;
    if (edge.alpha > 0 && !paint_stream_push(stream, bar.left, bar.bottom, bar.right, bar.bottom + 1, edge))
        return false;
    // The header over the banner: the same dropdown and invite in white.
    return paint_guild_header(stream, scene, navigation, measure, theme.tokens.state.hover,
                              theme.tokens.state.pressed, theme.tokens.text.on_accent, theme.tokens.text.on_accent);
}

bool shell_presentation_build_member_status_overlay(const MemberListScene* member_scene, const Theme& theme,
                                                    PaintStream* stream) noexcept {
    if (!stream || !member_scene) {
        return member_scene == nullptr && stream != nullptr;
    }
    if (!member_scene->active) {
        return true;
    }
    if (!ui_rect_has_area(member_scene->viewport) ||
        !paint_stream_push_clip(stream, member_scene->viewport)) {
        return false;
    }
    const MemberListRowWidget* hovered_row = member_list_scene_resolve(*member_scene, member_scene->widgets.hovered);
    for (u32 index = 0; index < member_scene->count; ++index) {
        const MemberListRowWidget& row = member_scene->rows[index];
        if (!member_list_scene_row_resident(*member_scene, index) ||
            row.kind != MemberListRowKind::member) {
            continue;
        }
        const UiLayoutNode* status = member_list_scene_layout_node(*member_scene, row.status);
        if (!status) {
            paint_stream_pop_clip(stream);
            return false;
        }
        if (hovered_row == &row) {
            const auto* node = member_list_scene_layout_node(*member_scene, row.widget);
            if (node && !paint_stream_push_rounded(stream, node->rectangle.left + 4,
                                                   node->rectangle.top + 2, node->rectangle.right - 4,
                                                   node->rectangle.bottom - 2, theme.tokens.state.hover,
                                                   theme.scale.radius_md))
                return false;
        }
        const auto tag = member_list_badge(*member_scene, index);
        if (tag.owner.generation && !ui_badge_paint(tag, member_scene->widgets, theme, stream))
            return false;
        if (!ui_plugin_slot_badge_paint(member_list_plugin_badge(*member_scene, index), member_scene->widgets, theme,
                                        stream))
            return false;
        const GuildMemberRecord* member = member_scene->members && row.source_row.member_slot < member_scene->members->capacity
                                              ? &member_scene->members->records[row.source_row.member_slot]
                                              : nullptr;
        // Rows without a server record (private conversations) carry their resolved status.
        const auto presence = static_cast<UiPresence>(member ? member->status : row.source_row.status);
        const bool mobile =
            member_scene->users &&
            ui_presence_mobile(presence, user_store_client_status(*member_scene->users, row.user_id));
        if (!ui_presence_paint(stream, status->rectangle, presence, theme.base_lower, theme, mobile)) {
            paint_stream_pop_clip(stream);
            return false;
        }
        // The activity glyph leads the status line (shell_text_presentation).
        UiActivityStatus activity{};
        const UiLayoutNode* label = member_list_scene_layout_node(*member_scene, row.label);
        if (label && member_list_row_status(*member_scene, index, {}, &activity) &&
            activity.glyph != UiActivityGlyph::none &&
            !ui_icon_paint(stream, ui_activity_glyph_icon(activity.glyph),
                           ui_activity_glyph_rectangle(member_list_row_status_line(label->content_rectangle)),
                           theme.tokens.status.online)) {
            paint_stream_pop_clip(stream);
            return false;
        }
    }
    return paint_stream_pop_clip(stream);
}

bool shell_presentation_build_guild_rail_overlay(const UiScene& scene, const GuildRailScene* guild_scene,
                                                 const Theme& theme, PaintStream* stream) noexcept {
    if (!stream)
        return false;
    const UiLayoutNode* guild = ui_scene_node(scene, UiSemantic::guild_rail);
    const UiLayoutNode* navigation = ui_scene_node(scene, UiSemantic::guild_navigation);
    if (!guild_scene || !guild_scene->active || !guild || !navigation)
        return true;
    if (!paint_stream_push_clip(stream, navigation->rectangle))
        return false;
    // A rail-coloured ring drawn over the icon's media separates the lower
    // badge from the icon.
    for (u32 index = 0; index < guild_scene->count; ++index) {
        const GuildRailRowWidget& row_widget = guild_scene->rows[index];
        const GuildViewRow& row = row_widget.view;
        const UiLayoutNode* layout = guild_rail_scene_layout_node(*guild_scene, row_widget);
        if (!layout) {
            paint_stream_pop_clip(stream);
            return false;
        }
        const UiRect rectangle = guild_rail_icon_rectangle(layout->rectangle);
        if (rectangle.bottom <= guild->rectangle.top || rectangle.top >= guild->rectangle.bottom ||
            (row.folder_role == GuildFolderRole::header && row.folder_expanded))
            continue;
        if (row.mention_count != 0 &&
            !paint_rail_badge(stream, ui_mention_badge_guild_rectangle(rectangle, row.mention_count), theme)) {
            paint_stream_pop_clip(stream);
            return false;
        }
        // Plugin badge on the opposite (top-right) corner.
        UiPluginSlotCorner plugin{};
        if (guild_rail_plugin_corner(*guild_scene, index, &plugin) &&
            !ui_plugin_slot_corner_paint(plugin, theme.base_lowest, theme, stream)) {
            paint_stream_pop_clip(stream);
            return false;
        }
    }
    return paint_stream_pop_clip(stream);
}

} // namespace maicord
