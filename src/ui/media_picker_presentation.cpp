#include "maicord/ui/media_picker_presentation.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/scene_kit.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/theme_style_projection.hpp"

namespace maicord {
namespace {
UiRect widget_rectangle(const UiMediaPickerScene& scene, UiWidgetHandle widget) noexcept {
    const auto* node = ui_widget_manager_layout_node(scene.widgets, widget);
    return node ? node->rectangle : UiRect{};
}
bool active_widget(const UiMediaPickerScene& scene, UiWidgetHandle widget) noexcept {
    return ui_widget_handle_equal(scene.widgets.hovered, widget) ||
           ui_widget_handle_equal(scene.widgets.captured, widget) ||
           ui_widget_handle_equal(scene.widgets.focused, widget);
}
// Emoji quads are clipped to their rectangle on the CPU, so they share the region scissor.
void emoji_text(UiTextPen<>& text, const UiMediaPickerScene& scene, UiWidgetHandle owner, const Utf8View& value,
                const UiRect& rectangle, const UiRect& region, const Color& color, u32 flags) noexcept {
    if (value.bytes && ui_rect_intersects(rectangle, region))
        text.emit({value, {&scene.widgets, owner}, rectangle, region, color, flags, UiTextAlignment::center,
                   UiTextAlignment::center, TextWrapping::no_wrap, TextOverflow::clip, UiTextFace::normal, {}, {}});
}
// The server acronym: the first character of each word, at most three.
Utf8View rail_acronym(Utf8View name, Slice<char> storage) noexcept {
    u32 used = 0, characters = 0;
    bool word = false;
    for (u32 at = 0; at < name.bytes && characters < 3;) {
        const auto decoded = utf8_decode_one(name.data + at, name.bytes - at);
        if (!decoded.valid)
            break;
        const bool space = decoded.codepoint == ' ' || decoded.codepoint == '\t';
        if (!space && !word) {
            if (used + decoded.width > storage.count)
                break;
            copy_bytes(storage.data + used, name.data + at, decoded.width);
            used += decoded.width;
            ++characters;
        }
        word = !space;
        at += decoded.width;
    }
    return {storage.data, used};
}
// Grid emoji use Discord's medium or large size with the cell.
u32 emoji_style(const UiMediaPickerScene& scene) noexcept {
    return scene.cell_width - scene.style.metrics.emoji_padding * 2 > scene.style.metrics.emoji_extent
               ? text_style_picker_large_emoji
               : text_style_picker_grid_emoji;
}
// The visible grid: below a pinned section header.
UiRect grid_region(const UiMediaPickerScene& scene) noexcept {
    auto region = scene.body;
    region.top = maximum(region.top, scene.pinned_bottom);
    return region;
}
UiRect footer_text(const UiMediaPickerScene& scene) noexcept {
    const auto& m = scene.style.metrics;
    const auto r = scene.footer;
    return {r.left + m.padding + m.footer_art + m.gap, r.top, r.right - m.padding, r.bottom};
}
// Inline results: the popover plate, the loading spinner, the hovered and the
// keyboard-selected result's highlight,
// missing-art placeholders and the GIF strip's paging arrows.
bool paint_inline(const UiMediaPickerScene& scene, const Theme& theme, PaintStream* paint) noexcept {
    UiPaint p{paint, theme};
    p.popover(scene.panel, scene.style.panel_radius, false);
    if (scene.content->phase == UiMediaPickerPhase::loading) {
        const float x = (scene.body.left + scene.body.right) * .5f, half = kUiInlineResultsArrowExtent * .5f;
        const float top = scene.body.top + kUiInlineResultsPadding;
        p.spinner({x - half, top, x + half, top + kUiInlineResultsArrowExtent}, theme.text_secondary,
                  scene.spinner_phase);
    }
    p.clip(scene.body);
    const Color highlight = ui_background_hover_color(theme, theme.surface_high, .28f);
    const auto& m = scene.style.metrics;
    for (u32 i = 0; p && i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        if (ui_widget_handle_matches(scene.widgets.hovered, cell.widget) || cell.source_index == scene.selected_index)
            p.rounded(widget_rectangle(scene, cell.widget), highlight, theme.scale.radius_sm);
        if (!cell.item.enabled || !cell.item.asset_id) {
            const auto r = cell.rectangle;
            const float extent = minimum(m.icon_extent, ui_rect_height(r) * .5f);
            const float x = (r.left + r.right - extent) * .5f, y = (r.top + r.bottom - extent) * .5f;
            auto color = theme.text_secondary;
            color.alpha *= .65f;
            p.icon(UiIcon::image, {x, y, x + extent, y + extent}, color);
        }
    }
    return p.end();
}
// The GIF strip's paging arrows, over the tiles (the foreground pass follows
// the media). They sit on a dark overlay whatever the theme (as the image
// viewer's paging buttons): light ink, stronger fill while pointed at.
bool paint_inline_arrows(const UiMediaPickerScene& scene, const Theme& theme, PaintStream* paint) noexcept {
    const auto& m = scene.style.metrics;
    UiPaint p{paint, theme};
    p.clip(scene.body);
    for (u32 i = 0; p && i < 2; ++i) {
        const auto widget = scene.page_widgets[i];
        if (!widget.generation)
            continue;
        const auto r = widget_rectangle(scene, widget);
        const bool hot = ui_widget_handle_matches(scene.widgets.hovered, widget) ||
                         ui_widget_handle_matches(scene.widgets.captured, widget);
        const float extent = m.icon_extent, x = (r.left + r.right - extent) * .5f,
                    y = (r.top + r.bottom - extent) * .5f;
        p.rounded_border(r, hot ? theme.tokens.overlay.scrim_strong : theme.tokens.overlay.scrim_light,
                         theme.tokens.border.subtle, theme.scale.radius_md, 1)
            .icon(i ? UiIcon::chevron_right : UiIcon::chevron_left, {x, y, x + extent, y + extent},
                  kDefaultTheme.text_primary);
    }
    return p.end();
}
// The heading "<kind> matching **query**" (14 DIP, the query bold), then each
// sticker row's name (16 DIP) over its "Similar to:" line and, at the row's
// end, its pack or server (12 DIP).
bool text_inline(const UiMediaPickerScene& scene, const Theme& theme, UiTextOutput* output) noexcept {
    const float pad = kUiInlineResultsPadding;
    UiTextPen text{output, scene.widgets, scene.panel, scene.body};
    if (scene.content->phase == UiMediaPickerPhase::ready && scene.content->heading.bytes) {
        // The 18 DIP line centres in its 26 DIP block (4 DIP padding around it).
        const UiRect r{scene.panel.left + pad, scene.panel.top + pad, scene.panel.right - pad, scene.body.top - pad};
        text.emit({scene.content->heading,
                   {&scene.widgets, scene.root},
                   r,
                   ui_rect_intersection(r, scene.panel),
                   theme.text_secondary,
                   kTextStyleSizeCodeSm,
                   UiTextAlignment::start,
                   UiTextAlignment::center,
                   TextWrapping::no_wrap,
                   TextOverflow::ellipsis,
                   UiTextFace::normal,
                   {},
                   {&scene.heading_span, scene.heading_span.byte_count ? 1u : 0u}});
    }
    if (scene.kind != UiMediaPickerKind::sticker)
        return text.ok();
    for (u32 i = 0; i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        const auto row = widget_rectangle(scene, cell.widget);
        const float left = cell.rectangle.right + pad, right = row.right - pad;
        // The pack column ends the row; the name keeps the rest.
        const float detail = cell.item.detail.bytes
                                 ? minimum(kUiMenuDetailMaximumWidth, (right - left) * kUiMenuDetailWidthFraction)
                                 : 0;
        const float name_right = right - (detail ? detail + pad * 2 : 0);
        const float middle = (row.top + row.bottom) * .5f;
        const bool note = cell.item.note.bytes != 0;
        auto body = text.in(scene.body);
        body(cell.widget, cell.item.label,
             {left, note ? row.top + kUiComposerPadding : row.top, name_right, note ? middle + 2 : row.bottom},
             theme.text_primary);
        if (note)
            body(cell.widget, cell.item.note, {left, middle + 2, name_right, row.bottom - kUiComposerPadding},
                 theme.text_secondary, kTextStyleSizeCodeXs);
        if (detail)
            body(cell.widget, cell.item.detail, {right - detail, row.top, right, row.bottom}, theme.text_secondary,
                 kTextStyleSizeCodeXs, ui_text_end);
    }
    return text.ok();
}
} // namespace
bool ui_media_picker_scene_paint(const UiMediaPickerScene& scene, const Theme& theme, PaintStream* paint,
                                 const UiEditGeometry* edit, u32 dpi) noexcept {
    if (!ui_media_picker_scene_current(scene))
        return true;
    if (scene.variant == UiMediaPickerVariant::inline_results)
        return paint_inline(scene, theme, paint);
    const auto& style = scene.style;
    const auto& m = style.metrics;
    const auto panel = scene.panel;
    UiPaint p{paint, theme};
    p.popover(panel, style.panel_radius, false);
    // Discord's category list and inspector bar sit on the lower tier, as
    // inset cards with the composer's corner radius.
    const float card_radius = ui_theme_metrics(theme.styles).edit_radius;
    p.use<ui_background_paint_card>(scene.rail, theme, theme.base_lower, card_radius, UiBackgroundTier::lower)
        .use<ui_background_paint_card>(scene.footer, theme, theme.base_lower, card_radius, UiBackgroundTier::lower);
    for (u32 i = 0; p && i < 3; ++i) {
        const auto widget = scene.tab_widgets[i];
        if (!widget.generation)
            continue;
        const bool current_tab = i == static_cast<u32>(scene.kind);
        if (current_tab || active_widget(scene, widget))
            p.rounded(widget_rectangle(scene, widget),
                      ui_background_hover_color(theme, current_tab ? theme.surface_high : theme.surface, .28f),
                      scene.style.edit_radius);
    }
    p.use<ui_edit_field_paint>(scene.field, scene.text, scene.content->edit, edit, theme.base_lowest, theme, dpi,
                               style.edit_radius)
        .icon(UiIcon::search,
              {scene.text.left - m.gap - m.icon_extent, (scene.field.top + scene.field.bottom - m.icon_extent) * .5f,
               scene.text.left - m.gap, (scene.field.top + scene.field.bottom + m.icon_extent) * .5f},
              theme.text_secondary);
    if (scene.content->back) {
        const auto r = widget_rectangle(scene, scene.back);
        const float y = (r.top + r.bottom) * .5f;
        p.icon(UiIcon::chevron_left, {r.left, y - m.icon_extent * .5f, r.left + m.icon_extent, y + m.icon_extent * .5f},
               theme.text_secondary);
    }
    p.separator({panel.left, scene.body.top - 1, panel.right, scene.body.top}).clip(grid_region(scene));
    const bool gif = scene.kind == UiMediaPickerKind::gif;
    for (u32 i = 0; p && i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        const bool selected = active_widget(scene, cell.widget);
        // Emoji and stickers rest on the panel; the inspected cell is filled.
        if (!gif) {
            if (selected)
                p.rounded(cell.rectangle, ui_background_hover_color(theme, theme.surface_high, .28f),
                          scene.style.control_radius);
        } else {
            const Color fill = selected ? ui_background_hover_color(theme, theme.surface_high, .28f) : theme.base_low;
            p.use<ui_background_paint_surface>(cell.rectangle, theme, fill, selected ? theme.tokens.emphasis.outline : fill,
                                               scene.style.control_radius, selected ? style.selection_width : 0.f, 1.f,
                                               selected ? UiBackgroundSurfaceMode::tinted
                                                        : UiBackgroundSurfaceMode::gradient,
                                               UiBackgroundTier::lower, UiBackgroundBorderTone::semantic);
        }
    }
    for (u32 i = 0; p && i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        if (cell.item.enabled && (cell.item.asset_id || cell.item.glyph.bytes || cell.item.regional_flag))
            continue;
        // A labelled category already has a centered native affordance.
        // Its missing artwork must not put an image symbol under that ink.
        if (cell.item.enabled && cell.item.kind == UiMediaPickerItemKind::category && cell.item.label.bytes)
            continue;
        const auto r = cell.rectangle;
        const float extent = minimum(m.icon_extent, (ui_rect_height(r) - m.text_inset * 2) * .5f);
        const float left = (r.left + r.right - extent) * .5f;
        const float top = cell.item.enabled ? (r.top + r.bottom - extent) * .5f : r.top + m.text_inset;
        // Asset token zero is a neutral source placeholder, never a claim
        // about loading or GPU residency; unavailable items cross it out.
        auto color = theme.text_secondary;
        if (cell.item.enabled)
            color.alpha *= .65f;
        p.icon(cell.item.enabled ? UiIcon::image : UiIcon::image_off, {left, top, left + extent, top + extent}, color);
    }
    // Section headers: label after the list inset, chevron before the gutter.
    p.unclip().clip(scene.body);
    for (u32 i = 0; p && i < scene.header_count; ++i) {
        const auto& slot = scene.projection->headers[i];
        const auto r = slot.rectangle;
        const float y = (r.top + r.bottom) * .5f;
        const float right = r.right - m.scrollbar_gutter;
        p.icon(scene.source->sections[slot.section_index].expanded ? UiIcon::chevron_down : UiIcon::chevron_right,
               {right - m.icon_extent, y - m.icon_extent * .5f, right, y + m.icon_extent * .5f}, theme.text_secondary);
    }
    p.unclip();
    if (scene.rail_count)
        p.clip(scene.rail);
    // Rail artwork is text (emoji glyphs, server acronyms) or server icon
    // media; only the hover/current-category plate and the separator after the
    // last server are painted here.
    const u32 current = ui_media_picker_scene_current_section(scene);
    for (u32 i = 0; p && i < scene.rail_count; ++i) {
        const auto& slot = scene.projection->rail[i];
        const auto r = slot.rectangle;
        const auto visual = scene.source->sections[slot.section_index].visual;
        const bool server =
            visual == UiMediaPickerSectionVisual::guild || visual == UiMediaPickerSectionVisual::media;
        const u32 next = slot.section_index + 1u;
        if (server && next < scene.source->sections.count) {
            const auto following = scene.source->sections[next].visual;
            if (following != UiMediaPickerSectionVisual::guild && following != UiMediaPickerSectionVisual::media) {
                // 12 DIP below the item's 8 DIP spacing; the separator is 1 DIP.
                const float y = r.bottom + (m.rail_pitch - ui_rect_height(r)) + m.header_gap;
                p.separator({r.left, y, r.right, y + 1});
            }
        }
        const bool hovered = active_widget(scene, slot.widget);
        if (hovered || slot.section_index == current)
            p.rounded(r, ui_background_hover_color(theme, hovered ? theme.surface_high : theme.surface, .28f),
                      scene.style.control_radius);
    }
    if (scene.rail_count)
        p.unclip();
    p.scrollbar(ui_media_picker_scene_scrollbar(scene), scene.body_scrollbar_state)
        .use<paint_stream_push_focus>(scene.widgets, theme.tokens.border.focus);
    return p.end();
}
bool ui_media_picker_scene_paint_foreground(const UiMediaPickerScene& scene, const Theme& theme,
                                            PaintStream* paint) noexcept {
    if (!ui_media_picker_scene_current(scene))
        return true;
    // Inline results draw no tile outline or favorite button: only the GIF strip's arrows.
    if (scene.variant == UiMediaPickerVariant::inline_results)
        return paint_inline_arrows(scene, theme, paint);
    UiPaint p{paint, theme};
    p.clip(grid_region(scene));
    for (u32 i = 0; p && i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        if (!active_widget(scene, cell.widget) || cell.item.kind != UiMediaPickerItemKind::gif)
            continue;
        p.rounded_border(cell.rectangle, Color{}, theme.tokens.emphasis.outline, scene.style.control_radius,
                         scene.style.selection_width);
        UiRect button_clip{}, button{};
        if (p && ui_media_picker_scene_favorite(scene, i, &button_clip, &button)) {
            const auto& m = scene.style.metrics;
            const float extent = minimum(m.icon_extent, minimum(ui_rect_width(button), ui_rect_height(button)) - 4);
            const float x = (button.left + button.right) * .5f;
            const float y = (button.top + button.bottom) * .5f;
            auto color = cell.item.favorite ? theme.tokens.emphasis.outline : theme.text_primary;
            if (cell.item.favorite_pending)
                color.alpha *= .5f;
            // The star button nests in the tile's corner: concentric radius.
            p.clip(button_clip)
                .rounded_border(button, theme.base_lower, theme.interactive,
                                maximum(0.f, minimum(scene.style.control_radius - m.emoji_padding,
                                                     minimum(ui_rect_width(button), ui_rect_height(button)) * .5f)),
                                1)
                .use<ui_icon_paint_styled>(UiIcon::favorite,
                                           UiRect{x - extent * .5f, y - extent * .5f, x + extent * .5f, y + extent * .5f},
                                           UiIconStyle{color, cell.item.favorite ? color : Color{}, 0});
            if (cell.item.favorite_pending)
                p.use<paint_stream_push_line>(UiPoint{x - extent * .2f, button.bottom - 2},
                                              UiPoint{x + extent * .2f, button.bottom - 2}, 1.f, theme.text_secondary);
            else if (cell.item.favorite_failed)
                p.use<paint_stream_push_line>(UiPoint{button.right - 3, button.top + 3},
                                              UiPoint{button.right - 3, button.top + 3}, 3.f, theme.notification);
            p.unclip();
        }
    }
    return p.end();
}
bool ui_media_picker_scene_text_output(const UiMediaPickerScene& scene, const Theme& theme,
                                       UiTextOutput* output, const UiEditGeometry* edit) noexcept {
    if (!ui_media_picker_scene_current(scene))
        return true;
    if (scene.variant == UiMediaPickerVariant::inline_results)
        return text_inline(scene, theme, output);
    const auto& m = scene.style.metrics;
    const Utf8View labels[]{utf8_literal("GIFs"), utf8_literal("Stickers"), utf8_literal("Emoji")};
    UiTextPen text{output, scene.widgets, kUiNoRect};
    for (u32 i = 0; i < 3; ++i)
        if (scene.tab_widgets[i].generation)
            text.in(scene.tabs)(scene.tab_widgets[i], labels[i], widget_rectangle(scene, scene.tab_widgets[i]),
                                theme.text_primary, text_style_bold, ui_text_center);
    if (text.ok() && !ui_edit_field_text_output(output, {&scene.widgets, scene.edit}, scene.text, scene.content->edit,
                                                edit, scene.content->placeholder, theme))
        text.fail();
    for (u32 i = 0; i < scene.header_count; ++i) {
        const auto& slot = scene.projection->headers[i];
        auto r = slot.rectangle;
        r.left += m.list_inset;
        r.right -= m.scrollbar_gutter + m.icon_extent + m.text_inset;
        text.in(scene.body)(slot.widget, scene.source->sections[slot.section_index].label, r, theme.text_secondary,
                            text_style_bold);
    }
    for (u32 i = 0; i < scene.rail_count; ++i) {
        const auto& slot = scene.projection->rail[i];
        const auto& section = scene.source->sections[slot.section_index];
        UiMediaPickerRailMediaSlot media{};
        if (ui_media_picker_scene_rail_media(scene, i, &media))
            continue;
        char storage[16];
        if (section.visual == UiMediaPickerSectionVisual::guild || section.visual == UiMediaPickerSectionVisual::media)
            text.in(scene.rail)(slot.widget, rail_acronym(section.label, {storage, sizeof(storage)}), slot.rectangle,
                                theme.text_primary, text_style_bold, ui_text_center | ui_text_clip);
        else
            emoji_text(text, scene, slot.widget, ui_media_picker_section_glyph(section.visual), slot.rectangle,
                       scene.rail, theme.text_primary, text_style_picker_emoji);
    }
    const auto grid = grid_region(scene);
    const u32 glyph_style = emoji_style(scene);
    for (u32 i = 0; i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        if (!cell.item.enabled) {
            const auto r = cell.rectangle;
            const float glyph = minimum(m.icon_extent, (ui_rect_height(r) - m.text_inset * 2) * .5f);
            text.in(cell.clip)(cell.widget, utf8_literal("Unavailable"),
                               {r.left + m.text_inset, r.top + m.text_inset * 2 + glyph, r.right - m.text_inset,
                                r.bottom - m.text_inset},
                               theme.text_secondary, text_style_subtext, ui_text_center);
        } else if (cell.item.kind == UiMediaPickerItemKind::emoji && cell.item.glyph.bytes && !cell.item.regional_flag) {
            emoji_text(text, scene, cell.widget, cell.item.glyph, cell.rectangle, grid, theme.text_primary, glyph_style);
        } else if (cell.item.kind == UiMediaPickerItemKind::category) {
            text.in(cell.clip)(cell.widget, cell.item.label,
                               {cell.rectangle.left + m.gap, cell.rectangle.top, cell.rectangle.right - m.gap,
                                cell.rectangle.bottom},
                               theme.text_primary, text_style_bold, ui_text_center);
        }
    }
    if (scene.content->phase != UiMediaPickerPhase::ready || !scene.source || !scene.source->count)
        text.in(scene.body)(scene.root, scene.content->status,
                            {scene.body.left + m.padding + m.text_inset, scene.body.top + m.padding * 2,
                             scene.body.right - m.padding - m.text_inset, scene.body.top + m.padding * 2 + m.search_height},
                            theme.text_secondary, 0, ui_text_center);
    if (!ui_rect_has_area(scene.footer))
        return text.ok();
    // Discord's inspector: 28 DIP art, then ":name:" over "from <source>".
    const auto r = footer_text(scene);
    auto footer = text.in(scene.footer);
    if (const auto* cell = ui_media_picker_scene_inspected(scene)) {
        if (cell->item.kind == UiMediaPickerItemKind::emoji && cell->item.glyph.bytes && !cell->item.regional_flag) {
            const float left = scene.footer.left + m.padding;
            const float top = (scene.footer.top + scene.footer.bottom - m.footer_art) * .5f;
            emoji_text(text, scene, scene.root, cell->item.glyph, {left, top, left + m.footer_art, top + m.footer_art},
                       scene.footer, theme.text_primary, text_style_picker_emoji);
        }
        const auto title = ui_media_picker_scene_footer_title(scene);
        const auto detail = ui_media_picker_scene_footer_detail(scene);
        const float middle = (r.top + r.bottom) * .5f;
        footer(scene.root, title, detail.bytes ? UiRect{r.left, middle - m.icon_extent, r.right, middle} : r,
               theme.text_primary, text_style_bold)(scene.root, detail, {r.left, middle, r.right, middle + m.icon_extent},
                                                    theme.text_secondary, text_style_subtext);
    } else {
        footer(scene.root, scene.content->status,
               {scene.footer.left + m.padding, scene.footer.top, scene.footer.right - m.padding, scene.footer.bottom},
               theme.text_secondary, text_style_subtext);
    }
    return text.ok();
}
} // namespace maicord
