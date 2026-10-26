#pragma once
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/surface_geometry.hpp"

namespace maicord {
// File cards, and the inset of their Save action from the card's edge.
inline constexpr float kUiAttachmentFileCardRadius = kUiControlCornerRadius, kUiAttachmentFileSaveInset = 4;
// Save action geometry shared by paint and input. It reuses the attachment's
// widget instead of adding one per row.
inline UiRect ui_attachment_save_rectangle(UiRect attachment, bool file_card) noexcept {
    if (file_card) {
        attachment.right = minimum(attachment.right, attachment.left + 420.f);
        attachment.bottom = minimum(attachment.bottom, attachment.top + 36.f);
    }
    const float inset = file_card ? kUiAttachmentFileSaveInset : 8.f;
    const float size = file_card ? 28.f : 30.f;
    if (ui_rect_width(attachment) < size + inset * 2 ||
        ui_rect_height(attachment) < size + inset * 2)
        return {};
    return {attachment.right - inset - size, attachment.top + inset,
            attachment.right - inset, attachment.top + inset + size};
}
// Virtual child action: the attachment widget owns its hover/focus and content
// bounds. The caller passes the same action rectangle and clip that input uses,
// and may expose the action while the containing media row is active.
inline bool ui_attachment_save_paint(PaintStream* paint, UiWidgetReference owner,
    UiRect rectangle, UiRect clip, const Theme& theme, bool file_card,
    bool parent_active = false) noexcept {
    if (!paint || !ui_rect_valid_or_empty(rectangle) || !ui_rect_valid_or_empty(clip)) return false;
    if (!ui_rect_has_area(rectangle) || !ui_rect_has_area(clip)) return true;
    if (!owner.manager || !ui_widget_manager_input_enabled(*owner.manager,owner.handle)) return true;
    const auto same = [](UiWidgetHandle a, UiWidgetHandle b) noexcept {
        return a.generation && a.slot == b.slot && a.generation == b.generation;
    };
    const auto& manager = *owner.manager;
    const bool hover = same(manager.hovered,owner.handle);
    const bool focus = same(manager.focused,owner.handle);
    if (!file_card && !parent_active && !hover && !focus) return true;
    const auto* node = ui_widget_manager_layout_node(manager,owner.handle);
    if (!node) return true;
    clip = ui_rect_intersection(clip,node->clip_rectangle);
    const auto visible = ui_rect_intersection(rectangle,clip);
    if (!ui_rect_has_area(visible)) return true;
    if (!paint_stream_push_clip(paint, clip)) return false;
    const auto border = focus ? ui_foreground_color(theme,theme.accent) : theme.interactive;
    const auto fill = hover || parent_active ? theme.surface_high : theme.base_lowest;
    const float size = minimum(20.f,minimum(ui_rect_width(rectangle),ui_rect_height(rectangle)) - 8.f);
    const float x = (rectangle.left + rectangle.right - size) * .5f;
    const float y = (rectangle.top + rectangle.bottom - size) * .5f;
    const UiRect icon{x,y,x + size,y + size};
    // On a file card it follows the card's corner; on media it matches the GIF star.
    const float radius = file_card ? ui_inner_radius(kUiAttachmentFileCardRadius, kUiAttachmentFileSaveInset)
                                   : theme.scale.radius_sm;
    bool ok = ui_background_paint_surface(paint,rectangle,theme,fill,border,radius,1,1,
        UiBackgroundSurfaceMode::gradient,UiBackgroundTier::highest) &&
        paint_stream_record_content(paint,owner,visible,UiLayoutContentKind::control,
            ui_layout_content_bounds_allow_overlap) &&
        ui_icon_paint(paint,UiIcon::download,icon,ui_foreground_color(theme,theme.text_primary));
    const auto visible_icon = ui_rect_intersection(icon,clip);
    if (ok && ui_rect_has_area(visible_icon)) ok = paint_stream_record_content(paint,owner,visible_icon,
        UiLayoutContentKind::icon,ui_layout_content_bounds_allow_overlap);
    return paint_stream_pop_clip(paint) && ok;
}
// A GIF preview's favourite star, top right: left of the Save action on an
// attachment, in its place on an embed. Shared by paint and input.
inline UiRect ui_gif_favorite_rectangle(UiRect media, bool beside_save) noexcept {
    const UiRect save = ui_attachment_save_rectangle(media, false);
    if (!ui_rect_has_area(save) || !beside_save)
        return save;
    const float shift = ui_rect_width(save) + 4.f;
    if (save.left - shift < media.left + 8.f)
        return {};
    return {save.left - shift, save.top, save.right - shift, save.bottom};
}
// Shown while the media widget is hovered or focused. A saved GIF draws a
// filled star; a change in flight dims it.
inline bool ui_gif_favorite_paint(PaintStream* paint, UiWidgetReference owner, UiRect rectangle, UiRect clip,
                                  const Theme& theme, bool favorite, bool pending) noexcept {
    if (!paint || !ui_rect_valid_or_empty(rectangle) || !ui_rect_valid_or_empty(clip)) return false;
    if (!ui_rect_has_area(rectangle) || !ui_rect_has_area(clip)) return true;
    if (!owner.manager || !ui_widget_manager_input_enabled(*owner.manager, owner.handle)) return true;
    const auto same = [](UiWidgetHandle a, UiWidgetHandle b) noexcept {
        return a.generation && a.slot == b.slot && a.generation == b.generation;
    };
    const auto& manager = *owner.manager;
    if (!same(manager.hovered, owner.handle) && !same(manager.focused, owner.handle)) return true;
    const auto* node = ui_widget_manager_layout_node(manager, owner.handle);
    if (!node) return true;
    clip = ui_rect_intersection(clip, node->clip_rectangle);
    const auto visible = ui_rect_intersection(rectangle, clip);
    if (!ui_rect_has_area(visible)) return true;
    if (!paint_stream_push_clip(paint, clip)) return false;
    const float size = minimum(20.f, minimum(ui_rect_width(rectangle), ui_rect_height(rectangle)) - 8.f);
    const float x = (rectangle.left + rectangle.right - size) * .5f;
    const float y = (rectangle.top + rectangle.bottom - size) * .5f;
    auto color = favorite ? theme.tokens.emphasis.outline : ui_foreground_color(theme, theme.text_primary);
    if (pending)
        color.alpha *= .5f;
    bool ok = ui_background_paint_surface(paint, rectangle, theme, theme.surface_high, theme.interactive,
                                          theme.scale.radius_sm, 1, 1, UiBackgroundSurfaceMode::gradient,
                                          UiBackgroundTier::highest) &&
              paint_stream_record_content(paint, owner, visible, UiLayoutContentKind::control,
                                          ui_layout_content_bounds_allow_overlap) &&
              ui_icon_paint_styled(paint, UiIcon::favorite, {x, y, x + size, y + size},
                                   {color, favorite ? color : Color{}, 0});
    return paint_stream_pop_clip(paint) && ok;
}
} // namespace maicord
