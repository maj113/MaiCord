#pragma once
#include "maicord/ui/badge.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/icon.hpp"

namespace maicord {

// Declarative plugin decorations, read by scenes and the renderer through one
// application-installed query. Views borrow plugin host storage until the
// next slot revision; nothing here calls plugin code.
inline constexpr u32 kUiPluginSlotMessageAccessory = 0;
inline constexpr u32 kUiPluginSlotComposerToolbar = 1;
inline constexpr u32 kUiPluginSlotMemberBadge = 2;
inline constexpr u32 kUiPluginSlotProfileSection = 3;
inline constexpr u32 kUiPluginSlotHeaderToolbar = 4;
inline constexpr u32 kUiPluginSlotChannelRow = 5;
inline constexpr u32 kUiPluginSlotGuildRail = 6;
inline constexpr u32 kUiPluginSlotAccountPanel = 7;
inline constexpr u32 kUiPluginSlotMessageHover = 8;
inline constexpr u16 kUiPluginSlotNoIcon = 0xffff;
enum : u8 {
    ui_plugin_node_label,
    ui_plugin_node_icon,
    ui_plugin_node_badge,
    ui_plugin_node_button,
    ui_plugin_node_link,     // value: https URL opened through the link confirmation
    ui_plugin_node_separator // popovers only
};
// Square plugin buttons per toolbar: account panel and message hover bar.
inline constexpr u32 kUiPluginSlotAccountButtons = 2;
inline constexpr u32 kUiPluginSlotHoverButtons = 2;

struct UiPluginSlotNode {
    u64 owner_id, action_id;
    Utf8View text, tooltip;
    u16 icon;
    u8 kind, tone;
    Utf8View value;
};
using UiPluginSlotQuery = u32 (*)(u32 surface, u64 channel_id, u64 message_id, u64 user_id, u64 guild_id,
                                  Slice<UiPluginSlotNode> output) noexcept;
inline UiPluginSlotQuery g_ui_plugin_slot_query = nullptr;

inline u32 ui_plugin_slots(u32 surface, u64 channel_id, u64 message_id, u64 user_id, u64 guild_id,
                           Slice<UiPluginSlotNode> output) noexcept {
    return g_ui_plugin_slot_query ? g_ui_plugin_slot_query(surface, channel_id, message_id, user_id, guild_id, output)
                                  : 0;
}
inline u32 ui_plugin_slots(u32 surface, u64 channel_id, u64 message_id, u64 user_id,
                           Slice<UiPluginSlotNode> output) noexcept {
    return ui_plugin_slots(surface, channel_id, message_id, user_id, 0, output);
}
// Tooltip first, else the node text (icon-only nodes name themselves there).
inline Utf8View ui_plugin_slot_label(const UiPluginSlotNode& node) noexcept {
    return node.tooltip.bytes ? node.tooltip : node.text;
}
// The plugin control under the pointer (the account panel paints its hover
// fill from it). Only the application writes it.
struct UiPluginSlotHover {
    u32 surface, index;
    bool active;
};
inline UiPluginSlotHover g_ui_plugin_slot_hover{};

// Tones are theme roles; plugins never pick colours.
inline UiIconRole ui_plugin_slot_role(u8 tone) noexcept {
    switch (tone) {
    case 1: return UiIconRole::accent;
    case 2: return UiIconRole::success;
    case 3: return UiIconRole::warning;
    case 4: return UiIconRole::danger;
    default: return UiIconRole::muted;
    }
}

// The first node of a target becomes one compact badge: its text (or tooltip
// for an icon-only node) with the icon, or the tone marker, in the leading slot.
struct UiPluginSlotBadge {
    UiBadgeWidget badge;
    u16 icon;
    u8 tone;
    u64 owner_id, action_id;
};
inline bool ui_plugin_slot_badge(u32 surface, u64 channel_id, u64 message_id, u64 user_id, UiWidgetHandle owner,
                                 UiPluginSlotBadge* out) noexcept {
    reset_value(out);
    UiPluginSlotNode nodes[4]{};
    const u32 count = ui_plugin_slots(surface, channel_id, message_id, user_id, {nodes, 4});
    for (u32 i = 0; i < count; ++i) {
        const auto& node = nodes[i];
        const Utf8View label = node.text.bytes ? node.text : node.tooltip;
        if (!label.bytes) continue;
        out->badge.owner = owner;
        out->badge.label = label;
        out->badge.leading = true;
        out->badge.style = text_style_subtext | text_style_bold;
        out->icon = node.icon;
        out->tone = node.tone;
        out->owner_id = node.owner_id;
        out->action_id = node.kind == ui_plugin_node_button ? node.action_id : 0;
        return true;
    }
    return false;
}
// Toolbar surfaces (composer) draw buttons and icons as square controls;
// labels and badges have no room there. Plugin order, at most output.count.
inline u32 ui_plugin_slot_toolbar(u32 surface, Slice<UiPluginSlotNode> output) noexcept {
    UiPluginSlotNode nodes[16]{};
    const u32 count = ui_plugin_slots(surface, 0, 0, 0, {nodes, 16});
    u32 used = 0;
    for (u32 i = 0; i < count && used < output.count; ++i)
        if (nodes[i].kind == ui_plugin_node_button || nodes[i].kind == ui_plugin_node_icon)
            output[used++] = nodes[i];
    return used;
}
// Unknown or missing icon names fall back to the plugin glyph.
inline UiIcon ui_plugin_slot_icon(const UiPluginSlotNode& node) noexcept {
    return node.icon < static_cast<u16>(UiIcon::count) ? static_cast<UiIcon>(node.icon) : UiIcon::puzzle;
}
// Right-aligns the target's badge inside line (header title, member row).
inline bool ui_plugin_slot_line_badge(u32 surface, u64 channel_id, u64 message_id, u64 user_id, UiWidgetHandle owner,
                                      UiRect line, UiTextMeasureCache* cache, UiPluginSlotBadge* out) noexcept {
    if (!ui_plugin_slot_badge(surface, channel_id, message_id, user_id, owner, out)) return false;
    out->badge.rectangle = ui_badge_layout(cache, out->badge.label, line, true, out->badge.style).outer;
    if (ui_rect_width(out->badge.rectangle) >= 40) return true;
    reset_value(out);
    return false;
}
// Paints the badge frame and its leading icon (or a tone marker) in theme colours.
inline bool ui_plugin_slot_badge_paint(const UiPluginSlotBadge& slot, const UiWidgetManager& widgets,
                                       const Theme& theme, PaintStream* paint) noexcept {
    if (!slot.badge.label.bytes) return true;
    UiBadgeWidget badge = slot.badge;
    const Color role = ui_icon_role_color(theme, ui_plugin_slot_role(slot.tone));
    badge.marker = role;
    if (slot.icon != kUiPluginSlotNoIcon) badge.marker.alpha = 0;
    if (!ui_badge_paint(badge, widgets, theme, paint)) return false;
    if (slot.icon == kUiPluginSlotNoIcon || slot.icon >= static_cast<u16>(UiIcon::count)) return true;
    const auto* node = ui_widget_manager_layout_node(widgets, badge.owner);
    if (!node) return false;
    const auto geometry = ui_badge_geometry(badge, widgets);
    if (!paint_stream_push_clip(paint, node->clip_rectangle)) return false;
    const bool painted = ui_icon_paint(paint, static_cast<UiIcon>(slot.icon), geometry.icon, role);
    return paint_stream_pop_clip(paint) && painted;
}

// Toolbar ink: neutral nodes keep the surrounding icon colour, tones their role.
inline Color ui_plugin_slot_ink(const Theme& theme, u8 tone, Color neutral) noexcept {
    return tone ? ui_icon_role_color(theme, ui_plugin_slot_role(tone)) : neutral;
}
// Channel and DM rows: the target's badge right-aligned at the end of the
// row's label (which already stops before the mention badge or the DM close
// button), centred on center_y. Hidden unless the name keeps `keep` DIPs.
inline bool ui_plugin_slot_row_badge(u32 surface, u64 channel_id, UiWidgetHandle owner, UiRect label, float center_y,
                                     float keep, UiPluginSlotBadge* out) noexcept {
    if (!ui_plugin_slot_badge(surface, channel_id, 0, 0, owner, out)) return false;
    const UiRect line{label.left, center_y - 10, label.right, center_y + 10};
    const auto layout = ui_badge_layout(nullptr, out->badge.label, line, true, out->badge.style);
    out->badge.rectangle = layout.outer;
    if (!layout.clipped && layout.outer.left >= label.left + keep) return true;
    reset_value(out);
    return false;
}
// A small round badge on a server icon's top-right corner (the mention badge
// owns the bottom-right): the node's icon, or a tone dot.
struct UiPluginSlotCorner {
    UiRect rectangle;
    Utf8View label;
    u64 owner_id, action_id;
    u16 icon;
    u8 tone;
};
inline constexpr float kUiPluginSlotCornerExtent = 16;
inline bool ui_plugin_slot_corner(u32 surface, u64 guild_id, UiRect icon, UiPluginSlotCorner* out) noexcept {
    reset_value(out);
    if (!guild_id || !ui_rect_has_area(icon)) return false;
    UiPluginSlotNode nodes[4]{};
    const u32 count = ui_plugin_slots(surface, 0, 0, 0, guild_id, {nodes, 4});
    for (u32 i = 0; i < count; ++i) {
        const auto& node = nodes[i];
        if (node.kind == ui_plugin_node_separator || node.kind == ui_plugin_node_link) continue;
        out->rectangle = {icon.right + 2 - kUiPluginSlotCornerExtent, icon.top - 2, icon.right + 2,
                          icon.top - 2 + kUiPluginSlotCornerExtent};
        out->label = ui_plugin_slot_label(node);
        out->owner_id = node.owner_id;
        out->action_id = node.kind == ui_plugin_node_button ? node.action_id : 0;
        out->icon = node.icon;
        out->tone = node.tone;
        return true;
    }
    return false;
}
// cutout is the rail colour ringing the badge, as around the mention badge.
inline bool ui_plugin_slot_corner_paint(const UiPluginSlotCorner& corner, Color cutout, const Theme& theme,
                                        PaintStream* paint) noexcept {
    if (!ui_rect_has_area(corner.rectangle)) return true;
    const UiRect r = corner.rectangle;
    const float ring = 2;
    const Color role = ui_icon_role_color(theme, ui_plugin_slot_role(corner.tone));
    const bool icon = corner.icon != kUiPluginSlotNoIcon && corner.icon < static_cast<u16>(UiIcon::count);
    if (!paint_stream_push_rounded_border(paint, r.left - ring, r.top - ring, r.right + ring, r.bottom + ring,
                                          icon ? theme.surface_high : role, cutout,
                                          ui_rect_width(r) * .5f + ring, ring))
        return false;
    return !icon || ui_icon_paint(paint, static_cast<UiIcon>(corner.icon),
                                  {r.left + 2, r.top + 2, r.right - 2, r.bottom - 2}, role);
}
// Account panel: square buttons left of microphone/deafen/settings (which
// end kUiVoiceAccountButtonsWidth from the right), nearest first.
inline constexpr float kUiPluginSlotAccountButtonsRight = 112, kUiPluginSlotAccountButtonStride = 36;
inline UiRect ui_plugin_slot_account_button(UiRect footer, u32 index) noexcept {
    const float right = footer.right - kUiPluginSlotAccountButtonsRight - kUiPluginSlotAccountButtonStride * index;
    const float middle = (footer.top + footer.bottom) * .5f;
    return {right - 32, middle - 16, right, middle + 16};
}
inline u32 ui_plugin_slot_account_buttons(Slice<UiPluginSlotNode> output) noexcept {
    return ui_plugin_slot_toolbar(kUiPluginSlotAccountPanel,
                                  {output.data, minimum(output.count, kUiPluginSlotAccountButtons)});
}
// Extra width the account name gives up to the plugin buttons.
inline float ui_plugin_slot_account_width() noexcept {
    UiPluginSlotNode nodes[kUiPluginSlotAccountButtons]{};
    return kUiPluginSlotAccountButtonStride * static_cast<float>(ui_plugin_slot_account_buttons({nodes, kUiPluginSlotAccountButtons}));
}

} // namespace maicord
