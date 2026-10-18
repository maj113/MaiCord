#include "maicord/ui/theme_style_projection.hpp"
#include "maicord/ui/badge.hpp"
#include "maicord/ui/mention_badge.hpp"
#include "maicord/ui/presence.hpp"
#include "maicord/ui/shell_presentation.hpp"
#include "maicord/ui/surface_geometry.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/navigation_foreground.hpp"
#include "maicord/ui/voice_channel_rows.hpp"
#include "maicord/ui/voice_panel.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/plugin_slots.hpp"
#include "maicord/ui/plugin_slot_surfaces.hpp"
#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/ui/guild_banner.hpp"
#include "maicord/model/member_list.hpp"
#include "maicord/ui/composer_voice_bar.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/ui/member_list_scene.hpp"
#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/theme_styles.hpp"

namespace maicord {
namespace {
UiIcon channel_row_icon(const ChannelViewRow& row) noexcept {
    switch (row.type) {
    case 2: return UiIcon::speaker;
    case 4: return row.collapsed ? UiIcon::chevron_right : UiIcon::chevron_down;
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

float pixel_truncated(float value) noexcept {
    // Geometry stays in fractional DIPs. Truncating to an integer DIP is not
    // pixel snapping at 125%/150% DPI and lets paint drift from the exact text
    // and hit-test layout. The renderer transform maps these coordinates to the
    // physical pixel grid.
    return value;
}

bool push_rounded(PaintStream* stream, float left, float top, float right, float bottom, const Color& color,
                  float radius) noexcept {
    return paint_stream_push_rounded(stream, pixel_truncated(left), pixel_truncated(top),
                                     pixel_truncated(right), pixel_truncated(bottom), color, radius);
}

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
    return push_rounded(stream, right - 8, center - height * .5f, right, center + height * .5f, color, 4);
}

bool paint_thread_spine(PaintStream* stream, const ShellThreadSpine& spine, Color color) noexcept {
    constexpr float w = kShellThreadSpineWidth, r = kShellThreadSpineRadius;
    // Quarter arc from the rail into the arm as four round-capped segments.
    constexpr float cosines[]{1.0f, 0.9238795f, 0.7071068f, 0.3826834f, 0.0f};
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

bool record_content(PaintStream* stream, const UiWidgetManager& manager, UiWidgetHandle owner,
                    const UiRect& rectangle, UiLayoutContentKind kind,
                    u16 flags = ui_layout_content_bounds_collision) noexcept {
    return paint_stream_record_content(stream, UiWidgetReference{&manager, owner}, rectangle, kind, flags);
}

// Drag-and-drop reorder: a 2-DIP accent drop line between rows, or an accent
// ring around the row a drop combines with.
bool paint_drag_visual(PaintStream* stream, const NavigationDragVisual& drag, const Theme& theme,
                       float ring_radius) noexcept {
    if (!drag.active || !ui_rect_has_area(drag.indicator))
        return true;
    const UiRect& r = drag.indicator;
    if (!drag.ring)
        return push_rounded(stream, r.left, r.top, r.right, r.bottom, theme.accent, kNavigationDragLine * .5f);
    constexpr float inset = 3.0f;
    Color clear = theme.accent;
    clear.alpha = 0;
    return paint_stream_push_rounded_border(stream, pixel_truncated(r.left - inset), pixel_truncated(r.top - inset),
                                            pixel_truncated(r.right + inset), pixel_truncated(r.bottom + inset), clear,
                                            theme.accent, ring_radius + inset, 2.0f);
}

// "Add a server": a round tile with a green plus that fills green on hover.
bool paint_add_server(PaintStream* stream, const GuildRailScene& scene, const Theme& theme) noexcept {
    const UiRect row = guild_rail_scene_add_rectangle(scene);
    if (!ui_rect_has_area(row))
        return true;
    const UiRect tile = guild_rail_icon_rectangle(row);
    const bool hovered = scene.add_button.generation && scene.widgets.hovered.slot == scene.add_button.slot &&
                         scene.widgets.hovered.generation == scene.add_button.generation;
    const Color green = ui_icon_role_color(theme, UiIconRole::success);
    if (!push_rounded(stream, tile.left, tile.top, tile.right, tile.bottom, hovered ? green : theme.surface_high,
                      hovered ? 14.0f : ui_rect_width(tile) * 0.5f) ||
        !record_content(stream, scene.widgets, scene.add_button, tile, UiLayoutContentKind::control))
        return false;
    const float cx = (tile.left + tile.right) * .5f, cy = (tile.top + tile.bottom) * .5f;
    const float half = 12.0f;
    return ui_icon_paint(stream, UiIcon::plus, {cx - half, cy - half, cx + half, cy + half},
                         hovered ? ui_icon_role_color(theme, UiIconRole::on_accent) : green);
}

bool paint_voice_participants(PaintStream* stream, const ShellNavigationScene& scene,
                              const ShellNavigationRowWidget& row_widget, UiRect row_rectangle,
                              const VoiceStateStore& voice_states, UiRect viewport,
                              const Theme& theme) noexcept {
    UiVoiceParticipantRow user{};
    for (u32 i = 0; ui_voice_participant_row(row_widget.view, row_rectangle, voice_states, i, &user); ++i) {
        if (user.row.bottom <= viewport.top || user.row.top >= viewport.bottom)
            continue;
        // Speaking ring: a success-coloured disc 2 DIP larger than the avatar, drawn beneath it.
        const UiRect& a = user.avatar;
        if (user.speaking && !push_rounded(stream, a.left - 2, a.top - 2, a.right + 2, a.bottom + 2,
                                           ui_icon_role_color(theme, UiIconRole::success),
                                           ui_rect_width(a) * 0.5f + 2))
            return false;
        if (!push_rounded(stream, user.avatar.left, user.avatar.top, user.avatar.right, user.avatar.bottom,
                          theme.avatar_placeholder, ui_rect_width(user.avatar) * 0.5f) ||
            !record_content(stream, scene.widgets, row_widget.widget, user.avatar, UiLayoutContentKind::media,
                            ui_layout_content_bounds_allow_overlap))
            return false;
        const UiRect indicators[]{user.muted, user.deafened};
        const UiIcon icons[]{UiIcon::mic_off, UiIcon::headphones_off};
        for (u32 k = 0; k < 2; ++k)
            if (const auto& indicator = indicators[k]; ui_rect_has_area(indicator) &&
                (!ui_icon_paint(stream, icons[k], indicator, theme.notification) ||
                 !record_content(stream, scene.widgets, row_widget.widget, indicator, UiLayoutContentKind::icon,
                                 ui_layout_content_bounds_allow_overlap)))
                return false;
        // Camera on; clicking the row opens the call view focused on this user.
        if (ui_rect_has_area(user.video) &&
            (!ui_icon_paint(stream, UiIcon::video, user.video, ui_icon_role_color(theme, UiIconRole::muted)) ||
             !record_content(stream, scene.widgets, row_widget.widget, user.video, UiLayoutContentKind::icon,
                             ui_layout_content_bounds_allow_overlap)))
            return false;
        // Go Live badge; its label is emitted with the row text. Hovering the
        // row previews the stream, clicking the badge watches it.
        if (const auto& live = user.live; ui_rect_has_area(live) &&
            (!push_rounded(stream, live.left, live.top, live.right, live.bottom, theme.tokens.accent.danger,
                          theme.scale.radius_sm) ||
             !record_content(stream, scene.widgets, row_widget.widget, live, UiLayoutContentKind::control,
                             ui_layout_content_bounds_allow_overlap)))
            return false;
    }
    return true;
}

Color folder_color(const Theme& theme, const GuildViewRow& row, float alpha) noexcept {
    Color color = row.folder_color == kGuildFolderNoColor
                      ? theme.tokens.emphasis.outline
                      : Color{static_cast<float>((row.folder_color >> 16) & 255) / 255.f,
                              static_cast<float>((row.folder_color >> 8) & 255) / 255.f,
                              static_cast<float>(row.folder_color & 255) / 255.f, 1.f};
    color.alpha *= alpha;
    return color;
}

// Expanded (or animating) folders sit on one tinted plate spanning the
// header and its visible members.
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
            if (!push_rounded(stream, plate.left, plate.top, plate.right, plate.bottom,
                              folder_color(theme, first.view, .15f), 16) ||
                !record_content(stream, scene.widgets, first.widget, plate, UiLayoutContentKind::decoration,
                                static_cast<u16>(ui_layout_content_bounds_allow_overflow |
                                                 ui_layout_content_bounds_allow_overlap)))
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
    if (row.folder_expanded) {
        const float half = kUiNavigationIconExtent * .5f;
        const float x = (rectangle.left + rectangle.right) * .5f, y = (rectangle.top + rectangle.bottom) * .5f;
        return (!hovered || push_rounded(stream, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom,
                                         folder_color(theme, row, .25f), 12)) &&
               ui_icon_paint(stream, UiIcon::folder, {x - half, y - half, x + half, y + half},
                             folder_color(theme, row, 1));
    }
    if (!push_rounded(stream, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom,
                      folder_color(theme, row, hovered ? .55f : .4f), 13))
        return false;
    // Members without artwork get a neutral disc; icons are queued as media.
    for (u32 k = 0; navigation && k < 4 && k < row.folder_size; ++k) {
        const u32 source = row_widget.source_index + 1 + k;
        if (source >= navigation->guild_count || navigation->guild_rows[source].has_icon)
            continue;
        const UiRect mini = guild_rail_folder_preview_rectangle(rectangle, k);
        if (!push_rounded(stream, mini.left, mini.top, mini.right, mini.bottom, theme.surface_high,
                          ui_rect_width(mini) * .5f))
            return false;
    }
    return true;
}

} // namespace

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
           paint_stream_push(stream, divider, middle - 10, divider + 1, middle + 10, theme.tokens.border.subtle) &&
           record_content(stream, scene.widgets, scene.reply_mention, mention->rectangle,
                          UiLayoutContentKind::control, ui_layout_content_bounds_allow_overlap);
}

bool shell_presentation_build_chrome(const UiScene& scene, const NavigationView* navigation,
                                     const GuildRailScene* guild_scene,
                                     const ShellNavigationScene* channel_scene,
                                     const MessageComposerScene* composer_scene,
                                     const MemberListScene* member_scene, const Theme& theme,
                                     float viewport_width, float viewport_height, PaintStream* stream,
                                     ShellPresentationRegions* regions,
                                     const MessageComposerPresentationInput* composer_presentation,
                                     u64 visual_selected_channel_id) noexcept {
    if (!stream || !regions || !ui_extent_valid(viewport_width) || !ui_extent_valid(viewport_height) ||
        viewport_width <= 0.0f || viewport_height <= 0.0f) {
        return false;
    }
    const UiNode* root = ui_scene_resolve_semantic(scene, UiSemantic::root);
    const UiNode* guild = ui_scene_resolve_semantic(scene, UiSemantic::guild_rail);
    const UiNode* guild_navigation = ui_scene_resolve_semantic(scene, UiSemantic::guild_navigation);
    const UiNode* home = ui_scene_resolve_semantic(scene, UiSemantic::home_button);
    const UiNode* channels = ui_scene_resolve_semantic(scene, UiSemantic::channel_sidebar);
    const UiNode* channel_header = ui_scene_resolve_semantic(scene, UiSemantic::channel_header);
    const UiNode* channel_navigation = ui_scene_resolve_semantic(scene, UiSemantic::channel_navigation);
    const UiNode* channel_footer = ui_scene_resolve_semantic(scene, UiSemantic::channel_footer);
    const UiNode* title = ui_scene_resolve_semantic(scene, UiSemantic::title_bar);
    const UiNode* member_toggle = ui_scene_resolve_semantic(scene, UiSemantic::member_toggle);
    const UiNode* content = ui_scene_resolve_semantic(scene, UiSemantic::content);
    const UiNode* messages = ui_scene_resolve_semantic(scene, UiSemantic::message_view);
    const UiNode* composer = ui_scene_resolve_semantic(scene, UiSemantic::composer);
    const UiNode* members = ui_scene_resolve_semantic(scene, UiSemantic::member_sidebar);
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

    const bool home_hovered = scene.widgets.hovered.slot == home->widget.slot &&
                              scene.widgets.hovered.generation == home->widget.generation;
    const bool home_selected = navigation && !navigation->selected_guild_id;
    if (!paint_guild_indicator(stream, guild->rectangle.left, home->rectangle,
                               guild_scene ? &guild_scene->home_indicator : nullptr, home_selected,
                               home_hovered, false, theme.text_primary))
        return false;
    Color home_fill = home_selected  ? theme.tokens.emphasis.fill
                      : home_hovered ? ui_background_hover_color(theme, theme.interactive, .32f)
                                     : theme.surface_high;
    if (background && !home_selected)
        home_fill.alpha = 1;
    if (!ui_background_paint_surface(stream, home->rectangle, theme, home_fill, home_fill, theme.scale.radius_md,
                                     background && !home_selected ? .75f : 0, home_hovered ? 1.f : .80f,
                                     home_selected ? UiBackgroundSurfaceMode::tinted
                                                   : UiBackgroundSurfaceMode::gradient,
                                     UiBackgroundTier::highest, UiBackgroundBorderTone::palette) ||
        !record_content(stream, scene.widgets, home->widget, home->rectangle, UiLayoutContentKind::control))
        return false;

    const float home_center_x = (home->rectangle.left + home->rectangle.right) * 0.5f;
    const float home_center_y = (home->rectangle.top + home->rectangle.bottom) * 0.5f;
    const float home_icon_half = kUiNavigationIconExtent * 0.5f;
    if (!ui_icon_paint(stream, UiIcon::home,
                       {home_center_x - home_icon_half, home_center_y - home_icon_half,
                        home_center_x + home_icon_half, home_center_y + home_icon_half},
                       theme.text_primary)) {
        return false;
    }
    // Friend and message requests badge Home, cut out of the rail like a server badge.
    if (navigation && navigation->home_badge) {
        const float cutout = ui_background_active(theme) ? 0.0f : theme.scale.space_1 * 1.5f;
        const auto badge = ui_mention_badge_guild_rectangle(home->rectangle, navigation->home_badge);
        if (!paint_stream_push_rounded_border(stream, pixel_truncated(badge.left - cutout),
                                              pixel_truncated(badge.top - cutout),
                                              pixel_truncated(badge.right + cutout),
                                              pixel_truncated(badge.bottom + cutout), theme.notification,
                                              theme.base_lowest, ui_rect_height(badge) * 0.5f + cutout, cutout) ||
            !record_content(stream, scene.widgets, home->widget, badge, UiLayoutContentKind::decoration,
                            static_cast<u16>(ui_layout_content_bounds_allow_overflow |
                                             ui_layout_content_bounds_allow_overlap)))
            return false;
    }

    if (guild_scene && guild_scene->active) {
        if (!paint_stream_push_clip(stream, guild_navigation->rectangle.left,
                                    guild_navigation->rectangle.top, guild_navigation->rectangle.right,
                                    guild_navigation->rectangle.bottom)) {
            return false;
        }
        if (!paint_folder_groups(stream, *guild_scene, theme))
            return false;
        for (u32 index = 0; index < guild_scene->count; ++index) {
            const GuildRailRowWidget& row_widget = guild_scene->rows[index];
            const GuildViewRow& row = row_widget.view;
            const UiLayoutNode* layout = guild_rail_scene_layout_node(*guild_scene, row_widget);
            if (!layout) {
                return false;
            }
            const bool header = row.folder_role == GuildFolderRole::header;
            const UiRect rectangle = guild_rail_icon_rectangle(layout->rectangle);
            const GuildRailRowWidget* hovered =
                guild_rail_scene_resolve(*guild_scene, guild_scene->widgets.hovered);
            const bool row_hovered = hovered == &row_widget;
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
                if (!paint_folder_header(stream, navigation, row_widget, rectangle, row_hovered, theme) ||
                    !record_content(stream, guild_scene->widgets, row_widget.widget, rectangle,
                                    UiLayoutContentKind::control))
                    return false;
            }
            // A pinned conversation sits on avatar placeholders like its
            // conversation row; the avatars are media above.
            if (row.folder_role == GuildFolderRole::conversation) {
                if (!ui_private_avatar_placeholder_paint(stream, row.avatar_shape, rectangle, theme) ||
                    !record_content(stream, guild_scene->widgets, row_widget.widget, rectangle,
                                    UiLayoutContentKind::control))
                    return false;
                continue;
            }
            // The fallback tile belongs to guilds without artwork. Painting it
            // below an icon replaces the rail backdrop in transparent pixels.
            if (!header && !row.has_icon &&
                !push_rounded(stream, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom,
                              row_hovered       ? ui_background_hover_color(theme, theme.interactive, .32f)
                              : row.unavailable ? theme.interactive
                              // A light rail needs the raised white tile to separate the acronym.
                              : theme.appearance == ThemeAppearance::light ? theme.surface
                                                                           : theme.surface_high,
                              guild_rail_icon_radius(rectangle, &row_widget.indicator, row.selected, row_hovered))) {
                return false;
            }
            if (!header && !record_content(stream, guild_scene->widgets, row_widget.widget, rectangle,
                                           UiLayoutContentKind::control)) {
                return false;
            }
            // Mention and plugin badges sit above the icon's media:
            // shell_presentation_build_guild_rail_overlay.
        }
        if (!paint_add_server(stream, *guild_scene, theme) ||
            !paint_drag_visual(stream, guild_scene->drag, theme, 16.0f))
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
                !paint_stream_push(stream, clip.left, clip.top, clip.right, clip.bottom, placeholder))
                return false;
        }
        if (navigation && channel_scene && channel_scene->active) {
            if (!paint_stream_push_clip(
                    stream, channel_navigation->rectangle.left, channel_navigation->rectangle.top,
                    channel_navigation->rectangle.right, channel_navigation->rectangle.bottom)) {
                return false;
            }
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
                const ShellNavigationRowWidget* hovered =
                    shell_navigation_scene_resolve(*channel_scene, channel_scene->widgets.hovered);
                const bool row_hovered = hovered == &row_widget;
                const float row_top = row_rectangle.top;
                // Voice rows extend below their 32-DIP head for connected users.
                const float row_height = row.voice_count || row.stage_live ? kShellNavigationVoiceHeadHeight
                                                                           : ui_rect_height(row_rectangle);
                const float selected_right = row_rectangle.right - 8.0f;
                const bool thread = shell_navigation_thread_row(row) && !channel_is_private(row.type);
                const auto sibling = [&](u32 at) noexcept {
                    return at < channel_scene->count && shell_navigation_thread_row(channel_scene->rows[at].view) &&
                           channel_scene->rows[at].view.parent_id == row.parent_id;
                };
                const ShellThreadSpine spine =
                    thread ? shell_navigation_thread_spine(row_rectangle, row_height, icon_layout->rectangle,
                                                           index && !sibling(index - 1), sibling(index + 1))
                           : ShellThreadSpine{};
                // Thread highlights start where the spine's arm ends.
                const float selected_left =
                    minimum(selected_right - 1.0f,
                            thread ? spine.arm_end.x + 2.0f
                                   : maximum(row_rectangle.left + 8.0f, icon_layout->rectangle.left - 8.0f));
                const UiRect selected_rectangle{selected_left, row_top + 1.0f, selected_right,
                                                row_top + row_height - 1.0f};
                if (row_rectangle.bottom > channel_navigation->rectangle.top &&
                    row_rectangle.top < channel_navigation->rectangle.bottom) {
                    if ((row_selected || row_hovered) &&
                        !push_rounded(stream, selected_rectangle.left, selected_rectangle.top,
                                      selected_rectangle.right, selected_rectangle.bottom,
                                      ui_background_hover_color(
                                          theme, row_selected ? theme.interactive : theme.surface_high,
                                          row_selected ? .28f : .20f),
                                      4.0f)) {
                        return false;
                    }
                    if ((row_selected || row_hovered) &&
                        !record_content(stream, channel_scene->widgets, row_widget.widget,
                                        selected_rectangle, UiLayoutContentKind::decoration,
                                        ui_layout_content_bounds_allow_overlap)) {
                        return false;
                    }
                    const UiRect& icon_rectangle = icon_layout->rectangle;
                    const Color row_color = ui_navigation_foreground(theme, row, row_selected);
                    if (row.unread && !row_selected &&
                        !push_rounded(stream, row_rectangle.left - 3.0f, row_top + 10.0f,
                                      row_rectangle.left + 3.0f, row_top + row_height - 10.0f,
                                      theme.text_primary, 3.0f)) {
                        return false;
                    }
                    if (row.unread && !row_selected &&
                        !record_content(stream, channel_scene->widgets, row_widget.widget,
                                        UiRect{row_rectangle.left - 3.0f, row_top + 10.0f,
                                               row_rectangle.left + 3.0f, row_top + row_height - 10.0f},
                                        UiLayoutContentKind::decoration,
                                        static_cast<u16>(ui_layout_content_bounds_allow_overflow |
                                                         ui_layout_content_bounds_allow_overlap))) {
                        return false;
                    }
                    if (channel_is_private(row.type)) {
                        const u8 shape = ui_private_avatar_shape(
                            navigation && navigation->private_rows ? navigation->private_rows->state : nullptr,
                            row.id, row.type);
                        if (!ui_private_avatar_placeholder_paint(stream, shape, icon_rectangle, theme))
                            return false;
                    } else if (thread ? !paint_thread_spine(stream, spine, theme.interactive_hover)
                                      : !ui_icon_paint(stream, channel_row_icon(row), icon_rectangle,
                                                       row.stage_live ? ui_icon_role_color(theme, UiIconRole::success)
                                                                      : row_color)) {
                        return false;
                    }
                    if (!record_content(stream, channel_scene->widgets, row_widget.icon, icon_rectangle,
                                        UiLayoutContentKind::icon)) {
                        return false;
                    }
                    if (row.mention_count != 0 &&
                        !push_rounded(stream, badge_layout->rectangle.left, badge_layout->rectangle.top,
                                      badge_layout->rectangle.right, badge_layout->rectangle.bottom,
                                      theme.notification, 8.0f)) {
                        return false;
                    }
                    if (row.mention_count != 0 &&
                        !record_content(stream, channel_scene->widgets, row_widget.mention_badge,
                                        badge_layout->rectangle, UiLayoutContentKind::control)) {
                        return false;
                    }
                    UiPluginSlotBadge plugin{};
                    if (shell_navigation_plugin_badge(*channel_scene, index, &plugin) &&
                        !ui_plugin_slot_badge_paint(plugin, channel_scene->widgets, theme, stream))
                        return false;
                    if (row.voice_count && navigation->voice_states &&
                        !paint_voice_participants(stream, *channel_scene, row_widget, row_rectangle,
                                                  *navigation->voice_states, channel_navigation->rectangle,
                                                  theme))
                        return false;
                    // The dragged row sits back under a veil of the list colour.
                    if (channel_scene->drag.active && channel_scene->drag.source_id == row.id) {
                        Color veil = theme.base_lower;
                        veil.alpha = .6f;
                        if (!push_rounded(stream, row_rectangle.left, row_top, row_rectangle.right,
                                          row_top + row_height, veil, 4.0f))
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
    if (const UiNode* title_label = ui_scene_resolve_semantic(scene, UiSemantic::title_label); title_label && title) {
        UiRect line = title_label->rectangle;
        line.top = title->rectangle.top;
        line.bottom = title->rectangle.bottom;
        UiPluginSlotBadge header{};
        if (ui_plugin_slot_line_badge(kUiPluginSlotHeaderToolbar, 0, 0, 0, title_label->widget, line, nullptr, &header) &&
            !ui_plugin_slot_badge_paint(header, scene.widgets, theme, stream))
            return false;
    }
    if (member_toggle) {
        if (!ui_button_icon_paint(stream, scene.widgets, member_toggle->widget, UiIcon::members, theme,
                                  members != nullptr, false, UiButtonSurface::palette)) {
            return false;
        }
        if (!record_content(stream, scene.widgets, member_toggle->widget, member_toggle->rectangle,
                            UiLayoutContentKind::control, ui_layout_content_bounds_allow_overlap)) {
            return false;
        }
    }
    if (const auto* pins = ui_scene_resolve_semantic(scene, UiSemantic::pins_toggle)) {
        if (!ui_button_icon_paint(stream, scene.widgets, pins->widget, UiIcon::pin, theme, false, false,
                                  UiButtonSurface::palette) ||
            !record_content(stream, scene.widgets, pins->widget, pins->rectangle,
                            UiLayoutContentKind::control, ui_layout_content_bounds_allow_overlap))
            return false;
    }
    if (const auto* inbox = ui_scene_resolve_semantic(scene, UiSemantic::inbox_toggle)) {
        if (!ui_button_icon_paint(stream, scene.widgets, inbox->widget, UiIcon::inbox, theme, false, false,
                                  UiButtonSurface::palette) ||
            !record_content(stream, scene.widgets, inbox->widget, inbox->rectangle,
                            UiLayoutContentKind::control, ui_layout_content_bounds_allow_overlap))
            return false;
    }
    if (const auto* settings = ui_scene_resolve_semantic(scene, UiSemantic::settings)) {
        if (!ui_button_icon_paint(stream, scene.widgets, settings->widget, UiIcon::settings, theme) ||
            !record_content(stream, scene.widgets, settings->widget, settings->rectangle,
                            UiLayoutContentKind::control, ui_layout_content_bounds_allow_overlap))
            return false;
    }
    const struct {
        UiSemantic semantic;
        UiIcon icon;
    } private_buttons[]{{UiSemantic::private_add, UiIcon::user_plus}, {UiSemantic::private_call, UiIcon::phone}};
    for (const auto& item : private_buttons) {
        if (const auto* button = ui_scene_resolve_semantic(scene, item.semantic);
            button && (!ui_button_icon_paint(stream, scene.widgets, button->widget, item.icon, theme, false, false,
                                             UiButtonSurface::palette) ||
                       !record_content(stream, scene.widgets, button->widget, button->rectangle,
                                       UiLayoutContentKind::control, ui_layout_content_bounds_allow_overlap)))
            return false;
    }
    if (const auto* create = ui_scene_resolve_semantic(scene, UiSemantic::private_create)) {
        const bool hovered = scene.widgets.hovered.slot == create->widget.slot &&
                             scene.widgets.hovered.generation == create->widget.generation;
        const UiRect r = create->rectangle;
        if (!ui_icon_paint(stream, UiIcon::plus, {r.left + 4, r.top + 4, r.right - 4, r.bottom - 4},
                           hovered ? theme.text_primary : theme.text_secondary) ||
            !record_content(stream, scene.widgets, create->widget, r, UiLayoutContentKind::control,
                            ui_layout_content_bounds_allow_overlap))
            return false;
    }
    // Server header dropdown: hover (and open) fill, then the chevron that
    // becomes an X while the popout is open.
    if (const auto* menu = ui_scene_resolve_semantic(scene, UiSemantic::guild_menu);
        menu && navigation && navigation->selected_guild_id &&
        !shell_presentation_guild_banner_visible(scene, channel_scene)) {
        const bool hovered = scene.widgets.hovered.slot == menu->widget.slot &&
                             scene.widgets.hovered.generation == menu->widget.generation;
        const UiRect r = menu->rectangle;
        if ((hovered || scene.guild_menu_expanded) &&
            !push_rounded(stream, r.left, r.top, r.right, r.bottom,
                          ui_background_hover_color(theme, theme.surface_high, scene.guild_menu_expanded ? .28f : .20f),
                          kUiShellGuildMenuRadius))
            return false;
        const float extent = kUiShellGuildMenuChevron;
        const float right = maximum(r.left + extent, r.right - 8.f);
        const float top = (r.top + r.bottom - extent) * .5f;
        if (!ui_icon_paint(stream, scene.guild_menu_expanded ? UiIcon::close : UiIcon::chevron_down,
                           {right - extent, top, right, top + extent}, theme.text_primary) ||
            !record_content(stream, scene.widgets, menu->widget, r, UiLayoutContentKind::control,
                            ui_layout_content_bounds_allow_overlap))
            return false;
    }
    if (channel_footer && navigation && navigation->voice_panel) {
        const auto& voice = *navigation->voice_panel;
        const UiRect footer = channel_footer->rectangle;
        if (!ui_voice_panel_paint(stream, voice, footer, theme) ||
            !ui_voice_account_controls_paint(stream, voice, footer, theme))
            return false;
        const UiRect controls[]{ui_voice_account_button(footer, UiVoiceControl::mute),
                                ui_voice_account_button(footer, UiVoiceControl::deafen)};
        for (const UiRect& control : controls)
            if (!record_content(stream, scene.widgets, channel_footer->widget, control,
                                UiLayoutContentKind::control, ui_layout_content_bounds_allow_overlap))
                return false;
        // Plugin buttons left of the microphone.
        UiPluginSlotNode plugin_nodes[kUiPluginSlotAccountButtons]{};
        const u32 plugin_count = ui_plugin_slot_account_buttons({plugin_nodes, kUiPluginSlotAccountButtons});
        for (u32 i = 0; i < plugin_count; ++i) {
            const UiRect r = ui_plugin_slot_account_button(footer, i);
            const bool hovered = g_ui_plugin_slot_hover.active &&
                                 g_ui_plugin_slot_hover.surface == kUiPluginSlotAccountPanel &&
                                 g_ui_plugin_slot_hover.index == i;
            if ((hovered && !push_rounded(stream, r.left, r.top, r.right, r.bottom,
                                          ui_background_hover_color(theme, theme.surface_high, .20f),
                                          theme.scale.radius_sm)) ||
                !ui_icon_paint(stream, ui_plugin_slot_icon(plugin_nodes[i]),
                               {r.left + 7, r.top + 7, r.right - 7, r.bottom - 7},
                               ui_plugin_slot_ink(theme, plugin_nodes[i].tone, theme.text_secondary)) ||
                !record_content(stream, scene.widgets, channel_footer->widget, r, UiLayoutContentKind::control,
                                ui_layout_content_bounds_allow_overlap))
                return false;
        }
    }
    if (const auto* friends = ui_scene_resolve_semantic(scene, UiSemantic::friends_button)) {
        const bool hovered = scene.widgets.hovered.slot == friends->widget.slot &&
                             scene.widgets.hovered.generation == friends->widget.generation;
        if (!push_rounded(stream, friends->rectangle.left, friends->rectangle.top, friends->rectangle.right,
                          friends->rectangle.bottom,
                          scene.view == UiShellView::friends
                              ? ui_background_hover_color(theme, theme.interactive, .28f)
                          : hovered ? ui_background_hover_color(theme, theme.surface_high, .20f)
                                    : ui_background_surface_color(theme, theme.base_lower, .12f),
                          4.f) ||
            !record_content(stream, scene.widgets, friends->widget, friends->rectangle,
                            UiLayoutContentKind::control))
            return false;
    }
    if (members && member_scene && member_scene->active) {
        if (!paint_stream_push_clip(stream, members->rectangle.left, members->rectangle.top,
                                    members->rectangle.right, members->rectangle.bottom)) {
            return false;
        }
        for (u32 index = 0; index < member_scene->count; ++index) {
            const MemberListRowWidget& row = member_scene->rows[index];
            const UiLayoutNode* row_layout = member_list_scene_layout_node(*member_scene, row.widget);
            if (!row_layout) {
                paint_stream_pop_clip(stream);
                return false;
            }

            if (member_list_scene_row_resident(*member_scene, index) &&
                row.kind == MemberListRowKind::group) {
                continue;
            }
            const UiLayoutNode* avatar_layout = member_list_scene_layout_node(*member_scene, row.avatar);
            if (!avatar_layout ||
                !push_rounded(stream, avatar_layout->rectangle.left, avatar_layout->rectangle.top,
                              avatar_layout->rectangle.right, avatar_layout->rectangle.bottom,
                              theme.avatar_placeholder, 16.0f) ||
                !record_content(stream, member_scene->widgets, row.avatar, avatar_layout->rectangle,
                                UiLayoutContentKind::media)) {
                paint_stream_pop_clip(stream);
                return false;
            }
            if (!member_list_scene_row_resident(*member_scene, index)) {
                const auto* label = member_list_scene_layout_node(*member_scene, row.label);
                if (!label ||
                    !push_rounded(
                        stream, label->content_rectangle.left, label->content_rectangle.top + 9.0f,
                        minimum(label->content_rectangle.right, label->content_rectangle.left + 92.0f),
                        label->content_rectangle.top + 17.0f, theme.avatar_placeholder, 4.0f)) {
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
            UiBackgroundTier::highest, UiBackgroundBorderTone::palette) ||
        !record_content(stream, composer_surface ? composer_scene->widgets : scene.widgets,
                        composer_surface ? composer_scene->surface : composer->widget, composer_rectangle,
                        UiLayoutContentKind::control)) {
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
        !paint_stream_push_rounded_border(stream, edit->rectangle.left, edit->rectangle.top,
                                          edit->rectangle.right, edit->rectangle.bottom,
                                          background         ? Color{}
                                          : composer_enabled ? theme.surface_high
                                                             : theme.base_low,
                                          Color{}, kUiControlCornerRadius, 0.0f) ||
        (voice_bar && !ui_composer_voice_bar_paint(*composer_scene->voice, edit->content_rectangle, theme, stream)) ||
        (!composer_scene->command.bot_id &&
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
    UiPluginSlotNode plugin_nodes[kMessageComposerPluginSlots]{};
    const u32 plugin_count = minimum<u32>(
        composer_scene->plugin_count,
        ui_plugin_slot_toolbar(kUiPluginSlotComposerToolbar, {plugin_nodes, kMessageComposerPluginSlots}));
    for (u32 i = 0; i < plugin_count; ++i)
        if (!ui_button_icon_paint(stream, composer_scene->widgets, composer_scene->plugin_slots[i],
                                  ui_plugin_slot_icon(plugin_nodes[i]), theme))
            return false;
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
                                         UiBackgroundTier::highest, UiBackgroundBorderTone::palette) ||
            !record_content(stream, composer_scene->widgets, composer_scene->attachments,
                            attachments->rectangle, UiLayoutContentKind::control,
                            ui_layout_content_bounds_allow_overlap))
            return false;
    }
    if (composer_scene->reply_message_id != 0) {
        if (!reply ||
            !ui_background_paint_surface(stream, reply->rectangle, theme, theme.base_lower,
                                         theme.interactive, ui_theme_metrics(theme.styles).compact_radius,
                                         0, 1, UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest,
                                         UiBackgroundBorderTone::palette) ||
            !record_content(stream, composer_scene->widgets, composer_scene->reply, reply->rectangle,
                            UiLayoutContentKind::control, ui_layout_content_bounds_allow_overlap)) {
            return false;
        }
        const auto* cancel =
            message_composer_scene_layout_node(*composer_scene, MessageComposerControl::cancel_reply);
        if (!cancel ||
            !ui_button_icon_paint(stream, composer_scene->widgets, composer_scene->reply_cancel,
                                  UiIcon::close, theme, false, true, UiButtonSurface::palette) ||
            !record_content(stream, composer_scene->widgets, composer_scene->reply_cancel,
                            cancel->rectangle, UiLayoutContentKind::control,
                            ui_layout_content_bounds_allow_overlap))
            return false;
        if (composer_scene->reply_mention_visible &&
            !shell_presentation_paint_reply_mention(*composer_scene, theme, stream))
            return false;
    }
    if (message_composer_scene_typing_label(*composer_scene).bytes != 0) {
        const float center = (typing->rectangle.top + typing->rectangle.bottom) * 0.5f;
        for (u32 index = 0; index < 3; ++index) {
            const float left = typing->rectangle.left + 4.0f + static_cast<float>(index) * 5.0f;
            if (!push_rounded(stream, left, center - 2.0f, left + 4.0f, center + 2.0f,
                              (composer_scene->status_is_error || composer_scene->status_tone == 4 ? theme.notification
                              : composer_scene->status_tone == 3                        ? theme.tokens.text.warning
                              : composer_scene->status_tone == 2                        ? theme.tokens.text.positive
                              : composer_scene->status_tone == 1                        ? theme.accent
                                                                                        : theme.text_primary),
                              2.0f)) {
                return false;
            }
        }
    }
    if (composer_presentation && composer_presentation->geometry_active && composer_scene->model->focused) {
        for (u32 i = 0; i < composer_presentation->composition.count; ++i) {
            auto mark = composer_presentation->composition[i];
            mark.attributes.line_style = TextCompositionLineStyle::none;
            if (!text_composition_paint_mark(stream, mark, edit->content_rectangle, theme.text_primary))
                return false;
        }
    }
    return record_content(stream, composer_scene->widgets, composer_scene->add_attachment,
                          attachment->rectangle, UiLayoutContentKind::control) &&
           record_content(stream, composer_scene->widgets, composer_scene->edit, edit->rectangle,
                          UiLayoutContentKind::control) &&
           record_content(stream, composer_scene->widgets, composer_scene->emoji, emoji->rectangle,
                          UiLayoutContentKind::control) &&
           record_content(stream, composer_scene->widgets, composer_scene->send, send->rectangle,
                          UiLayoutContentKind::control);
}

UiRect caret_pixel_rectangle(UiRect caret, UiRect clip, u32 dpi) noexcept {
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
bool shell_presentation_build_composer_overlay(const MessageComposerScene* scene,
                                               const MessageComposerPresentationInput& input,
                                               const Theme& theme, PaintStream* paint, u32 dpi) noexcept {
    // Spelling marks stay visible without focus; the caret needs it.
    const bool focused = scene && scene->model && scene->model->focused;
    if (!scene || !scene->active || !scene->model || !input.geometry_active || (!focused && !input.composition.count))
        return true;
    const auto* edit = message_composer_scene_layout_node(*scene, MessageComposerControl::edit);
    if (!edit || !paint_stream_push_clip(paint, edit->content_rectangle.left, edit->content_rectangle.top,
                                         edit->content_rectangle.right, edit->content_rectangle.bottom))
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
            ok = paint_stream_push(paint, r.left, r.top, r.right, r.bottom, color);
    }
    return paint_stream_pop_clip(paint) && ok;
}

bool shell_presentation_guild_banner_visible(const UiScene& scene, const ShellNavigationScene* channel_scene) noexcept {
    const UiNode* channels = ui_scene_resolve_semantic(scene, UiSemantic::channel_sidebar);
    const UiNode* header = ui_scene_resolve_semantic(scene, UiSemantic::channel_header);
    return channel_scene && channel_scene->banner_inset > 0 && channels && header &&
           ui_guild_banner_geometry(channels->rectangle, header->rectangle, channel_scene->scroll_dips, false).visible;
}

bool shell_presentation_build_guild_banner_overlay(const UiScene& scene, const NavigationView* navigation,
                                                   const ShellNavigationScene* channel_scene, const Theme& theme,
                                                   PaintStream* stream) noexcept {
    if (!stream)
        return false;
    const UiNode* channels = ui_scene_resolve_semantic(scene, UiSemantic::channel_sidebar);
    const UiNode* header = ui_scene_resolve_semantic(scene, UiSemantic::channel_header);
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
    Color edge = theme.tokens.border.subtle;
    edge.alpha *= banner.glass;
    if (edge.alpha > 0 && !paint_stream_push(stream, bar.left, bar.bottom, bar.right, bar.bottom + 1, edge))
        return false;
    // The dropdown over the banner: hover fill, then the white chevron (or X).
    const auto* menu = ui_scene_resolve_semantic(scene, UiSemantic::guild_menu);
    if (!menu || !navigation || !navigation->selected_guild_id)
        return true;
    const bool hovered = scene.widgets.hovered.slot == menu->widget.slot &&
                         scene.widgets.hovered.generation == menu->widget.generation;
    const UiRect r = menu->rectangle;
    if ((hovered || scene.guild_menu_expanded) &&
        !push_rounded(stream, r.left, r.top, r.right, r.bottom,
                      scene.guild_menu_expanded ? theme.tokens.state.pressed : theme.tokens.state.hover,
                      kUiShellGuildMenuRadius))
        return false;
    const float extent = kUiShellGuildMenuChevron;
    const float right = maximum(r.left + extent, r.right - 8.f);
    const float top = (r.top + r.bottom - extent) * .5f;
    return ui_icon_paint(stream, scene.guild_menu_expanded ? UiIcon::close : UiIcon::chevron_down,
                         {right - extent, top, right, top + extent}, theme.tokens.text.on_accent) &&
           record_content(stream, scene.widgets, menu->widget, r, UiLayoutContentKind::control,
                          ui_layout_content_bounds_allow_overlap);
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
        !paint_stream_push_clip(stream, member_scene->viewport.left, member_scene->viewport.top,
                                member_scene->viewport.right, member_scene->viewport.bottom)) {
        return false;
    }
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
        if (member_list_scene_resolve(*member_scene, member_scene->widgets.hovered) == &row) {
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
        const GuildMemberRecord* member = member_scene->members && row.record_slot < member_scene->members->capacity
                                              ? &member_scene->members->records[row.record_slot]
                                              : nullptr;
        // Rows without a server record (private conversations) carry their resolved status.
        const auto presence = static_cast<UiPresence>(member ? member->status : row.source_row.status);
        const bool mobile =
            member_scene->users &&
            ui_presence_mobile(presence, user_store_client_status(*member_scene->users, row.user_id));
        // The phone stands 1.5 dots tall above the same bottom-right corner.
        const float rise = mobile ? ui_rect_height(status->rectangle) * .5f : 0.0f;
        const UiRect badge{status->rectangle.left - 2.0f, status->rectangle.top - 2.0f - rise,
                           status->rectangle.right + 2.0f, status->rectangle.bottom + 2.0f};
        if (!ui_presence_paint(stream, status->rectangle, presence, theme.base_lower, theme, mobile) ||
            !record_content(stream, member_scene->widgets, row.status, badge,
                            UiLayoutContentKind::decoration,
                            static_cast<u16>(ui_layout_content_bounds_allow_overflow |
                                             ui_layout_content_bounds_allow_overlap))) {
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
    const UiNode* guild = ui_scene_resolve_semantic(scene, UiSemantic::guild_rail);
    const UiNode* navigation = ui_scene_resolve_semantic(scene, UiSemantic::guild_navigation);
    if (!guild_scene || !guild_scene->active || !guild || !navigation)
        return true;
    if (!paint_stream_push_clip(stream, navigation->rectangle.left, navigation->rectangle.top,
                                navigation->rectangle.right, navigation->rectangle.bottom))
        return false;
    // A rail-coloured ring drawn over the icon's media separates the lower
    // badge from the icon.
    const float cutout = ui_background_active(theme) ? 0.0f : theme.scale.space_1 * 1.5f;
    constexpr u16 overflow =
        static_cast<u16>(ui_layout_content_bounds_allow_overflow | ui_layout_content_bounds_allow_overlap);
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
        const auto badge = ui_mention_badge_guild_rectangle(rectangle, row.mention_count);
        if (row.mention_count != 0 &&
            (!paint_stream_push_rounded_border(stream, pixel_truncated(badge.left - cutout),
                                               pixel_truncated(badge.top - cutout),
                                               pixel_truncated(badge.right + cutout),
                                               pixel_truncated(badge.bottom + cutout), theme.notification,
                                               theme.base_lowest, ui_rect_height(badge) * 0.5f + cutout, cutout) ||
             !record_content(stream, guild_scene->widgets, row_widget.widget, badge, UiLayoutContentKind::decoration,
                             overflow))) {
            paint_stream_pop_clip(stream);
            return false;
        }
        // Plugin badge on the opposite (top-right) corner.
        UiPluginSlotCorner plugin{};
        if (guild_rail_plugin_corner(*guild_scene, index, &plugin) &&
            (!ui_plugin_slot_corner_paint(plugin, theme.base_lowest, theme, stream) ||
             !record_content(stream, guild_scene->widgets, row_widget.widget, plugin.rectangle,
                             UiLayoutContentKind::decoration, overflow))) {
            paint_stream_pop_clip(stream);
            return false;
        }
    }
    return paint_stream_pop_clip(stream);
}

} // namespace maicord
