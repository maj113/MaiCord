#include "maicord/ui/message_action_bar_presentation.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/plugin_slots.hpp"

namespace maicord {
namespace {

bool push_owned(PaintStream* paint, const MessageActionBar& bar, UiRect rectangle, const Color& fill,
                const Color& border, float radius, float border_width, UiLayoutContentKind kind) noexcept {
    return paint_stream_push_rounded_border(paint, rectangle.left, rectangle.top, rectangle.right,
                                            rectangle.bottom, fill, border, radius, border_width) &&
           paint_stream_record_content(paint, bar.owner, rectangle, kind,
                                       ui_layout_content_bounds_allow_overlap |
                                           ui_layout_content_bounds_allow_overflow);
}

bool paint_shared_icon(PaintStream* paint, const MessageActionBar& bar, UiRect cell, const Color& color,
                       UiIcon icon) noexcept {
    const float x = (cell.left + cell.right) * .5f, y = (cell.top + cell.bottom) * .5f;
    const UiRect slot{x - 10, y - 10, x + 10, y + 10};
    return ui_icon_paint(paint, icon, slot, color) &&
           paint_stream_record_content(paint, bar.owner, slot, UiLayoutContentKind::icon,
                                       ui_layout_content_bounds_allow_overlap);
}
bool paint_add_reaction(PaintStream* paint, const MessageActionBar& bar, UiRect cell,
                        const Color& color) noexcept {
    return paint_shared_icon(paint, bar, cell, color, UiIcon::add_reaction);
}
bool paint_reply(PaintStream* paint, const MessageActionBar& bar, UiRect cell,
                 const Color& color) noexcept {
    return paint_shared_icon(paint, bar, cell, color, UiIcon::reply);
}

bool paint_more(PaintStream* paint, const MessageActionBar& bar, UiRect cell, const Color& color) noexcept {
    return paint_shared_icon(paint, bar, cell, color, UiIcon::ellipsis);
}

} // namespace

bool message_action_bar_presentation_build(const MessageActionBar& bar, const Theme& theme,
                                           PaintStream* paint) noexcept {
    if (!bar.active) {
        return true;
    }
    // Clipped to the timeline viewport minus the overlays above it (unread
    // banner, jump pill), so the toolbar slides under them like the official client's.
    const UiRect visible = message_action_bar_visible_rectangle(bar);
    if (!paint || bar.message_id == 0 || bar.item_count == 0 || bar.item_count > kMessageActionBarCapacity ||
        !bar.owner.manager || !ui_rect_has_area(bar.rectangle) || !ui_rect_has_area(visible) ||
        !paint_stream_push_clip(paint, visible.left, visible.top, visible.right, visible.bottom)) {
        return false;
    }
    UiPluginSlotNode plugins[kMessageActionBarPluginItems]{};
    const u32 plugin_count = ui_plugin_slot_toolbar(kUiPluginSlotMessageHover, {plugins, kMessageActionBarPluginItems});
    bool success =
        // The hover toolbar floats over the timeline like any popover.
        ui_surface_paint_plate(paint, bar.rectangle, theme, UiElevation::popover, theme.scale.radius_sm) &&
        paint_stream_record_content(paint, bar.owner, bar.rectangle, UiLayoutContentKind::control,
                                    ui_layout_content_bounds_allow_overlap |
                                        ui_layout_content_bounds_allow_overflow);
    for (u32 index = 0; success && index < bar.item_count; ++index) {
        const MessageActionBarItem& item = bar.items[index];
        if (bar.hovered_index == index) {
            const Color hover = ui_background_hover_color(theme, theme.surface_high);
            success = push_owned(paint, bar, item.rectangle, hover, hover, 4.0f, 0.0f,
                                 UiLayoutContentKind::control);
        }
        if (!success) {
            break;
        }
        switch (item.kind) {
        case MessageActionBarItemKind::quick_reaction:
            break;
        case MessageActionBarItemKind::add_reaction:
            success = paint_add_reaction(paint, bar, item.rectangle, theme.text_primary);
            break;
        case MessageActionBarItemKind::reply:
            success = paint_reply(paint, bar, item.rectangle, theme.text_primary);
            break;
        case MessageActionBarItemKind::forward:
            success = paint_shared_icon(paint, bar, item.rectangle, theme.text_primary, UiIcon::forwarded);
            break;
        case MessageActionBarItemKind::more:
            success = paint_more(paint, bar, item.rectangle, theme.text_primary);
            break;
        case MessageActionBarItemKind::edit:
            success = paint_shared_icon(paint, bar, item.rectangle, theme.text_primary, UiIcon::edit);
            break;
        case MessageActionBarItemKind::copy_link:
            success = paint_shared_icon(paint, bar, item.rectangle, theme.text_primary, UiIcon::link);
            break;
        case MessageActionBarItemKind::mark_unread:
            success = paint_shared_icon(paint, bar, item.rectangle, theme.text_primary, UiIcon::eye_off);
            break;
        case MessageActionBarItemKind::pin:
            success = paint_shared_icon(paint, bar, item.rectangle, theme.text_primary, UiIcon::pin);
            break;
        case MessageActionBarItemKind::unpin:
            success = paint_shared_icon(paint, bar, item.rectangle, theme.text_primary, UiIcon::unpin);
            break;
        case MessageActionBarItemKind::delete_message:
            success = paint_shared_icon(paint, bar, item.rectangle, theme.notification, UiIcon::trash);
            break;
        case MessageActionBarItemKind::copy_id:
            success = paint_shared_icon(paint, bar, item.rectangle, theme.text_primary, UiIcon::identifier);
            break;
        case MessageActionBarItemKind::retry:
            success = paint_shared_icon(paint, bar, item.rectangle, theme.text_primary, UiIcon::retry);
            break;
        case MessageActionBarItemKind::plugin:
            // A slot change since the bar opened leaves the cell blank until it reconfigures.
            if (item.catalog_index < plugin_count)
                success = paint_shared_icon(paint, bar, item.rectangle,
                                            ui_plugin_slot_ink(theme, plugins[item.catalog_index].tone,
                                                               theme.text_primary),
                                            ui_plugin_slot_icon(plugins[item.catalog_index]));
            break;
        }
    }
    return paint_stream_pop_clip(paint) && success;
}

bool message_action_bar_text_presentation_build_output(const MessageActionBar& bar, const Theme& theme,
                                                       UiTextOutput* text) noexcept {
    if (!bar.active) {
        return true;
    }
    const UiRect visible = message_action_bar_visible_rectangle(bar);
    if (!ui_text_output_valid(text) || bar.message_id == 0 || bar.item_count == 0 ||
        bar.item_count > kMessageActionBarCapacity || !ui_rect_has_area(visible)) {
        return false;
    }
    for (u32 index = 0; index < bar.item_count; ++index) {
        const MessageActionBarItem& item = bar.items[index];
        if (item.kind != MessageActionBarItemKind::quick_reaction || item.glyph.bytes == 0)
            continue;
        if (!ui_rect_has_area(ui_rect_intersection(item.rectangle, visible)))
            continue;
        if (!ui_text_output_push(text, UiTextCommand{item.glyph, bar.owner, item.rectangle, visible,
                                                     theme.text_primary, text_style_picker_emoji,
                                                     UiTextAlignment::center, UiTextAlignment::center,
                                                     UiTextWrapping::no_wrap, UiTextOverflow::clip})) {
            return false;
        }
    }
    return true;
}

} // namespace maicord

namespace maicord {
bool message_action_bar_text_presentation_build(const MessageActionBar& bar, const Theme& theme,
                                                UiTextCommandStream* text) noexcept {
    UiTextOutput output{text, nullptr};
    return message_action_bar_text_presentation_build_output(bar, theme, &output);
}
} // namespace maicord
