#include "maicord/ui/media_picker_presentation.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/spinner.hpp"
#include "maicord/ui/background_presentation.hpp"

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
bool text(UiTextOutput* output, const UiMediaPickerScene& scene, UiWidgetHandle owner, const Utf8View& value,
          const UiRect& rectangle, const UiRect& region, const Color& color, u32 flags = 0,
          UiTextAlignment alignment = UiTextAlignment::start,
          TextOverflow overflow = TextOverflow::ellipsis) noexcept {
    const auto clip = ui_rect_intersection(rectangle, region);
    if (!value.bytes || !ui_rect_has_area(clip))
        return true;
    // Emoji quads are clipped to rectangle on the CPU; share the region scissor.
    return ui_text_output_push(output, {value,
                                        {&scene.widgets, owner},
                                        rectangle,
                                        (flags & text_style_picker_emoji) ? region : clip,
                                        color,
                                        flags,
                                        alignment,
                                        UiTextAlignment::center,
                                        TextWrapping::no_wrap,
                                        overflow,
                                        UiTextFace::normal,
                                        {},
                                        {}});
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
    const auto panel = scene.panel;
    const float radius = scene.style.panel_radius;
    if (!ui_surface_shadow_paint_isolated(paint, panel, theme, UiElevation::popover, radius) ||
        !paint_stream_push_clip(paint, panel))
        return false;
    bool ok = ui_surface_paint_plate(paint, panel, theme, UiElevation::popover, radius);
    if (ok && scene.content->phase == UiMediaPickerPhase::loading) {
        const float x = (scene.body.left + scene.body.right) * .5f, half = kUiInlineResultsArrowExtent * .5f;
        const float top = scene.body.top + kUiInlineResultsPadding;
        ok = ui_spinner_paint(paint, {x - half, top, x + half, top + kUiInlineResultsArrowExtent},
                              theme.text_secondary, scene.spinner_phase);
    }
    ok = ok && paint_stream_push_clip(paint, scene.body);
    const Color highlight = ui_background_hover_color(theme, theme.surface_high, .28f);
    const auto& m = scene.style.metrics;
    for (u32 i = 0; ok && i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        if (ui_widget_handle_matches(scene.widgets.hovered, cell.widget) || cell.source_index == scene.selected_index)
            ok = paint_stream_push_rounded(paint, widget_rectangle(scene, cell.widget), highlight,
                                           theme.scale.radius_sm);
        if (ok && (!cell.item.enabled || !cell.item.asset_id)) {
            const auto r = cell.rectangle;
            const float extent = minimum(m.icon_extent, ui_rect_height(r) * .5f);
            const float x = (r.left + r.right - extent) * .5f, y = (r.top + r.bottom - extent) * .5f;
            auto color = theme.text_secondary;
            color.alpha *= .65f;
            ok = ui_icon_paint(paint, UiIcon::image, {x, y, x + extent, y + extent}, color);
        }
    }
    ok = paint_stream_pop_clip(paint) && ok;
    return paint_stream_pop_clip(paint) && ok;
}
// The GIF strip's paging arrows, over the tiles (the foreground pass follows
// the media). They sit on a dark overlay whatever the theme (as the image
// viewer's paging buttons): light ink, stronger fill while pointed at.
bool paint_inline_arrows(const UiMediaPickerScene& scene, const Theme& theme, PaintStream* paint) noexcept {
    const auto& m = scene.style.metrics;
    bool ok = paint_stream_push_clip(paint, scene.body);
    for (u32 i = 0; ok && i < 2; ++i) {
        const auto widget = scene.page_widgets[i];
        if (!widget.generation)
            continue;
        const auto r = widget_rectangle(scene, widget);
        const bool hot = ui_widget_handle_matches(scene.widgets.hovered, widget) ||
                         ui_widget_handle_matches(scene.widgets.captured, widget);
        const float extent = m.icon_extent, x = (r.left + r.right - extent) * .5f,
                    y = (r.top + r.bottom - extent) * .5f;
        ok = paint_stream_push_rounded_border(paint, r,
                                              hot ? theme.tokens.overlay.scrim_strong
                                                  : theme.tokens.overlay.scrim_light,
                                              theme.tokens.border.subtle, theme.scale.radius_md, 1) &&
             ui_icon_paint(paint, i ? UiIcon::chevron_right : UiIcon::chevron_left, {x, y, x + extent, y + extent},
                           kDefaultTheme.text_primary);
    }
    return paint_stream_pop_clip(paint) && ok;
}
// The heading "<kind> matching **query**" (14 DIP, the query bold), then each
// sticker row's name (16 DIP) over its "Similar to:" line and, at the row's
// end, its pack or server (12 DIP).
bool text_inline(const UiMediaPickerScene& scene, const Theme& theme, UiTextOutput* output) noexcept {
    const float pad = kUiInlineResultsPadding;
    if (scene.content->phase == UiMediaPickerPhase::ready && scene.content->heading.bytes) {
        // The 18 DIP line centres in its 26 DIP block (4 DIP padding around it).
        const UiRect r{scene.panel.left + pad, scene.panel.top + pad, scene.panel.right - pad, scene.body.top - pad};
        if (!ui_text_output_push(output, {scene.content->heading,
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
                                          {&scene.heading_span, scene.heading_span.byte_count ? 1u : 0u}}))
            return false;
    }
    if (scene.kind != UiMediaPickerKind::sticker)
        return true;
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
        if (!text(output, scene, cell.widget, cell.item.label,
                  {left, note ? row.top + kUiComposerPadding : row.top, name_right, note ? middle + 2 : row.bottom},
                  scene.body, theme.text_primary) ||
            (note && !text(output, scene, cell.widget, cell.item.note,
                           {left, middle + 2, name_right, row.bottom - kUiComposerPadding}, scene.body,
                           theme.text_secondary, kTextStyleSizeCodeXs)) ||
            (detail && !text(output, scene, cell.widget, cell.item.detail, {right - detail, row.top, right, row.bottom},
                             scene.body, theme.text_secondary, kTextStyleSizeCodeXs, UiTextAlignment::end)))
            return false;
    }
    return true;
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
    if (!ui_surface_shadow_paint_isolated(paint, panel, theme, UiElevation::popover, style.panel_radius) ||
        !paint_stream_push_clip(paint, panel))
        return false;
    bool ok =
        ui_surface_paint_plate(paint, panel, theme, UiElevation::popover, style.panel_radius) &&
        // Discord's category list and inspector bar sit on the lower tier.
        ui_background_paint_band(paint, scene.rail, panel, theme, theme.base_lower, style.panel_radius,
                                 UiBackgroundTier::lower) &&
        ui_background_paint_band(paint, scene.footer, panel, theme, theme.base_lower, style.panel_radius,
                                 UiBackgroundTier::lower);
    for (u32 i = 0; ok && i < 3; ++i) {
        const auto widget = scene.tab_widgets[i];
        if (!widget.generation)
            continue;
        const auto r = widget_rectangle(scene, widget);
        if (i == static_cast<u32>(scene.kind) || active_widget(scene, widget))
            ok = paint_stream_push_rounded(paint, r, ui_background_hover_color(
                theme, i == static_cast<u32>(scene.kind) ? theme.surface_high : theme.surface, .28f), scene.style.edit_radius);
    }
    ok = ok &&
         ui_edit_field_paint(paint, scene.field, scene.text, scene.content->edit, edit, theme.base_lowest,
                             theme, dpi, style.edit_radius) &&
         ui_icon_paint(paint, UiIcon::search,
                       {scene.text.left - m.gap - m.icon_extent,
                        (scene.field.top + scene.field.bottom - m.icon_extent) * .5f,
                        scene.text.left - m.gap,
                        (scene.field.top + scene.field.bottom + m.icon_extent) * .5f},
                       theme.text_secondary);
    if (ok && scene.content->back) {
        const auto r = widget_rectangle(scene, scene.back);
        const float y = (r.top + r.bottom) * .5f;
        ok = ui_icon_paint(paint, UiIcon::chevron_left,
                           {r.left, y - m.icon_extent * .5f, r.left + m.icon_extent, y + m.icon_extent * .5f},
                           theme.text_secondary);
    }
    ok = ok && paint_stream_push(paint, panel.left, scene.body.top - 1, panel.right, scene.body.top,
                                 theme.surface_high);
    if (ok && ui_rect_has_area(scene.footer))
        ok = paint_stream_push(paint, scene.footer.left, scene.footer.top, scene.footer.right,
                               scene.footer.top + 1, theme.surface_high);
    const auto grid = grid_region(scene);
    if (ok)
        ok = paint_stream_push_clip(paint, grid);
    if (ok) {
        const bool gif = scene.kind == UiMediaPickerKind::gif;
        for (u32 i = 0; ok && i < scene.cell_count; ++i) {
            const auto& cell = scene.projection->cells[i];
            const auto r = cell.rectangle;
            const bool selected = active_widget(scene, cell.widget);
            // Emoji and stickers rest on the panel; the inspected cell is filled.
            if (!gif) {
                if (selected)
                    ok = paint_stream_push_rounded(paint, r, ui_background_hover_color(theme, theme.surface_high, .28f), scene.style.control_radius);
            } else {
                const Color fill = selected ? ui_background_hover_color(theme, theme.surface_high, .28f)
                                            : theme.base_low;
                ok = ui_background_paint_surface(paint, r, theme, fill,
                                                 selected ? theme.tokens.emphasis.outline : fill,
                                                 scene.style.control_radius, selected ? style.selection_width : 0,
                                                 1,
                                                 selected ? UiBackgroundSurfaceMode::tinted
                                                          : UiBackgroundSurfaceMode::gradient,
                                                 UiBackgroundTier::lower);
            }
        }
        for (u32 i = 0; ok && i < scene.cell_count; ++i) {
            const auto& cell = scene.projection->cells[i];
            if (cell.item.enabled &&
                (cell.item.asset_id || cell.item.glyph.bytes || cell.item.regional_flag))
                continue;
            // A labelled category already has a centered native affordance.
            // Its missing artwork must not put an image symbol under that ink.
            if (cell.item.enabled && cell.item.kind == UiMediaPickerItemKind::category &&
                cell.item.label.bytes)
                continue;
            const auto r = cell.rectangle;
            const float extent = minimum(m.icon_extent, (ui_rect_height(r) - m.text_inset * 2) * .5f);
            const float left = (r.left + r.right - extent) * .5f;
            const float top = cell.item.enabled ? (r.top + r.bottom - extent) * .5f : r.top + m.text_inset;
            const UiRect glyph{left, top, left + extent, top + extent};
            // Asset token zero is a neutral source placeholder, never a claim
            // about loading or GPU residency; unavailable items cross it out.
            auto color = theme.text_secondary;
            if (cell.item.enabled)
                color.alpha *= .65f;
            ok = ui_icon_paint(paint, cell.item.enabled ? UiIcon::image : UiIcon::image_off, glyph, color);
        }
        ok = paint_stream_pop_clip(paint) && ok;
    }
    if (ok)
        ok = paint_stream_push_clip(paint, scene.body);
    if (ok) {
        // Section headers: label after the list inset, chevron before the gutter.
        for (u32 i = 0; ok && i < scene.header_count; ++i) {
            const auto& slot = scene.projection->headers[i];
            const auto r = slot.rectangle;
            const bool expanded = scene.source->sections[slot.section_index].expanded;
            const float y = (r.top + r.bottom) * .5f;
            const float right = r.right - m.scrollbar_gutter;
            ok = ui_icon_paint(paint, expanded ? UiIcon::chevron_down : UiIcon::chevron_right,
                               {right - m.icon_extent, y - m.icon_extent * .5f, right, y + m.icon_extent * .5f},
                               theme.text_secondary);
        }
        ok = paint_stream_pop_clip(paint) && ok;
    }
    if (scene.rail_count && !paint_stream_push_clip(paint, scene.rail))
        return false;
    // Rail artwork is text (emoji glyphs, server acronyms) or server icon
    // media; only the hover/current-category plate and the separator after the
    // last server are painted here.
    const u32 current = ui_media_picker_scene_current_section(scene);
    for (u32 i = 0; ok && i < scene.rail_count; ++i) {
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
                ok = paint_stream_push(paint, r.left, y, r.right, y + 1, theme.surface_high);
            }
        }
        const bool hovered = active_widget(scene, slot.widget);
        if (ok && (hovered || slot.section_index == current))
            ok = paint_stream_push_rounded(paint, r, ui_background_hover_color(theme, hovered ? theme.surface_high : theme.surface, .28f), scene.style.control_radius);
    }
    if (scene.rail_count)
        ok = paint_stream_pop_clip(paint) && ok;
    ok = ok &&
         ui_scrollbar_paint(paint, ui_media_picker_scene_scrollbar(scene), theme,
                            scene.body_scrollbar_state) &&
         paint_stream_push_focus(paint, scene.widgets, theme.tokens.border.focus);
    return paint_stream_pop_clip(paint) && ok;
}
bool ui_media_picker_scene_paint_foreground(const UiMediaPickerScene& scene, const Theme& theme,
                                            PaintStream* paint) noexcept {
    if (!ui_media_picker_scene_current(scene))
        return true;
    // Inline results draw no tile outline or favorite button: only the GIF strip's arrows.
    if (scene.variant == UiMediaPickerVariant::inline_results)
        return paint_inline_arrows(scene, theme, paint);
    const auto grid = grid_region(scene);
    if (!paint_stream_push_clip(paint, grid))
        return false;
    bool ok = true;
    for (u32 i = 0; ok && i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        if (!active_widget(scene, cell.widget) || cell.item.kind != UiMediaPickerItemKind::gif)
            continue;
        const auto r = cell.rectangle;
        ok = paint_stream_push_rounded_border(paint, r, Color{0, 0, 0, 0}, theme.tokens.emphasis.outline, scene.style.control_radius, scene.style.selection_width);
        UiRect button_clip{}, button{};
        if (ok && ui_media_picker_scene_favorite(scene, i, &button_clip, &button)) {
            const auto& m = scene.style.metrics;
            const float extent =
                minimum(m.icon_extent, minimum(ui_rect_width(button), ui_rect_height(button)) - 4);
            const float x = (button.left + button.right) * .5f;
            const float y = (button.top + button.bottom) * .5f;
            auto color = cell.item.favorite ? theme.tokens.emphasis.outline : theme.text_primary;
            if (cell.item.favorite_pending)
                color.alpha *= .5f;
            if (!paint_stream_push_clip(paint, button_clip)) {
                ok = false;
                break;
            }
            // The star button nests in the tile's corner: concentric radius.
            ok = paint_stream_push_rounded_border(paint, button, theme.base_lower, theme.interactive, maximum(0.f, minimum(scene.style.control_radius - m.emoji_padding,
                                 minimum(ui_rect_width(button), ui_rect_height(button)) * .5f)), 1) &&
                 ui_icon_paint_styled(paint, UiIcon::favorite,
                                      {x - extent * .5f, y - extent * .5f, x + extent * .5f, y + extent * .5f},
                                      {color, cell.item.favorite ? color : Color{}, 0});
            if (ok && cell.item.favorite_pending)
                ok = paint_stream_push_line(paint, {x - extent * .2f, button.bottom - 2},
                                            {x + extent * .2f, button.bottom - 2}, 1, theme.text_secondary);
            else if (ok && cell.item.favorite_failed)
                ok = paint_stream_push_line(paint, {button.right - 3, button.top + 3},
                                            {button.right - 3, button.top + 3}, 3, theme.notification);
            ok = paint_stream_pop_clip(paint) && ok;
        }
    }
    return paint_stream_pop_clip(paint) && ok;
}
bool ui_media_picker_scene_text_output(const UiMediaPickerScene& scene, const Theme& theme,
                                       UiTextOutput* output, const UiEditGeometry* edit) noexcept {
    if (!ui_media_picker_scene_current(scene))
        return true;
    if (scene.variant == UiMediaPickerVariant::inline_results)
        return text_inline(scene, theme, output);
    const auto& m = scene.style.metrics;
    const Utf8View labels[]{utf8_literal("GIFs"), utf8_literal("Stickers"), utf8_literal("Emoji")};
    for (u32 i = 0; i < 3; ++i)
        if (scene.tab_widgets[i].generation &&
            !text(output, scene, scene.tab_widgets[i], labels[i],
                  widget_rectangle(scene, scene.tab_widgets[i]), scene.tabs, theme.text_primary,
                  text_style_bold, UiTextAlignment::center))
            return false;
    if (!ui_edit_field_text_output(output, {&scene.widgets, scene.edit}, scene.text, scene.content->edit,
                                   edit, scene.content->placeholder, theme))
        return false;
    for (u32 i = 0; i < scene.header_count; ++i) {
        const auto& slot = scene.projection->headers[i];
        auto r = slot.rectangle;
        r.left += m.list_inset;
        r.right -= m.scrollbar_gutter + m.icon_extent + m.text_inset;
        if (!text(output, scene, slot.widget, scene.source->sections[slot.section_index].label, r,
                  scene.body, theme.text_secondary, text_style_bold))
            return false;
    }
    for (u32 i = 0; i < scene.rail_count; ++i) {
        const auto& slot = scene.projection->rail[i];
        const auto& section = scene.source->sections[slot.section_index];
        UiMediaPickerRailMediaSlot media{};
        if (ui_media_picker_scene_rail_media(scene, i, &media))
            continue;
        char storage[16];
        const bool initials = section.visual == UiMediaPickerSectionVisual::guild ||
                              section.visual == UiMediaPickerSectionVisual::media;
        const auto value = initials ? rail_acronym(section.label, {storage, sizeof(storage)})
                                    : ui_media_picker_section_glyph(section.visual);
        if (!text(output, scene, slot.widget, value, slot.rectangle, scene.rail, theme.text_primary,
                  initials ? text_style_bold : text_style_picker_emoji, UiTextAlignment::center,
                  TextOverflow::clip))
            return false;
    }
    const auto grid = grid_region(scene);
    const u32 glyph_style = emoji_style(scene);
    for (u32 i = 0; i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        if (!cell.item.enabled) {
            const auto r = cell.rectangle;
            const float glyph = minimum(m.icon_extent, (ui_rect_height(r) - m.text_inset * 2) * .5f);
            if (!text(output, scene, cell.widget, utf8_literal("Unavailable"),
                      {r.left + m.text_inset, r.top + m.text_inset * 2 + glyph, r.right - m.text_inset,
                       r.bottom - m.text_inset},
                      cell.clip, theme.text_secondary, text_style_subtext, UiTextAlignment::center))
                return false;
        } else if (cell.item.kind == UiMediaPickerItemKind::emoji && cell.item.glyph.bytes &&
                   !cell.item.regional_flag) {
            if (!text(output, scene, cell.widget, cell.item.glyph, cell.rectangle,
                      grid, theme.text_primary, glyph_style,
                      UiTextAlignment::center, TextOverflow::clip))
                return false;
        } else if (cell.item.kind == UiMediaPickerItemKind::category &&
                   !text(output, scene, cell.widget, cell.item.label,
                         {cell.rectangle.left + m.gap, cell.rectangle.top, cell.rectangle.right - m.gap,
                          cell.rectangle.bottom},
                         cell.clip, theme.text_primary, text_style_bold, UiTextAlignment::center))
            return false;
    }
    if (scene.content->phase != UiMediaPickerPhase::ready || !scene.source || !scene.source->count) {
        if (!text(output, scene, scene.root, scene.content->status,
                  {scene.body.left + m.padding + m.text_inset, scene.body.top + m.padding * 2,
                   scene.body.right - m.padding - m.text_inset,
                   scene.body.top + m.padding * 2 + m.search_height},
                  scene.body, theme.text_secondary, 0, UiTextAlignment::center))
            return false;
    }
    if (!ui_rect_has_area(scene.footer))
        return true;
    // Discord's inspector: 28 DIP art, then ":name:" over "from <source>".
    const auto r = footer_text(scene);
    if (const auto* cell = ui_media_picker_scene_inspected(scene)) {
        if (cell->item.kind == UiMediaPickerItemKind::emoji && cell->item.glyph.bytes &&
            !cell->item.regional_flag) {
            const float left = scene.footer.left + m.padding;
            const float top = (scene.footer.top + scene.footer.bottom - m.footer_art) * .5f;
            if (!text(output, scene, scene.root, cell->item.glyph, {left, top, left + m.footer_art, top + m.footer_art},
                      scene.footer, theme.text_primary, text_style_picker_emoji, UiTextAlignment::center,
                      TextOverflow::clip))
                return false;
        }
        const auto title = ui_media_picker_scene_footer_title(scene);
        const auto detail = ui_media_picker_scene_footer_detail(scene);
        const float middle = (r.top + r.bottom) * .5f;
        return text(output, scene, scene.root, title,
                    detail.bytes ? UiRect{r.left, middle - m.icon_extent, r.right, middle} : r, scene.footer,
                    theme.text_primary, text_style_bold) &&
               text(output, scene, scene.root, detail, {r.left, middle, r.right, middle + m.icon_extent},
                    scene.footer, theme.text_secondary, text_style_subtext);
    }
    return text(output, scene, scene.root, scene.content->status,
                {scene.footer.left + m.padding, scene.footer.top, scene.footer.right - m.padding,
                 scene.footer.bottom},
                scene.footer, theme.text_secondary, text_style_subtext);
}
} // namespace maicord
