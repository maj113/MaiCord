#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/plugin_slots.hpp"
#include "maicord/ui/message_action_bar.hpp"

namespace maicord {
namespace {

bool paint_shared_icon(PaintStream* paint, UiRect cell, const Color& color, UiIcon icon) noexcept {
    const float x = (cell.left + cell.right) * .5f, y = (cell.top + cell.bottom) * .5f;
    return ui_icon_paint(paint, icon, {x - 10, y - 10, x + 10, y + 10}, color);
}
// Indexed by MessageActionBarItemKind; quick reactions paint text and plugins their slot icon.
constexpr UiIcon kKindIcons[] = {UiIcon::count,   UiIcon::add_reaction, UiIcon::reply,    UiIcon::ellipsis,
                                 UiIcon::forwarded, UiIcon::edit,         UiIcon::link,     UiIcon::eye_off,
                                 UiIcon::pin,     UiIcon::unpin,        UiIcon::trash,    UiIcon::identifier,
                                 UiIcon::retry};
static_assert(sizeof(kKindIcons) / sizeof(kKindIcons[0]) == static_cast<u32>(MessageActionBarItemKind::plugin));

} // namespace

bool message_action_bar_presentation_build(const MessageActionBar& bar, const Theme& theme,
                                           PaintStream* paint) noexcept {
    if (!bar.active) {
        return true;
    }
    // Clipped to the timeline viewport minus the overlays above it (unread
    // banner, jump pill), so the toolbar slides under them.
    const UiRect visible = message_action_bar_visible_rectangle(bar);
    if (!paint || bar.message_id == 0 || bar.item_count == 0 || bar.item_count > kMessageActionBarCapacity ||
        !bar.owner.manager || !ui_rect_has_area(bar.rectangle) || !ui_rect_has_area(visible) ||
        !paint_stream_push_clip(paint, visible)) {
        return false;
    }
    UiPluginSlotNode plugins[kMessageActionBarPluginItems]{};
    const u32 plugin_count = ui_plugin_slot_toolbar(kUiPluginSlotMessageHover, {plugins, kMessageActionBarPluginItems});
    // The hover toolbar floats over the timeline like any popover.
    bool success = ui_surface_paint_plate(paint, bar.rectangle, theme, UiElevation::popover, theme.scale.radius_sm);
    for (u32 index = 0; success && index < bar.item_count; ++index) {
        const MessageActionBarItem& item = bar.items[index];
        if (bar.hovered_index == index) {
            const Color hover = ui_background_hover_color(theme, theme.surface_high);
            success = paint_stream_push_rounded_border(paint, item.rectangle, hover, hover, 4.0f, 0.0f);
        }
        if (!success) {
            break;
        }
        if (item.kind == MessageActionBarItemKind::plugin) {
            // A slot change since the bar opened leaves the cell blank until it reconfigures.
            if (item.catalog_index < plugin_count)
                success = paint_shared_icon(paint, item.rectangle,
                                            ui_plugin_slot_ink(theme, plugins[item.catalog_index].tone,
                                                               theme.text_primary),
                                            ui_plugin_slot_icon(plugins[item.catalog_index]));
        } else if (item.kind != MessageActionBarItemKind::quick_reaction) {
            success = paint_shared_icon(paint, item.rectangle,
                                        item.kind == MessageActionBarItemKind::delete_message ? theme.notification
                                                                                              : theme.text_primary,
                                        kKindIcons[static_cast<u32>(item.kind)]);
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
        if (!ui_rect_intersects(item.rectangle, visible))
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
