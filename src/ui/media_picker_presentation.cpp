#include "maicord/ui/media_picker_presentation.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
UiRect widget_rectangle(const UiMediaPickerScene& scene, UiWidgetHandle widget) noexcept {
    const auto* node = ui_widget_manager_layout_node(scene.widgets, widget);
    return node ? node->rectangle : UiRect{};
}
bool active_widget(const UiMediaPickerScene& scene, UiWidgetHandle widget) noexcept {
    return (scene.widgets.hovered.slot == widget.slot &&
            scene.widgets.hovered.generation == widget.generation) ||
           (scene.widgets.captured.slot == widget.slot &&
            scene.widgets.captured.generation == widget.generation) ||
           (scene.widgets.focused.slot == widget.slot &&
            scene.widgets.focused.generation == widget.generation);
}
const UiMediaPickerCell* detail_cell(const UiMediaPickerScene& scene) noexcept {
    for (u32 i = 0; i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        if (scene.widgets.hovered.slot == cell.widget.slot &&
            scene.widgets.hovered.generation == cell.widget.generation)
            return &cell;
    }
    for (u32 i = 0; i < scene.cell_count; ++i)
        if (scene.projection->cells[i].source_index == scene.selected_index)
            return &scene.projection->cells[i];
    return nullptr;
}
bool text(UiTextOutput* output, const UiMediaPickerScene& scene, UiWidgetHandle owner, Utf8View value,
          UiRect rectangle, UiRect region, Color color, u32 flags = 0,
          UiTextAlignment alignment = UiTextAlignment::start,
          UiTextOverflow overflow = UiTextOverflow::ellipsis) noexcept {
    const auto clip = ui_rect_intersection(rectangle, region);
    if (!value.bytes || !ui_rect_has_area(clip))
        return true;
    return ui_text_output_push(output, {value,
                                        {&scene.widgets, owner},
                                        rectangle,
                                        clip,
                                        color,
                                        flags,
                                        alignment,
                                        UiTextAlignment::center,
                                        UiTextWrapping::no_wrap,
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
} // namespace
bool ui_media_picker_scene_paint(const UiMediaPickerScene& scene, const Theme& theme, PaintStream* paint,
                                 const UiEditGeometry* edit, u32 dpi) noexcept {
    if (!ui_media_picker_scene_current(scene))
        return true;
    const auto& style = scene.style;
    const auto& m = style.metrics;
    const auto panel = scene.panel;
    if (!ui_surface_shadow_paint_isolated(paint, panel, theme, UiElevation::popover, style.panel_radius) ||
        !paint_stream_push_clip(paint, panel.left, panel.top, panel.right, panel.bottom))
        return false;
    bool ok =
        ui_surface_paint_plate(paint, panel, theme, UiElevation::popover, style.panel_radius) &&
        paint_stream_record_content(paint, {&scene.widgets, scene.root}, panel,
                                    UiLayoutContentKind::control, ui_layout_content_bounds_allow_overlap);
    for (u32 i = 0; ok && i < 3; ++i) {
        const auto widget = scene.tab_widgets[i];
        if (!widget.generation)
            continue;
        const auto r = widget_rectangle(scene, widget);
        if (i == static_cast<u32>(scene.kind) || active_widget(scene, widget))
            ok = paint_stream_push_rounded(
                paint, r.left, r.top, r.right, r.bottom,
                ui_background_hover_color(
                    theme, i == static_cast<u32>(scene.kind) ? theme.surface_high : theme.surface, .28f),
                scene.style.control_radius);
        ok = ok &&
             paint_stream_record_content(paint, {&scene.widgets, widget}, r, UiLayoutContentKind::control,
                                         ui_layout_content_bounds_allow_overlap);
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
    ok = ok &&
         paint_stream_push(paint, panel.left, scene.body.top - 1, panel.right, scene.body.top,
                           theme.surface_high) &&
         paint_stream_push(paint, panel.left, scene.footer.top, panel.right, scene.footer.top + 1,
                           theme.surface_high);
    if (ok)
        ok = paint_stream_push_clip(paint, scene.body.left, scene.body.top, scene.body.right,
                                    scene.body.bottom);
    if (ok) {
        for (u32 i = 0; ok && i < scene.cell_count; ++i) {
            const auto& cell = scene.projection->cells[i];
            const auto r = cell.rectangle;
            const bool selected = active_widget(scene, cell.widget);
            const Color fill = selected ? ui_background_hover_color(theme, theme.surface_high, .28f)
                               : ui_background_active(theme) && scene.kind != UiMediaPickerKind::gif
                                   ? Color{}
                               : scene.kind == UiMediaPickerKind::sticker ? theme.base_lower
                                                                          : theme.base_low;
            ok = ui_background_paint_surface(
                     paint, r, theme, fill, selected ? theme.tokens.emphasis.outline : fill, scene.style.control_radius,
                     selected ? style.selection_width : 0, 1,
                     scene.kind == UiMediaPickerKind::gif && !selected ? UiBackgroundSurfaceMode::gradient
                                                                       : UiBackgroundSurfaceMode::tinted,
                     UiBackgroundTier::lower) &&
                 paint_stream_record_content(paint, {&scene.widgets, cell.widget}, cell.clip,
                                             UiLayoutContentKind::media,
                                             ui_layout_content_bounds_allow_overlap);
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
        for (u32 i = 0; ok && i < scene.header_count; ++i) {
            const auto& slot = scene.projection->headers[i];
            const auto r = slot.rectangle;
            const bool expanded = scene.source->sections[slot.section_index].expanded;
            const float y = (r.top + r.bottom) * .5f;
            ok = ui_icon_paint(paint, expanded ? UiIcon::chevron_down : UiIcon::chevron_right,
                               {r.right - m.icon_extent, y - m.icon_extent * .5f, r.right, y + m.icon_extent * .5f},
                               theme.text_secondary);
        }
        ok = paint_stream_pop_clip(paint) && ok;
    }
    if (scene.rail_count && !paint_stream_push_clip(paint, scene.rail.left, scene.rail.top,
                                                    scene.rail.right, scene.rail.bottom))
        return false;
    // Rail artwork is text (emoji glyphs, server acronyms) or server icon
    // media; only the hover/current-category plate is painted here.
    const u32 current = ui_media_picker_scene_current_section(scene);
    for (u32 i = 0; ok && i < scene.rail_count; ++i) {
        const auto& slot = scene.projection->rail[i];
        const auto r = slot.rectangle;
        const bool hovered = active_widget(scene, slot.widget);
        if (!hovered && slot.section_index != current)
            continue;
        ok = paint_stream_push_rounded(
            paint, r.left, r.top, r.right, r.bottom,
            ui_background_hover_color(theme, hovered ? theme.surface_high : theme.surface, .28f),
            scene.style.control_radius);
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
    if (!paint_stream_push_clip(paint, scene.body.left, scene.body.top, scene.body.right,
                                scene.body.bottom))
        return false;
    bool ok = true;
    for (u32 i = 0; ok && i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        if (!active_widget(scene, cell.widget))
            continue;
        const auto r = cell.rectangle;
        ok = paint_stream_push_rounded_border(paint, r.left, r.top, r.right, r.bottom, Color{0, 0, 0, 0},
                                              theme.tokens.emphasis.outline, scene.style.control_radius,
                                              scene.style.selection_width);
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
            if (!paint_stream_push_clip(paint, button_clip.left, button_clip.top, button_clip.right,
                                        button_clip.bottom)) {
                ok = false;
                break;
            }
            ok = paint_stream_push_rounded_border(
                     paint, button.left, button.top, button.right, button.bottom, theme.base_lower,
                     theme.interactive,
                     minimum(scene.style.control_radius,
                             minimum(ui_rect_width(button), ui_rect_height(button)) * .5f),
                     1) &&
                 paint_stream_record_content(paint, {&scene.widgets, cell.widget}, button_clip,
                                             UiLayoutContentKind::control,
                                             ui_layout_content_bounds_allow_overlap) &&
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
        r.right -= m.icon_extent + m.text_inset;
        if (!text(output, scene, slot.widget, scene.source->sections[slot.section_index].label, r,
                  scene.body, theme.text_primary, text_style_bold))
            return false;
    }
    for (u32 i = 0; i < scene.rail_count; ++i) {
        const auto& slot = scene.projection->rail[i];
        const auto& section = scene.source->sections[slot.section_index];
        UiMediaPickerRailMediaSlot media{};
        if (ui_media_picker_scene_rail_media(scene, i, &media))
            continue;
        char storage[16];
        const auto value = section.visual == UiMediaPickerSectionVisual::guild
                               ? rail_acronym(section.label, {storage, sizeof(storage)})
                               : ui_media_picker_section_glyph(section.visual);
        if (!text(output, scene, slot.widget, value, slot.rectangle, scene.rail, theme.text_primary,
                  section.visual == UiMediaPickerSectionVisual::guild ? text_style_bold : text_style_heading_2,
                  UiTextAlignment::center, UiTextOverflow::clip))
            return false;
    }
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
            const auto r = cell.rectangle;
            if (!text(output, scene, cell.widget, cell.item.glyph,
                      {r.left + m.emoji_inset, r.top + m.emoji_inset, r.right - m.emoji_inset,
                       r.bottom - m.emoji_inset},
                      cell.clip, theme.text_primary, text_style_picker_emoji, UiTextAlignment::center,
                      UiTextOverflow::clip))
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
    if (const auto* cell = detail_cell(scene)) {
        const auto r = scene.footer;
        if (!text(output, scene, scene.root, cell->item.label,
                  {r.left + m.padding, r.top + m.text_inset, r.right - m.padding,
                   r.top + m.text_inset + m.icon_extent},
                  r, theme.text_primary, text_style_bold) ||
            !text(output, scene, scene.root, cell->item.detail,
                  {r.left + m.padding, r.top + m.text_inset + m.icon_extent, r.right - m.padding,
                   r.bottom - m.text_inset},
                  r, theme.text_secondary, text_style_subtext))
            return false;
    } else if (!text(output, scene, scene.root, scene.content->status,
                     {scene.footer.left + m.padding, scene.footer.top + m.gap,
                      scene.footer.right - m.padding, scene.footer.bottom - m.gap},
                     scene.footer, theme.text_secondary, text_style_subtext))
        return false;
    return true;
}
} // namespace maicord
