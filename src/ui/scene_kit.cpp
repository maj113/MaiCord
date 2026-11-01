#include "maicord/ui/scene_kit.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/ui/rect_edges.hpp"
#include "maicord/ui/spinner.hpp"
#include "maicord/ui/surface_elevation.hpp"

namespace maicord {
template <UiTextClip Policy>
UiTextPen<Policy>& UiTextPen<Policy>::push(const UiRect& region, UiWidgetHandle owner, const Utf8View& value,
                                           const UiRect& r, const Color& color, u32 style, u32 mode) noexcept {
    if (!ok_ || !value.bytes)
        return *this;
    UiRect clip = region;
    if constexpr (Policy == UiTextClip::intersect) {
        clip = ui_rect_intersection(r, region);
        if (!ui_rect_has_area(clip))
            return *this;
    } else if constexpr (Policy == UiTextClip::region) {
        if (!ui_rect_has_area(r))
            return *this;
    } else if (!ui_rect_intersects(r, region)) {
        return *this;
    }
    const bool wrap = mode & ui_text_wrap;
    ok_ = ui_text_output_push(out_, {value, {widgets_, owner}, r, clip, color, style,
                                     static_cast<UiTextAlignment>((mode >> 2) & 3),
                                     wrap && !(mode & ui_text_middle) ? UiTextAlignment::start : UiTextAlignment::center,
                                     wrap ? TextWrapping::wrap : TextWrapping::no_wrap,
                                     (mode & ui_text_clip) ? TextOverflow::clip : TextOverflow::ellipsis,
                                     UiTextFace::normal});
    return *this;
}
template class UiTextPen<UiTextClip::intersect>;
template class UiTextPen<UiTextClip::region>;
template class UiTextPen<UiTextClip::label>;

UiPaint& UiPaint::look(const UiRect& r, const UiLook& look, u8 state) noexcept {
    if (!ok_ || ((look.flags & ui_look_hot_only) && !state))
        return *this;
    const UiToken hover = look.hover == kUiTokenNone ? look.fill : look.hover;
    const UiToken selected = look.selected == kUiTokenNone ? hover : look.selected;
    const UiToken border = look.border == kUiTokenNone ? look.fill : look.border;
    const UiToken selected_border = look.selected_border == kUiTokenNone ? border : look.selected_border;
    const UiToken fill = (state & ui_state_selected) ? selected : (state & ui_state_hovered) ? hover : look.fill;
    const UiToken edge = (state & ui_state_focused)    ? look.focus
                         : (state & ui_state_selected) ? selected_border
                                                       : border;
    const float radius = (look.flags & ui_look_pill)        ? ui_rect_height(r) * .5f
                         : (look.flags & ui_look_radius_sm) ? theme_->scale.radius_sm
                         : (look.flags & ui_look_radius_md) ? theme_->scale.radius_md
                                                            : look.radius;
    if ((look.flags & ui_look_selected_emphasis) && (state & ui_state_selected))
        return surface(r, ui_emphasis_selected(*theme_, UiElevation::popover), ui_token(*theme_, edge), radius);
    return surface(r, ui_token(*theme_, fill), ui_token(*theme_, edge), radius, look.flags & ui_look_dialog);
}
UiPaint& UiPaint::control(const UiWidgetManager& widgets, UiWidgetHandle handle, const UiLook& look,
                          bool selected) noexcept {
    if (!handle.generation)
        return *this;
    return this->look(ui_widget_manager_rectangle(widgets, handle), look,
                      static_cast<u8>(ui_widget_state(widgets, handle) | (selected ? ui_state_selected : 0)));
}
bool UiPaint::plate(PaintStream* out, const Theme& theme, const UiRect& r, const Color& fill, const Color& border,
                    float radius, bool dialog) noexcept {
    return ui_background_paint_surface(out, r, theme, fill, border, radius, 1, dialog ? 1.f : .9f,
                                       UiBackgroundSurfaceMode::gradient,
                                       dialog ? UiBackgroundTier::highest : UiBackgroundTier::high);
}
bool UiPaint::popover_frame(PaintStream* out, const Theme& theme, const UiRect& panel, float radius,
                            bool bordered) noexcept {
    if (!ui_surface_shadow_paint_isolated(out, panel, theme, UiElevation::popover, radius) ||
        !paint_stream_push_clip(out, panel))
        return false;
    if (ui_surface_paint_plate(out, panel, theme, UiElevation::popover, radius, nullptr,
                               bordered ? &theme.surface_high : nullptr))
        return true;
    paint_stream_pop_clip(out);
    return false;
}
bool UiPaint::dialog_frame(PaintStream* out, const Theme& theme, const UiRect& viewport, const UiRect& panel) noexcept {
    if (!paint_stream_push_clip(out, viewport))
        return false;
    if (paint_stream_push(out, viewport, theme.tokens.overlay.scrim) &&
        ui_surface_paint(out, panel, theme, UiElevation::dialog, theme.scale.radius_lg + theme.scale.space_1, nullptr,
                         &theme.interactive_hover))
        return true;
    paint_stream_pop_clip(out);
    return false;
}

UiLabelWriter& UiLabelWriter::operator<<(Utf8View text) noexcept {
    if (*bytes_ + text.bytes <= capacity_) {
        copy_bytes(data_ + *bytes_, text.data, text.bytes);
        *bytes_ = static_cast<u8>(*bytes_ + text.bytes);
    }
    return *this;
}
UiLabelWriter& UiLabelWriter::operator<<(u64 value) noexcept {
    if (*bytes_ + decimal_digits(value) <= capacity_)
        *bytes_ = static_cast<u8>(*bytes_ + decimal_write(data_ + *bytes_, value));
    return *this;
}

bool ui_scene_open(UiFixedWidgetStorageOwner* storage, UiWidgetManager* widgets, u32 capacity, u32 sidecar_bytes,
                   u32 sidecar_alignment) noexcept {
    if (!ui_fixed_widget_storage_initialize(storage, capacity, sidecar_bytes, sidecar_alignment))
        return false;
    if (ui_widget_manager_initialize(widgets, storage->widgets, storage->layout_nodes))
        return true;
    ui_fixed_widget_storage_release(storage);
    return false;
}
UiWidgetHandle ui_scene_root(UiWidgetManager* widgets, Utf8View name) noexcept {
    ui_widget_manager_reset(widgets);
    const auto style =
        ui_layout_fill_style(ui_layout_node_visible | ui_layout_node_allow_overlap | ui_layout_node_clip_children);
    const auto root = ui_widget_manager_add_prevalidated(widgets, ui_widget_invalid_handle(),
                                                         {1, UiWidgetKind::container, ui_widget_none, {name, style}});
    return root.ok() ? root.handle : UiWidgetHandle{};
}
UiWidgetHandle ui_scene_part(UiWidgetManager* widgets, UiWidgetHandle parent, UiWidgetKind kind,
                             Utf8View name) noexcept {
    const auto part = ui_widget_manager_add_part(widgets, parent, kind, name);
    return part.ok() ? part.handle : UiWidgetHandle{};
}
UiWidgetHandle ui_scene_part_root(UiWidgetManager* widgets, Utf8View name) noexcept {
    const auto root = ui_scene_part(widgets, ui_widget_invalid_handle(), UiWidgetKind::container, name);
    if (auto* node = ui_widget_manager_layout_node(widgets, root))
        node->spec.style.width = node->spec.style.height = ui_layout_fill();
    return root;
}
bool ui_scene_hover(UiWidgetManager* widgets, u32* revision, float x, float y, bool* changed) noexcept {
    if (!changed || !ui_widget_manager_hover_at(widgets, x, y, changed))
        return false;
    if (*changed)
        ++*revision;
    return true;
}

} // namespace maicord
