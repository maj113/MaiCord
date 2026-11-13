#include "scene_internal.hpp"
#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/ui/theme/surface_elevation.hpp"
#include "maicord/ui/chat/messages/presentation.hpp"
#include "maicord/ui/chat/composer/attachment_actions.hpp"

namespace maicord {
using namespace search_scene_detail;

namespace search_scene_detail {
static bool chooser_action(UiSearchAction action) noexcept {
    return action == UiSearchAction::filter_value || ui_search_action_filter(action) ||
           action == UiSearchAction::more_filters || action == UiSearchAction::clear_recent ||
           action == UiSearchAction::recent;
}
static bool sort_action(UiSearchAction action) noexcept {
    return action >= UiSearchAction::newest && action <= UiSearchAction::relevance;
}
// A label of the search surface. `mode` holds ui_text_* bits; chooser_text marks a label of the filter chooser
// (clipped to it). Labels under an open menu are skipped, and a result row clips to what the menus leave of the body.
void text(Pen& out, const UiSearchScene& s, UiWidgetHandle owner, const Utf8View& value, const UiRect& r,
          const Color& color, u32 style, u32 mode, bool chooser_text) noexcept {
    if (!value.bytes || !ui_rect_has_area(r))
        return;
    const auto action = owner.generation && owner.slot < s.storage.widgets.count
                            ? s.targets[owner.slot].action
                            : UiSearchAction::none;
    const bool row = action == UiSearchAction::result || action == UiSearchAction::rich_item;
    const bool filter = chooser_text || chooser_action(action) ||
                        (owner.slot == s.root.slot && utf8_equal(value, utf8_literal("Filters")));
    const bool sort = sort_action(action);
    if (!row && !filter && chooser_open(s) && ui_rect_intersects(r, s.chooser))
        return;
    if (!row && !sort && s.content->sort_menu && ui_rect_intersects(r, s.sort_chooser))
        return;
    auto region = row ? overlay_region(s, r) : widget_viewport(s);
    if (chooser_text || action == UiSearchAction::filter_value || action == UiSearchAction::clear_recent ||
        action == UiSearchAction::recent)
        region = ui_rect_intersection(region, s.chooser);
    out.push(region, owner, value, r, color, style, mode);
}
static void centered(Pen& out, const UiSearchScene& s, UiWidgetHandle owner, const Utf8View& value, const UiRect& r,
                     const Color& color, u32 style = text_style_subtext) noexcept {
    text(out, s, owner, value, r, color, style, ui_text_center);
}
} // namespace search_scene_detail

bool ui_search_scene_paint(const UiSearchScene& s, const Theme& theme, PaintStream* out,
                           const UiEditGeometry* geometry, u32 dpi,
                           MessageMediaCommandStream* rich_media, const MessageTextPresentation* rich_text) noexcept {
    if (!current(s))
        return true;
    UiPaint p{out, theme};
    p.use<ui_edit_field_paint>(s.field, s.text, s.content->edit, geometry, theme.base_lowest, theme, dpi, 8)
        .icon(UiIcon::search, ui_rect_edges(s.field, ui_end(-26), ui_start(6), ui_end(-6), ui_end(-6)),
              theme.text_secondary);
    if (s.scope.generation) {
        // The scope reads as a selected pill (the emphasis tint of selected
        // tabs); its x removes it.
        const auto chip = ui_bounds(s, s.scope);
        const bool focus = ui_focused(s, s.scope), hover = ui_hovered(s, s.scope);
        p.use<ui_background_paint_surface>(chip, theme, ui_emphasis_selected(theme),
                                           focus   ? theme.tokens.border.focus
                                           : hover ? theme.tokens.emphasis.outline
                                                   : Color{},
                                           theme.scale.radius_sm, focus || hover ? 1.f : 0.f, 1.f, kGradient,
                                           UiBackgroundTier::high, kSemantic)
            .icon(UiIcon::close, ui_rect_edges(chip, ui_end(-18), ui_start(6), ui_end(-6), ui_end(-6)),
                  hover || focus ? theme.text_primary : theme.text_secondary);
    }
    if (s.content->panel) {
        const auto panel = s.panel;
        p.use<ui_background_paint_surface>(panel, theme, theme.base_lower, Color{}, 0.f, 0.f, 1.f, kGradient,
                                           UiBackgroundTier::chat, kSemantic)
            .use<paint_tabs>(s, theme)
            .clip(s.body);
        if (tiled(s))
            p.use<paint_items>(s, theme, rich_media);
        for (u32 i = 0; p && i < s.count; ++i) {
            const auto r = s.row_rectangles[i];
            p.use<ui_background_paint_surface>(r, theme, s.visible[i].hit ? theme.surface : theme.base_lower,
                                               s.visible[i].hit ? theme.surface_high : Color{}, 10.f,
                                               s.visible[i].hit ? 1.f : 0.f, 1.f, kGradient, UiBackgroundTier::low,
                                               kSemantic);
            if (rich_enabled(s))
                continue;
            const float x = r.left + 28, y = r.top + 28;
            p.rounded({x - 16, y - 16, x + 16, y + 16}, theme.avatar_placeholder, theme.scale.radius_full)
                .rounded({x - 5, y - 9, x + 5, y + 1}, theme.text_secondary, 5)
                .rounded({x - 9, y + 3, x + 9, y + 11}, theme.text_secondary, 7);
        }
        if (p && rich_enabled(s) && !tiled(s)) {
            const auto* scene = ui_search_scene_message_scene(s);
            MessagePresentationResult result{};
            if (scene && rich_media &&
                message_presentation_build(*scene, theme, out, rich_media, &result, rich_text)) {
                for (u32 i = 0; i < rich_media->count; ++i) {
                    auto& command = rich_media->storage[i];
                    command.clip = rich_item_clip(s, command.rectangle, command.clip);
                }
            } else {
                p.fail();
            }
            for (u32 i = 0; p && i < s.count; ++i)
                if (s.visible[i].unavailable)
                    p.surface(s.row_rectangles[i], theme.surface_high, theme.interactive,
                              ui_theme_metrics(s.theme_styles).control_radius);
        }
        p.unclip().separator({panel.left, panel.bottom - 44, panel.right, panel.bottom - 43});
    }
    if (s.content->sort_menu)
        p.use<ui_surface_paint>(s.sort_chooser, theme, UiElevation::popover,
                                ui_theme_metrics(s.theme_styles).control_radius, &theme.surface_high, nullptr);
    if (chooser_open(s))
        p.use<ui_surface_paint>(s.chooser, theme, UiElevation::popover,
                                s.choice_source ? ui_theme_metrics(s.theme_styles).menu_radius
                                                : ui_theme_metrics(s.theme_styles).control_radius,
                                &theme.surface_high, nullptr);
    for (u32 i = 1; p && i < s.widgets.count; ++i) {
        const auto action = s.targets[i].action;
        if (action == UiSearchAction::edit || action == UiSearchAction::submit)
            continue;
        const UiWidgetHandle self{static_cast<u16>(i), s.widgets.generation};
        const auto r = ui_bounds(s, self);
        // Tabs and item widgets paint their own states.
        if (action == UiSearchAction::tab ||
            (tiled(s) && (action == UiSearchAction::result || action == UiSearchAction::rich_item)))
            continue;
        const bool filter = chooser_action(action), sort = sort_action(action);
        const bool selected =
            (action == UiSearchAction::filter_value && s.targets[i].source_index == s.choice_selected) ||
            (sort && static_cast<u8>(action) - static_cast<u8>(UiSearchAction::newest) == s.content->sort) ||
            (action == UiSearchAction::page &&
             s.targets[i].page_offset / (s.content->page_size ? s.content->page_size : 25) ==
                 s.content->offset / (s.content->page_size ? s.content->page_size : 25));
        const bool hovered =
                       s.widgets.hovered.generation == s.widgets.generation && s.widgets.hovered.slot == i,
                   captured = s.widgets.captured.generation == s.widgets.generation &&
                              s.widgets.captured.slot == i;
        // Jump buttons under an open menu stay hidden with the rest of their card.
        if ((!filter && chooser_open(s) && ui_rect_intersects(r, s.chooser)) ||
            (!sort && s.content->sort_menu && ui_rect_intersects(r, s.sort_chooser)))
            continue;
        if (rich_enabled(s) && action == UiSearchAction::result)
            p.surface(r, theme.surface_high, theme.interactive, ui_theme_metrics(s.theme_styles).control_radius);
        if (action == UiSearchAction::rich_item && s.targets[i].save) {
            if (p && !ui_attachment_save_paint(out, {&s.widgets, self}, r, ui_search_scene_content_clip(s, r), theme,
                                               true))
                p.fail();
            continue;
        }
        if (action == UiSearchAction::filters || action == UiSearchAction::sort)
            p.use<ui_background_paint_surface>(r, theme, theme.surface, theme.surface_high, theme.scale.radius_md,
                                               1.f, .9f, kGradient, UiBackgroundTier::highest,
                                               UiBackgroundBorderTone::palette);
        if (hovered || captured || selected) {
            const float radius =
                action == UiSearchAction::filter_value ? ui_theme_metrics(s.theme_styles).compact_radius : 4.f;
            // Chooser and sort rows stack, so their plates keep the shared menu row gap.
            const UiRect plate = filter || sort ? ui_menu_row_plate(r, theme.scale.space_1) : r;
            if (hovered || captured)
                p.rounded(plate,
                          ui_background_hover_color(theme, captured ? theme.interactive : theme.interactive_hover,
                                                    captured ? .32f : .20f),
                          radius);
            if (selected)
                p.rounded(plate, ui_background_hover_color(theme, theme.interactive, .28f), radius);
        }
        if (action == UiSearchAction::close) {
            const float icon = ui_icon_button_extent(ui_rect_width(r) - 8);
            const float x = (r.left + r.right - icon) * .5f, y = (r.top + r.bottom - icon) * .5f;
            p.icon(UiIcon::close, {x, y, x + icon, y + icon}, hovered || captured ? theme.text_primary : theme.text_secondary);
            continue;
        }
        if (sort) {
            const float x = r.right - 16, y = (r.top + r.bottom) * .5f;
            p.rounded_border({x - 8, y - 8, x + 8, y + 8}, Color{}, theme.text_secondary, theme.scale.radius_full, 1);
            if (selected)
                p.rounded({x - 4, y - 4, x + 4, y + 4}, theme.tokens.emphasis.fill, theme.scale.radius_sm);
        }
        if (action == UiSearchAction::filter_value) {
            UiMenuRowGeometry choice_geometry{};
            if (!choice_menu_geometry(s, r, &choice_geometry))
                return false;
            p.clip(choice_geometry.clip);
            const auto& choice = s.choice_source->rows[s.targets[i].source_index];
            const UiRect avatar = choice_avatar_rectangle(choice_geometry, r);
            if (choice_person(*s.choice_source, choice))
                p.rounded(avatar, theme.avatar_placeholder, ui_rect_height(avatar) * .5f);
            else
                p.icon(choice_icon(*s.choice_source, choice), choice_geometry.icon, theme.text_secondary);
            p.unclip();
            continue;
        }
        if (action == UiSearchAction::sort) {
            const float cy = (r.top + r.bottom) * .5f;
            p.icon(UiIcon::chevron_down, {r.right - 24, cy - 8, r.right - 8, cy + 8}, theme.text_secondary);
        }
        if (action == UiSearchAction::recent) {
            const float cy = (r.top + r.bottom) * .5f;
            p.icon(UiIcon::recent, {r.left + 8, cy - 9, r.left + 26, cy + 9}, theme.text_secondary);
            continue;
        }
        if (filter && action != UiSearchAction::clear_recent) {
            const UiIcon icons[] = {UiIcon::user,       UiIcon::channel,   UiIcon::attachment,
                                    UiIcon::mention,    UiIcon::calendar,  UiIcon::date_before,
                                    UiIcon::date_after, UiIcon::pin,       UiIcon::link,
                                    UiIcon::file,       UiIcon::file_text, UiIcon::bot};
            const float cy = (r.top + r.bottom) * .5f;
            p.clip(r)
                .icon(action == UiSearchAction::more_filters
                          ? UiIcon::filters
                          : icons[static_cast<u8>(action) - static_cast<u8>(UiSearchAction::filter_from)],
                      {r.left + 8, cy - 10, r.left + 28, cy + 10}, theme.text_secondary)
                .unclip();
        }
    }
    if (s.choice_source && s.choice_count && s.choice_count < s.choice_total)
        p.scrollbar(ui_scrollbar_geometry(choice_rows_rectangle(s), s.choice_first * choice_row_height(s),
                                          s.choice_total * choice_row_height(s)));
    if (s.content->panel)
        p.scrollbar(ui_scrollbar_geometry(s.body, s.scroll_dips,
                                          tiled(s) ? s.content_height : s.row_offsets[s.measured_count]));
    return p.end();
}
bool ui_search_scene_text_output(const UiSearchScene& s, const Theme& theme, UiTextOutput* output,
                                 const UiEditGeometry* edit) noexcept {
    if (!current(s))
        return true;
    if (!ui_edit_field_text_output(
            output, {&s.widgets, s.edit}, s.text, s.content->edit, edit,
            s.content->placeholder.bytes ? s.content->placeholder : utf8_literal("Search messages"), theme))
        return false;
    Pen out{output, s.widgets, s.panel};
    if (s.scope.generation) {
        const auto chip = ui_bounds(s, s.scope);
        text(out, s, s.scope, s.content->scope_chip,
             ui_rect_edges(chip, ui_start(8), ui_start(0), ui_end(-20), ui_end(0)), theme.text_primary,
             text_style_subtext);
    }
    if (s.content->panel) {
        const auto panel = s.panel;
        text(out, s, s.root, s.content->heading.bytes ? s.content->heading : utf8_literal("Search results"),
             ui_rect_edges(panel, ui_start(12), ui_start(8), ui_end(-64), ui_start(32)), theme.text_primary,
             text_style_bold);
        text(out, s, s.root, {s.count_text, s.count_bytes},
             ui_rect_edges(panel, ui_start(12), ui_start(40), ui_end(-200), ui_start(72)), theme.text_secondary,
             text_style_subtext);
        if (!chooser_open(s))
            text(out, s, s.root, s.content->filter_summary.bytes ? s.content->filter_summary : s.content->status,
                 s.content->tabs ? ui_rect_edges(panel, ui_start(12), ui_start(118), ui_end(-12), ui_start(142))
                                 : ui_rect_edges(panel, ui_start(12), ui_start(76), ui_end(-12), ui_start(100)),
                 theme.text_secondary, text_style_subtext);
        if (tiled(s)) {
            const auto& projection = *s.rich_projection;
            for (u32 v = 0; v < projection.visible_count; ++v) {
                const auto& item = projection.items[projection.visible[v]];
                const auto r = projection.screens[v];
                const auto owner = projection.primary[v];
                if (s.layout == UiSearchLayout::grid) {
                    const MessageMediaObscure obscure = item_obscure(s, item);
                    if (obscure != MessageMediaObscure::none)
                        centered(out, s, owner, message_media_obscure_label(obscure),
                                 ui_rect_edges(r, ui_start(4), ui_end(-28), ui_end(-4), ui_end(-6)),
                                 theme.text_secondary);
                    continue;
                }
                const float right = projection.secondary[v].generation ? r.right - 72 : r.right - 12;
                const auto link = item_link(s, item);
                const auto detail = s.layout == UiSearchLayout::files
                                        ? Utf8View{projection.details[v], projection.detail_bytes[v]}
                                        : url_hostname(link);
                const auto title = item_title(s, item);
                text(out, s, owner, title.bytes ? title : utf8_literal("Untitled"),
                     {r.left + 56, r.top + 9, right, r.top + 29}, theme.text_primary, text_style_bold);
                text(out, s, owner, detail, {r.left + 56, r.top + 29, right, r.top + 47}, theme.text_secondary,
                     text_style_subtext);
                if (s.layout == UiSearchLayout::links && projection.secondary[v].generation)
                    centered(out, s, projection.secondary[v], utf8_literal("Jump"), item_jump_rectangle(r),
                             theme.text_primary);
            }
            if (!projection.item_count && s.content->phase == UiSearchPhase::ready && !s.content->stale)
                centered(out, s, s.root,
                         s.layout == UiSearchLayout::grid    ? utf8_literal("No media on this page.")
                         : s.layout == UiSearchLayout::files ? utf8_literal("No files on this page.")
                                                             : utf8_literal("No links on this page."),
                         ui_rect_edges(s.body, ui_start(12), ui_start(16), ui_end(-12), ui_start(40)),
                         theme.text_secondary);
        }
        for (u32 i = 0; s.content->tabs && i < 5; ++i) {
            const auto r = ui_bounds(s, s.tabs[i]);
            if (!ui_rect_has_area(r))
                continue;
            const auto color = s.content->tab == i ? theme.text_primary : theme.text_secondary;
            const bool counted = s.tab_bytes[i] != 0;
            const float mid = (r.top + r.bottom - 3) * .5f;
            centered(out, s, s.tabs[i], tab_label(i),
                     counted ? UiRect{r.left + 2, r.top + 3, r.right - 2, mid + 2}
                             : UiRect{r.left + 2, r.top, r.right - 2, r.bottom - 3},
                     color, s.content->tab == i ? text_style_bold | text_style_subtext : text_style_subtext);
            if (counted)
                centered(out, s, s.tabs[i], {s.tab_text[i], s.tab_bytes[i]},
                         {r.left + 2, mid, r.right - 2, r.bottom - 5}, theme.text_secondary);
        }
        for (u32 i = 0; i < s.count; ++i) {
            const auto& row = s.visible[i];
            const auto r = s.row_rectangles[i];
            if (rich_enabled(s)) {
                if (row.unavailable) {
                    text(out, s, s.root, row.body,
                         {r.left + 16, r.top + 12, r.right - (row.access_check ? 88 : 16), r.bottom - 12},
                         theme.text_secondary, text_style_subtext);
                    if (row.access_check)
                        if (const auto* check = ui_widget_manager_layout_node(s.widgets, s.rows[i]))
                            centered(out, s, s.rows[i], utf8_literal("Check access"), check->rectangle,
                                     theme.text_primary);
                    continue;
                }
                const auto* jump = ui_widget_manager_layout_node(s.widgets, s.rows[i]);
                text(out, s, s.rows[i], row.channel, s.group_rectangles[i], theme.text_secondary,
                     text_style_subtext | text_style_bold);
                if (jump)
                    centered(out, s, s.rows[i], utf8_literal("Jump"), jump->rectangle, theme.text_primary);
                continue;
            }
            const auto header = plain_header(r, row);
            if (ui_rect_has_area(s.group_rectangles[i]))
                text(out, s, s.rows[i], row.channel, s.group_rectangles[i], theme.text_secondary,
                     text_style_subtext | text_style_bold);
            text(out, s, s.rows[i], row.author, header.name, row.hit ? theme.text_primary : theme.text_secondary,
                 text_style_bold);
            text(out, s, s.rows[i], row.detail, header.timestamp, theme.text_secondary, text_style_subtext);
            text(out, s, s.rows[i], row.body,
                 ui_rect_edges(r, ui_start(52), ui_start(36), ui_end(-12), ui_end(-12)),
                 row.hit ? theme.text_primary : theme.text_secondary, 0, ui_text_wrap);
        }
    }
    if (s.choice_source) {
        const auto& metrics = ui_theme_metrics(s.theme_styles);
        const auto heading =
            s.choice_source->heading.bytes ? s.choice_source->heading : utf8_literal("Search values");
        const auto hint = s.choice_source->hint.bytes
                              ? s.choice_source->hint
                              : (!s.choice_total ? utf8_literal("No matches. Keep typing.") : Utf8View{});
        const float left = s.chooser.left + metrics.menu_padding, right = s.chooser.right - metrics.menu_padding;
        const float top = s.chooser.top + metrics.menu_padding;
        text(out, s, s.root, heading, {left, top, right, top + choice_heading_height(s)}, theme.text_primary,
             text_style_bold, 0, true);
        const float bottom = s.chooser.bottom - metrics.menu_padding;
        text(out, s, s.root, hint, {left, maximum(top, bottom - choice_hint_height(s)), right, bottom},
             theme.text_secondary, text_style_subtext, ui_text_wrap, true);
    } else if (chooser_open(s)) {
        text(out, s, s.root, utf8_literal("Filters"),
             ui_rect_edges(s.chooser, ui_start(12), ui_start(4), ui_end(-12), ui_start(26)), theme.text_secondary,
             text_style_subtext | text_style_bold);
        if (recent_shown(s))
            text(out, s, s.root, utf8_literal("Recent searches"),
                 {s.chooser.left + 12, recent_top(s), s.chooser.right - 76, recent_top(s) + 28},
                 theme.text_secondary, text_style_subtext | text_style_bold, 0, true);
    }
    for (u32 i = 1; i < s.widgets.count; ++i) {
        const auto a = s.targets[i].action;
        if (a == UiSearchAction::edit || a == UiSearchAction::result || a == UiSearchAction::submit ||
            a == UiSearchAction::rich_item || a == UiSearchAction::tab)
            continue;
        const UiWidgetHandle self{static_cast<u16>(i), s.widgets.generation};
        const auto rectangle = ui_bounds(s, self);
        auto r = rectangle;
        r.left += 4;
        r.right -= 4;
        if (a == UiSearchAction::filter_value) {
            UiMenuRowGeometry geometry{};
            if (!choice_menu_geometry(s, rectangle, &geometry))
                return false;
            const auto& choice = s.choice_source->rows[s.targets[i].source_index];
            text(out, s, self, choice.label, geometry.primary, theme.text_primary, text_style_bold);
            text(out, s, self, choice.detail, geometry.detail, theme.text_secondary, text_style_subtext);
            continue;
        }
        if (a == UiSearchAction::page) {
            for (u32 page = 0; page < s.page_count; ++page)
                if (s.pages[page].slot == i)
                    centered(out, s, s.pages[page], {s.page_text[page], s.page_bytes[page]}, r, theme.text_primary);
            continue;
        }
        if (ui_search_action_filter(a) || a == UiSearchAction::more_filters) {
            static constexpr Utf8View hints[] = {utf8_literal("from:"),
                                                 utf8_literal("in:"),
                                                 utf8_literal("has:image"),
                                                 utf8_literal("mentions:"),
                                                 utf8_literal("on:YYYY-MM-DD"),
                                                 utf8_literal("before:YYYY-MM-DD"),
                                                 utf8_literal("after:YYYY-MM-DD"),
                                                 utf8_literal("pinned:true"),
                                                 utf8_literal("linkFrom:youtube.com"),
                                                 utf8_literal("fileType:pdf"),
                                                 utf8_literal("fileName:report"),
                                                 utf8_literal("authorType:bot")};
            r.left += 32;
            const auto hint = a == UiSearchAction::more_filters
                                  ? utf8_literal("Dates, files, links and more")
                                  : hints[static_cast<u8>(a) - static_cast<u8>(UiSearchAction::filter_from)];
            const float mid = (r.top + r.bottom) * .5f;
            text(out, s, self, label(a), {r.left, r.top + 2, r.right, mid + 2}, theme.text_primary,
                 text_style_subtext);
            text(out, s, self, hint, {r.left, mid, r.right, r.bottom - 2}, theme.text_secondary,
                 text_style_subtext);
            continue;
        }
        if (a == UiSearchAction::recent || a == UiSearchAction::clear_recent) {
            const auto value = a == UiSearchAction::recent && s.targets[i].source_index < s.content->recent_count
                                   ? s.content->recent[s.targets[i].source_index]
                                   : label(a);
            text(out, s, self, value, a == UiSearchAction::recent ? UiRect{r.left + 28, r.top, r.right, r.bottom} : r,
                 a == UiSearchAction::recent ? theme.text_primary : theme.text_secondary, text_style_subtext,
                 a == UiSearchAction::recent ? 0 : ui_text_center);
            continue;
        }
        if (a == UiSearchAction::sort) {
            const Utf8View sorts[] = {label(UiSearchAction::newest), label(UiSearchAction::oldest),
                                      label(UiSearchAction::relevance)};
            centered(out, s, self, sorts[s.content->sort < 3 ? s.content->sort : 0],
                     {r.left + 4, r.top, r.right - 20, r.bottom}, theme.text_primary);
            continue;
        }
        if (a == UiSearchAction::close)
            continue;
        if (sort_action(a))
            r.right -= 28;
        const bool enabled = (s.widgets.storage[i].flags & ui_widget_enabled) != 0;
        text(out, s, self, label(a), r, enabled ? theme.text_primary : theme.text_secondary, text_style_subtext,
             a == UiSearchAction::filters || a == UiSearchAction::previous || a == UiSearchAction::next
                 ? ui_text_center
                 : 0);
    }
    return out.ok();
}
} // namespace maicord
