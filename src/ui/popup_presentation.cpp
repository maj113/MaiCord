#include "maicord/ui/popup_presentation.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/ui/presence.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/core/hash.hpp"

namespace maicord {
namespace {
inline constexpr UiThemeMetrics kDefaultPopupMetrics{};
const UiThemeMetrics& popup_metrics(const PopupScene& scene) noexcept {
    return scene.themed_menu && scene.theme_styles && scene.theme_styles->ready
               ? scene.theme_styles->metrics
               : kDefaultPopupMetrics;
}
const UiThemeMetrics& popup_paint_metrics(const PopupScene& scene, const Theme& theme) noexcept {
    return scene.themed_menu && theme.styles && theme.styles->ready ? theme.styles->metrics
                                                                    : kDefaultPopupMetrics;
}
Color menu_row_foreground(const PopupItem& item, const Theme& theme) noexcept;
bool paint_popup_presence(PaintStream* paint, UiRect rectangle, UiPresence status, Color background,
                          bool highlighted, const Theme& theme) noexcept {
    if (highlighted && status == UiPresence::offline) {
        const float extent = minimum(ui_rect_width(rectangle), ui_rect_height(rectangle));
        return paint_stream_push_rounded_border(paint, rectangle.left - 2, rectangle.top - 2,
                                                rectangle.right + 2, rectangle.bottom + 2,
                                                theme.text_secondary, background, extent * .5f + 2, 2) &&
               paint_stream_push_rounded(paint, rectangle.left + extent * .25f,
                                         rectangle.top + extent * .25f, rectangle.right - extent * .25f,
                                         rectangle.bottom - extent * .25f, background, extent * .25f);
    }
    return ui_presence_paint(paint, rectangle, status, background, theme);
}

bool paint_menu_row_check(const PopupScene& scene, u32 index, const Theme& theme,
                          PaintStream* paint) noexcept {
    const auto& item = scene.items[index];
    if (!(item.flags & popup_item_checked) || !popup_item_menu_row(item))
        return true;
    PopupItemLayout layout{};
    if (!popup_scene_resolve_item_layout(scene, index, &layout))
        return false;
    auto content = layout.label->content_rectangle;
    if (item.flags & (popup_item_avatar | popup_item_emoji_preview))
        content.left = minimum(content.right, layout.row->rectangle.left + 48);
    const float extent = minimum(popup_metrics(scene).menu_icon, ui_rect_width(content) * .5f);
    const float top = (content.top + content.bottom - extent) * .5f;
    const UiRect slot{content.right - extent, top, content.right, top + extent};
    const auto clip = ui_rect_intersection(slot, layout.clip_rectangle);
    if (!ui_rect_has_area(clip))
        return true;
    if (!paint_stream_push_clip(paint, clip.left, clip.top, clip.right, clip.bottom))
        return false;
    const Color color = menu_row_foreground(item, theme);
    const bool painted =
        ui_icon_paint(paint, UiIcon::check, slot, color) &&
        paint_stream_record_content(paint, {&scene.widgets, scene.rows[index].widget}, slot,
                                    UiLayoutContentKind::icon, ui_layout_content_bounds_allow_overlap);
    return paint_stream_pop_clip(paint) && painted;
}

Color menu_row_foreground(const PopupItem& item, const Theme& theme) noexcept {
    if (item.flags & (popup_item_command_row | popup_item_command_header))
        return theme.text_primary;
    if (!(item.flags & popup_item_enabled))
        return theme.text_secondary;
    return item.flags & popup_item_danger ? ui_foreground_color(theme, theme.notification)
                                          : theme.text_primary;
}

bool paint_menu_row_icon(const PopupScene& scene, u32 index, const Theme& theme,
                         PaintStream* paint) noexcept {
    const PopupItem& item = scene.items[index];
    if (!popup_item_menu_row(item))
        return true;
    const auto presentation = popup_item_menu_presentation(item);
    if (!presentation.icon_plus_one)
        return true;
    UiMenuRowGeometry geometry{};
    if (!popup_scene_menu_row_geometry(scene, index, &geometry))
        return false;
    const UiRect icon_clip = ui_menu_row_slot_clip(geometry, geometry.icon);
    if (!ui_rect_has_area(icon_clip))
        return true;
    if (!paint_stream_push_clip(paint, icon_clip.left, icon_clip.top, icon_clip.right, icon_clip.bottom))
        return false;
    const bool status_icon = item.status && !(item.flags & popup_item_avatar);
    const bool highlighted = (item.flags & popup_item_enabled) &&
                             (scene.selected_index == index || (item.flags & popup_item_checked));
    const auto slot = geometry.icon;
    const float x = (slot.left + slot.right) * .5f, y = (slot.top + slot.bottom) * .5f;
    Color icon_color = menu_row_foreground(item, theme);
    if ((item.media_flags & kPopupItemMediaIconTint) && item.media_variant &&
        !(item.flags & (popup_item_avatar | popup_item_emoji_preview)) && (item.flags & popup_item_enabled)) {
        constexpr float scale = 1.0f / 255.0f;
        // Role colours are server data and pass through every theme unchanged.
        icon_color = Color{static_cast<float>((item.media_variant >> 16) & 0xFFu) * scale,
                           static_cast<float>((item.media_variant >> 8) & 0xFFu) * scale,
                           static_cast<float>(item.media_variant & 0xFFu) * scale, 1.0f};
    }
    const bool painted =
        (status_icon ? paint_popup_presence(
                           paint, {x - 5, y - 5, x + 5, y + 5}, static_cast<UiPresence>(item.status),
                           highlighted ? theme.surface_high : theme.base_lower, highlighted, theme)
                     : ui_icon_paint(paint, ui_menu_row_icon(presentation), geometry.icon, icon_color)) &&
        paint_stream_record_content(paint, {&scene.widgets, scene.rows[index].widget}, geometry.icon,
                                    UiLayoutContentKind::icon, ui_layout_content_bounds_allow_overlap);
    return paint_stream_pop_clip(paint) && painted;
}
} // namespace

UiRect popup_avatar_rectangle(const UiLayoutNode& row) noexcept {
    const float y = (row.rectangle.top + row.rectangle.bottom) * .5f - 12;
    return {row.rectangle.left + 8, y, row.rectangle.left + 32, y + 24};
}

bool popup_presentation_build(const PopupScene& scene, const Theme& theme, PaintStream* paint) noexcept {
    if (!paint || !scene.active || scene.item_count == 0 || scene.item_count > scene.items.count ||
        scene.item_count > scene.rows.count) {
        return !scene.active;
    }
    const UiLayoutNode* panel = popup_scene_layout_node(scene, scene.panel);
    const float panel_radius =
        scene.themed_menu ? popup_paint_metrics(scene, theme).menu_radius : kUiMenuCornerRadius;
    // The drop shadow falls outside the panel clip, so it paints first.
    if (!panel ||
        !ui_surface_shadow_paint_isolated(paint, panel->rectangle, theme, UiElevation::popover, panel_radius) ||
        !paint_stream_push_clip(paint, panel->rectangle.left, panel->rectangle.top, panel->rectangle.right,
                                panel->rectangle.bottom)) {
        return false;
    }
    // The pushed clip also forces a distinct paint batch, so the overlay cannot
    // merge into the last base batch and draw under media/text sharing the root
    // clip. Every popup kind shares the popover elevation material on gradient themes.
    bool success =
        ui_surface_paint_plate(paint, panel->rectangle, theme, UiElevation::popover, panel_radius) &&
        paint_stream_record_content(paint, UiWidgetReference{&scene.widgets, scene.panel}, panel->rectangle,
                                    UiLayoutContentKind::control, ui_layout_content_bounds_allow_overlap);
    for (u32 index = 0; success && index < scene.item_count; ++index) {
        const PopupItem& item = scene.items[index];
        const PopupRowWidget& row = scene.rows[index];
        PopupItemLayout item_layout{};
        if (!popup_scene_resolve_item_layout(scene, index, &item_layout)) {
            success = false;
            break;
        }
        if (!item_layout.visible) {
            continue;
        }
        const UiLayoutNode* layout = item_layout.row;
        if ((item.flags & popup_item_separator_before) != 0) {
            const float gap = (item.flags & popup_item_quick_reaction) != 0 ||
                                      (scene.item_count >= kPopupQuickReactionCount &&
                                       (scene.items[0].flags & popup_item_quick_reaction) != 0)
                                  ? kPopupContextSeparatorGap
                                  : popup_metrics(scene).menu_gap;
            const float y = layout->rectangle.top - gap * 0.5f;
            if (!paint_stream_push(paint, layout->rectangle.left + 4.0f, y, layout->rectangle.right - 4.0f,
                                   y + 1.0f, theme.interactive)) {
                success = false;
                break;
            }
        }
        // A checked (not pointed-at) row uses the emphasis selected tint at
        // popover elevation, never the hover fill, so the pointed-at row stays
        // distinguishable from checked ones.
        const bool checked_only = scene.selected_index != index && (item.flags & popup_item_checked);
        const Color highlight = checked_only ? ui_emphasis_selected(theme, UiElevation::popover)
                                             : ui_background_hover_color(theme, theme.surface_high, .28f);
        if ((scene.selected_index == index || (item.flags & popup_item_checked) != 0) &&
            (item.flags & popup_item_enabled) != 0 &&
            !paint_stream_push_rounded_border(paint, layout->rectangle.left, layout->rectangle.top,
                                              layout->rectangle.right, layout->rectangle.bottom, highlight,
                                              highlight, popup_paint_metrics(scene, theme).compact_radius,
                                              0.0f)) {
            success = false;
            break;
        }
        if (item.flags & popup_item_avatar) {
            const auto avatar = popup_avatar_rectangle(*layout);
            if (!paint_stream_push_rounded(paint, avatar.left, avatar.top, avatar.right, avatar.bottom,
                                           theme.interactive, theme.scale.radius_lg) ||
                (!(item.flags & (popup_item_command_row | popup_item_command_category)) && !paint_popup_presence(
                    paint, {avatar.right + 1, avatar.bottom - 7, avatar.right + 8, avatar.bottom},
                    static_cast<UiPresence>(item.status),
                    (item.flags & popup_item_enabled) &&
                            (scene.selected_index == index || (item.flags & popup_item_checked))
                        ? highlight
                        : theme.base_lower,
                    (item.flags & popup_item_enabled) &&
                        (scene.selected_index == index || (item.flags & popup_item_checked)),
                    theme))) {
                success = false;
                break;
            }
        }
        if (item.flags & popup_item_command_close) {
            const float inset = popup_paint_metrics(scene, theme).menu_gap;
            auto icon = layout->rectangle;
            icon.left += inset;
            icon.top += inset;
            icon.right -= inset;
            icon.bottom -= inset;
            if (!ui_icon_paint(paint, UiIcon::close, icon, theme.text_secondary)) {
                success = false;
                break;
            }
        }
        if (!paint_menu_row_icon(scene, index, theme, paint) ||
            !paint_menu_row_check(scene, index, theme, paint) ||
            !paint_stream_record_content(paint, UiWidgetReference{&scene.widgets, row.widget},
                                         layout->rectangle, UiLayoutContentKind::control,
                                         ui_layout_content_bounds_allow_overlap)) {
            success = false;
        }
    }
    const auto feedback = popup_feedback_rectangle(scene);
    if (ui_rect_has_area(feedback))
        success = paint_stream_push(paint, feedback.left, feedback.top, feedback.right, feedback.top + 1,
                                    theme.surface_high) &&
                  success;
    if (const UiRect track = popup_feedback_slider(scene); ui_rect_has_area(track)) {
        const auto tokens = ui_settings_tokens(theme);
        const u32 count = scene.feedback.slider_count;
        const auto g = ui_slider_geometry(tokens, track, count, scene.feedback.slider_value, false);
        success = ui_slider_paint(paint, tokens, g, count, count, {}) && success;
    }
    if (panel->spec.style.flags & ui_layout_node_scroll_vertical)
        success =
            ui_scrollbar_paint(paint,
                               ui_scrollbar_geometry(scene.rectangle, scene.scroll_offset,
                                                     ui_rect_height(scene.rectangle) + scene.scroll_max),
                               theme) &&
            success;
    return paint_stream_pop_clip(paint) && success;
}

bool popup_text_presentation_build_output(const PopupScene& scene, const Theme& theme,
                                          UiTextOutput* text, UiTextMeasureCache* measure) noexcept {
    if (!ui_text_output_valid(text) || !scene.active || scene.item_count == 0 ||
        scene.item_count > scene.items.count || scene.item_count > scene.rows.count) {
        return !scene.active;
    }
    for (u32 index = 0; index < scene.item_count; ++index) {
        const PopupItem& item = scene.items[index];
        if (item.flags & popup_item_command_close)
            continue;
        if ((item.flags & popup_item_custom_emoji) != 0 ||
            ((item.flags & popup_item_emoji) &&
             !(item.flags & (popup_item_custom_emoji | popup_item_avatar | popup_item_guild_icon)) &&
             regional_flag_sequence(item.label)) ||
            ((item.flags & popup_item_guild_icon) && item.media_variant)) {
            continue;
        }
        const PopupRowWidget& row = scene.rows[index];
        PopupItemLayout item_layout{};
        if (!popup_scene_resolve_item_layout(scene, index, &item_layout)) {
            return false;
        }
        if (!item_layout.visible) {
            continue;
        }
        const UiLayoutNode* label = item_layout.label;
        const Color color = menu_row_foreground(item, theme);
        const bool centered = (item.flags & popup_item_emoji) != 0;
        const Utf8View hints[] = {{},
                                  utf8_literal("Ctrl+Z"),
                                  utf8_literal("Ctrl+Y"),
                                  utf8_literal("Ctrl+X"),
                                  utf8_literal("Ctrl+C"),
                                  utf8_literal("Ctrl+V"),
                                  utf8_literal("Del"),
                                  utf8_literal("Ctrl+A")};
        static_assert(sizeof(hints) / sizeof(hints[0]) == popup_shortcut_count);
        const auto hint =
            !centered && item.shortcut < popup_shortcut_count ? hints[item.shortcut] : Utf8View{};
        UiRect label_rectangle = label->content_rectangle;
        const bool command_header = (item.flags & popup_item_command_header) != 0;
        if (command_header)
            label_rectangle.right = maximum(label_rectangle.left,
                label_rectangle.right - popup_metrics(scene).menu_row_height);
        const bool shared_row = popup_item_menu_row(item);
        UiMenuRowGeometry menu_geometry{};
        if (shared_row) {
            if (!popup_scene_menu_row_geometry(scene, index, &menu_geometry))
                return false;
            label_rectangle = menu_geometry.primary;
        }
        if (!shared_row && (item.flags & (popup_item_avatar | popup_item_emoji_preview)))
            label_rectangle.left = item_layout.row->rectangle.left + 48;
        if (item.flags & popup_item_command_category)
            label_rectangle = label->content_rectangle;
        if (item.detail.bytes) {
            Color detail_color = theme.text_primary;
            detail_color.alpha *= .65f;
            UiRect detail = shared_row ? menu_geometry.detail : label_rectangle;
            if (command_header) {
                const float available = ui_rect_width(label_rectangle) * .5f;
                const TextStyleSpan name_style{0, item.label.bytes, text_style_bold};
                UiTextMeasureValue measured{};
                const bool shaped = measure && ui_text_measure_cache_measure(measure,
                    {{scene.owner_id, hash64_bytes(kHash64Seed, item.label.data, item.label.bytes),
                      0, 0x4348, 0}, item.label, {&name_style, 1},
                     {maximum(1.f, ui_rect_width(label_rectangle)), ui_rect_height(label_rectangle)}, true},
                    &measured).ok();
                // The renderer supplies its normal text backend. A caller
                // without shaping uses two bounded slots, never byte estimates.
                const float name_width = shaped ? minimum(available, measured.size.width) : available;
                label_rectangle.right = label_rectangle.left + name_width;
                detail.left = minimum(detail.right, label_rectangle.right + popup_metrics(scene).menu_gap);
            } else if (!shared_row) {
                if (item.flags & popup_item_command_row) {
                    detail.top += 25;
                    label_rectangle.bottom = label_rectangle.top + 25;
                } else {
                    detail.left = detail.right - minimum(180.0f, ui_rect_width(label_rectangle) * .42f);
                    label_rectangle.right = maximum(label_rectangle.left, detail.left - 12);
                }
            }
            if (!ui_text_output_push(
                    text,
                    {item.detail,
                     {&scene.widgets, row.label},
                     detail,
                     shared_row ? ui_menu_row_slot_clip(menu_geometry, detail) : item_layout.clip_rectangle,
                     detail_color,
                     text_style_none,
                     command_header || (shared_row && popup_item_menu_presentation(item).secondary_line) ||
                             (item.flags & popup_item_command_row)
                         ? UiTextAlignment::start
                         : UiTextAlignment::end,
                     UiTextAlignment::center,
                     UiTextWrapping::no_wrap,
                     UiTextOverflow::ellipsis}))
                return false;
        }
        if (hint.bytes) {
            UiRect hint_rectangle = shared_row ? menu_geometry.shortcut : label_rectangle;
            if (!shared_row) {
                const float hint_width = minimum(72.0f, ui_rect_width(label_rectangle) * 0.45f);
                hint_rectangle.left = hint_rectangle.right - hint_width;
                label_rectangle.right = maximum(label_rectangle.left, hint_rectangle.left - 12.0f);
            }
            if (!ui_text_output_push(text,
                                     {hint,
                                      {&scene.widgets, row.label},
                                      hint_rectangle,
                                      shared_row ? ui_menu_row_slot_clip(menu_geometry, hint_rectangle)
                                                 : item_layout.clip_rectangle,
                                      color,
                                      text_style_none,
                                      UiTextAlignment::end,
                                      UiTextAlignment::center,
                                      UiTextWrapping::no_wrap,
                                      UiTextOverflow::ellipsis}))
                return false;
        }
        if ((item.flags & popup_item_command_category) && (item.flags & popup_item_avatar))
            continue;
        if (!ui_text_output_push(
                text, UiTextCommand{
                          item.label, UiWidgetReference{&scene.widgets, row.label}, label_rectangle,
                          shared_row ? ui_menu_row_slot_clip(menu_geometry, label_rectangle)
                                     : item_layout.clip_rectangle,
                          color,
                          command_header ? text_style_bold :
                          (item.flags & popup_item_emoji) != 0 ? text_style_picker_emoji : text_style_none,
                          centered ? UiTextAlignment::center : UiTextAlignment::start,
                          UiTextAlignment::center, UiTextWrapping::no_wrap, UiTextOverflow::ellipsis})) {
            return false;
        }
    }
    const auto feedback = popup_feedback_rectangle(scene);
    if (scene.feedback.text.bytes && ui_rect_has_area(feedback)) {
        auto label = feedback;
        label.left += scene.feedback.style.text_inset;
        label.right = maximum(label.left, label.right - scene.feedback.style.text_inset);
        if (const UiRect track = popup_feedback_slider(scene); ui_rect_has_area(track))
            label.bottom = maximum(label.top, track.top);
        const auto color =
            ui_foreground_color(theme, scene.feedback.error ? theme.notification : theme.text_secondary);
        if (!ui_text_output_push(text, {scene.feedback.text,
                                        {&scene.widgets, scene.panel},
                                        label,
                                        feedback,
                                        color,
                                        text_style_subtext,
                                        UiTextAlignment::start,
                                        UiTextAlignment::center,
                                        UiTextWrapping::no_wrap,
                                        UiTextOverflow::ellipsis}))
            return false;
    }
    return true;
}

PopupMediaPresentationStatus popup_media_presentation_resolve(const PopupScene& scene, u32 index,
                                                              PopupMediaCommand* command) noexcept {
    if (command) {
        *command = PopupMediaCommand{};
    }
    if (!command || !scene.active || index >= scene.item_count || index >= scene.items.count) {
        return PopupMediaPresentationStatus::invalid;
    }
    const PopupItem& item = scene.items[index];
    const bool unicode_cell =
        (item.flags & popup_item_emoji) &&
        !(item.flags & (popup_item_custom_emoji | popup_item_avatar | popup_item_guild_icon));
    const u16 flag = item.regional_flag ? item.regional_flag
                     : unicode_cell     ? regional_flag_sequence(item.label)
                                        : 0;
    if (!flag && ((item.flags & (popup_item_custom_emoji | popup_item_avatar | popup_item_emoji_preview |
                                 popup_item_guild_icon)) == 0 ||
                  ((item.flags & popup_item_guild_icon) && !item.media_variant))) {
        return PopupMediaPresentationStatus::skip;
    }
    PopupItemLayout layout{};
    if ((!flag && item.media_id == 0) || !popup_scene_resolve_item_layout(scene, index, &layout)) {
        return PopupMediaPresentationStatus::invalid;
    }
    if (!layout.visible) {
        return PopupMediaPresentationStatus::skip;
    }
    // Custom emoji own a fixed, centered label node in PopupScene; using its
    // rectangle keeps media and Unicode emoji on one layout path.
    *command = PopupMediaCommand{layout.label->rectangle,
                                 layout.clip_rectangle,
                                 item.media_id,
                                 item.media_flags,
                                 item.media_variant,
                                 (item.flags & popup_item_avatar) != 0,
                                 (item.flags & popup_item_guild_icon) != 0};
    command->regional_flag = flag;
    if (command->guild_icon) {
        const auto r = layout.row->rectangle;
        const float x = (r.left + r.right) * .5f, y = (r.top + r.bottom) * .5f;
        command->rectangle = {x - 16.0f, y - 16.0f, x + 16.0f, y + 16.0f};
    }
    if (command->avatar || (item.flags & popup_item_emoji_preview))
        command->rectangle = popup_avatar_rectangle(*layout.row);
    return PopupMediaPresentationStatus::ready;
}

} // namespace maicord

namespace maicord {
bool popup_text_presentation_build(const PopupScene& scene, const Theme& theme,
                                   UiTextCommandStream* text, UiTextMeasureCache* measure) noexcept {
    UiTextOutput output{text, nullptr};
    return popup_text_presentation_build_output(scene, theme, &output, measure);
}
} // namespace maicord
