#include "maicord/ui/popup_presentation.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/ui/presence.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"

namespace maicord {
namespace {
const UiThemeMetrics& popup_metrics(const PopupScene& scene) noexcept {
    return scene.themed_menu && scene.theme_styles && scene.theme_styles->ready
               ? scene.theme_styles->metrics
               : kUiThemeMetricsDefault;
}
const UiThemeMetrics& popup_paint_metrics(const PopupScene& scene, const Theme& theme) noexcept {
    return scene.themed_menu && theme.styles && theme.styles->ready ? theme.styles->metrics
                                                                    : kUiThemeMetricsDefault;
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
    const bool painted = ui_icon_paint(paint, UiIcon::check, slot, color);
    return paint_stream_pop_clip(paint) && painted;
}

Color menu_row_foreground(const PopupItem& item, const Theme& theme) noexcept {
    if (item.flags & popup_item_command_row)
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
        // Role colours are server data and pass through every theme unchanged.
        icon_color = color_from_rgb24(item.media_variant);
    }
    const bool painted =
        status_icon ? paint_popup_presence(
                          paint, {x - 5, y - 5, x + 5, y + 5}, static_cast<UiPresence>(item.status),
                          highlighted ? theme.surface_high : theme.base_lower, highlighted, theme)
                    : ui_icon_paint(paint, ui_menu_row_icon(presentation), geometry.icon, icon_color);
    return paint_stream_pop_clip(paint) && painted;
}
UiRect popup_avatar_rectangle(const UiLayoutNode& row) noexcept {
    const float y = (row.rectangle.top + row.rectangle.bottom) * .5f - 12;
    return {row.rectangle.left + 8, y, row.rectangle.left + 32, y + 24};
}

// The command picker (all.css .wrapper__920ab, .categoryHeader_d1405b,
// .wrapper_b1e4f3, .selected__1a58a): rows pad their 40-DIP content by 8
// DIPs, an app image is 32 DIPs 4 DIPs below the content top with 16 DIPs
// after it, headers show a 16-DIP icon 8 DIPs in and their label 4 DIPs
// after it, and rail icons fill their 32-DIP slot (Built-in and Frequently
// Used glyphs 24 DIPs inside it).
constexpr float kPickerRowPadding = 8, kPickerImage = 32, kPickerImageGap = 16, kPickerHeaderIcon = 16,
                kPickerHeaderGap = 4, kPickerRailGlyph = 24, kPickerTitleLine = 20, kPickerLineGap = 4;
// Usage chips (.option__a19535, .option__920ab): 8 DIPs apart, 4 DIPs of
// padding, a text-sm line; the optional count follows a 1-DIP divider
// (.optionals__920ab) with 8 DIPs on both sides.
constexpr float kPickerChipGap = 8, kPickerChipPadding = 4, kPickerChipHeight = 18;
// Loading skeletons (.wrapper__78aa3, .autocompletePlaceholder__13533):
// 16-DIP pill bars, 8 DIPs apart.
constexpr float kPickerBar = 16, kPickerBarGap = 8;
bool picker_discovery(const PopupScene& scene) noexcept {
    return (scene.items[0].flags & popup_item_command_category) != 0;
}
bool picker_scene(const PopupScene& scene) noexcept {
    return popup_items_command_picker({scene.items.data, scene.item_count});
}
// Where a picker item's avatar or glyph goes.
UiRect picker_media_rectangle(const PopupItem& item, const UiLayoutNode& row) noexcept {
    const UiRect r = row.rectangle;
    if (item.flags & popup_item_command_category)
        return r;
    if (item.flags & popup_item_command_section) {
        const float y = (r.top + r.bottom - kPickerHeaderIcon) * .5f;
        return {r.left + kPickerRowPadding, y, r.left + kPickerRowPadding + kPickerHeaderIcon, y + kPickerHeaderIcon};
    }
    const float top = r.top + kPickerRowPadding + kPickerLineGap;
    return {r.left + kPickerRowPadding, top, r.left + kPickerRowPadding + kPickerImage, top + kPickerImage};
}
// A rail app icon is a circle that becomes a rounded square (30%) while
// hovered or selected (213404: SQUIRCLE mask when selectable and selected or
// hovered), as a server icon does.
float picker_media_radius(const PopupScene& scene, u32 index, UiRect media) noexcept {
    const PopupItem& item = scene.items[index];
    const float extent = minimum(ui_rect_width(media), ui_rect_height(media));
    const bool morph = (item.flags & popup_item_command_category) &&
                       ((item.flags & popup_item_checked) || scene.selected_index == index);
    return extent * (morph ? .3f : .5f);
}
// Text starts after the header icon or row image.
float picker_text_left(const PopupItem& item, const UiLayoutNode& row) noexcept {
    if (item.flags & popup_item_command_section)
        return item.presentation.icon_plus_one || (item.flags & popup_item_avatar)
                   ? row.rectangle.left + kPickerRowPadding + kPickerHeaderIcon + kPickerHeaderGap
                   : row.rectangle.left + kPickerRowPadding;
    return row.rectangle.left + kPickerRowPadding +
           ((item.flags & popup_item_avatar) || item.presentation.icon_plus_one ? kPickerImage + kPickerImageGap
                                                                                 : 0.f);
}
bool picker_keyboard_focus(const PopupScene& scene, u32 index) noexcept {
    return scene.selected_index == index &&
           !ui_widget_handle_equal(scene.widgets.hovered, scene.rows[index].widget);
}
float picker_measure(UiTextMeasureCache* measure, const PopupScene& scene, Utf8View value, u32 flags,
                     float limit, u16 tag) noexcept {
    if (!measure || !value.bytes)
        return limit;
    const TextStyleSpan span{0, value.bytes, flags};
    UiTextMeasureValue measured{};
    if (!ui_text_measure_cache_measure(
             measure,
             {{scene.owner_id, hash64_bytes(kHash64Seed, value.data, value.bytes), 0, tag, 0}, value, {&span, 1},
              {maximum(1.f, limit), kCommandRowHeight}, true},
             &measured)
             .ok())
        return limit;
    return minimum(limit, measured.size.width);
}
// A loading bar's width: the client draws each between lo and hi at random;
// the seed keeps a row's bars steady from frame to frame.
float placeholder_width(u32 seed, u32 bar, float lo, float hi) noexcept {
    const u64 mixed = hash64_mix(hash64_mix(kHash64Seed, seed), bar);
    return lo + static_cast<float>(mixed % 1000u) * (hi - lo) / 999.f;
}
// The skeleton bars of a placeholder row: in the picker the command skeleton
// (524007: usage 60-120 on the strong fill, description 200-600, source
// 45-90), elsewhere the generic Loading row (one 60-120 bar).
bool paint_placeholder(PaintStream* paint, const PopupItem& item, UiRect content, bool command,
                       const Theme& theme) noexcept {
    const u32 seed = item.media_variant;
    const float radius = kPickerBar * .5f;
    const auto bar = [&](float left, float top, float width, const Color& color) noexcept {
        const float right = minimum(content.right, left + width);
        return right <= left || paint_stream_push_rounded(paint, {left, top, right, top + kPickerBar}, color, radius);
    };
    if (!command)
        return bar(content.left, (content.top + content.bottom - kPickerBar) * .5f,
                   placeholder_width(seed, 0, 60, 120), theme.base_low);
    const float source = placeholder_width(seed, 2, 45, 90);
    const float info_right = content.right - source - kPickerImageGap;
    return bar(content.right - source, (content.top + content.bottom - kPickerBar) * .5f, source, theme.base_low) &&
           bar(content.left, content.top, minimum(info_right - content.left, placeholder_width(seed, 0, 60, 120)),
               theme.tokens.fill.muted) &&
           bar(content.left, content.top + kPickerBar + kPickerBarGap,
               minimum(info_right - content.left, placeholder_width(seed, 1, 200, 600)), theme.base_low);
}
// A command row's columns: description, source and usage summary from the
// detail ("description\x1Fsource\x1Fsummary", summary parts \x1E-separated:
// the required option names, then "+N optional"); the summary shows only on
// the selected row, as the client shows it on hover or keyboard selection.
inline constexpr u32 kPickerChipCapacity = 8;
struct PickerRow {
    Utf8View description, source;
    UiRect source_rectangle, title, description_rectangle;
    // The usage summary: the chips (required names), the optional count and
    // the divider before it; chip_count is zero when the summary is hidden.
    Utf8View chips[kPickerChipCapacity], optional;
    UiRect chip_rectangles[kPickerChipCapacity], optional_rectangle;
    u32 chip_count;
    float divider;
    bool summary;
};
PickerRow picker_row(const PopupScene& scene, u32 index, const PopupItemLayout& layout,
                     UiTextMeasureCache* measure) noexcept {
    const PopupItem& item = scene.items[index];
    PickerRow row{};
    Utf8View summary{};
    Utf8View* parts[] = {&row.description, &row.source, &summary};
    for (u32 at = 0, begin = 0, part = 0; at <= item.detail.bytes && part < 3; ++at)
        if (at == item.detail.bytes || item.detail.data[at] == '\x1F') {
            *parts[part++] = {item.detail.data + begin, at - begin};
            begin = at + 1;
        }
    const UiRect r = layout.row->rectangle;
    const float left = picker_text_left(item, *layout.row);
    const float top = r.top + kPickerRowPadding, bottom = r.bottom - kPickerRowPadding;
    float right = r.right - kPickerRowPadding;
    if (row.source.bytes) {
        // The app column takes its text's width, at most 40% of the row.
        const float width = picker_measure(measure, scene, row.source, kTextStyleSizeCodeXs,
                                           (right - r.left - kPickerRowPadding) * .4f, 0x4353);
        row.source_rectangle = {right - width, top, right, bottom};
        right = maximum(left, right - width - kPickerImageGap);
    }
    row.title = {left, top, right, top + kPickerTitleLine};
    row.description_rectangle = {left, row.title.bottom + kPickerLineGap, right, bottom};
    if (!summary.bytes || scene.selected_index != index)
        return row;
    row.summary = true;
    // Without a shaper the name and summary take bounded halves, never byte estimates.
    const float name = measure ? picker_measure(measure, scene, item.label, text_style_bold, right - left, 0x4352)
                               : (right - left) * .5f;
    float x = minimum(right, left + name);
    row.title.right = x;
    u32 parts_count = 0;
    for (u32 at = 0, begin = 0; at <= summary.bytes; ++at)
        if (at == summary.bytes || summary.data[at] == '\x1E') {
            const Utf8View part{summary.data + begin, at - begin};
            begin = at + 1;
            if (part.bytes && part.data[0] == '+')
                row.optional = part;
            else if (part.bytes && row.chip_count < kPickerChipCapacity)
                row.chips[row.chip_count++] = part;
            ++parts_count;
        }
    const float slot = parts_count ? maximum(0.f, right - x) / static_cast<float>(parts_count) : 0.f;
    const float chip_top = (row.title.top + row.title.bottom - kPickerChipHeight) * .5f;
    for (u32 n = 0; n < row.chip_count; ++n) {
        const float text = measure ? picker_measure(measure, scene, row.chips[n], kTextStyleSizeCodeSm,
                                                    maximum(1.f, right - x), 0x434F)
                                   : maximum(0.f, slot - kPickerChipGap - 2 * kPickerChipPadding);
        const float chip_left = minimum(right, x + kPickerChipGap);
        const float chip_right = minimum(right, chip_left + text + 2 * kPickerChipPadding);
        row.chip_rectangles[n] = {chip_left, chip_top, chip_right, chip_top + kPickerChipHeight};
        x = chip_right;
    }
    if (row.optional.bytes) {
        row.divider = minimum(right, x + kPickerChipGap);
        row.optional_rectangle = {minimum(right, row.divider + 1 + kPickerChipGap), row.title.top, right,
                                  row.title.bottom};
    }
    return row;
}
bool paint_picker_item(const PopupScene& scene, u32 index, const PopupItemLayout& layout, const Theme& theme,
                       PaintStream* paint, UiTextMeasureCache* measure) noexcept {
    const PopupItem& item = scene.items[index];
    const UiRect r = layout.row->rectangle;
    if (!paint_stream_push_clip(paint, layout.clip_rectangle))
        return false;
    bool ok = true;
    const bool pointed = scene.selected_index == index && (item.flags & popup_item_enabled);
    if (item.flags & popup_item_command_category) {
        // Built-in's extra 8 DIPs hold the divider unless only Frequently Used
        // (itself a built-in section) leads (845207 `ei`).
        const bool after_clock = index && (scene.items[index - 1].flags & popup_item_command_category) &&
                                 scene.items[index - 1].presentation.icon_plus_one &&
                                 ui_menu_row_icon(scene.items[index - 1].presentation) == UiIcon::recent;
        if ((item.flags & popup_item_separator_before) && !after_clock)
            ok = paint_stream_push(paint, r.left, r.top - kCommandRailDivider * .5f, r.right,
                                   r.top - kCommandRailDivider * .5f + 1, theme.tokens.border.subtle);
        const bool active = (item.flags & popup_item_checked) != 0;
        if (ok && (active || pointed))
            ok = paint_stream_push_rounded(paint, r, active ? theme.base_low : theme.tokens.state.hover,
                                           theme.scale.radius_md);
        if (ok && picker_keyboard_focus(scene, index))
            ok = paint_stream_push_rounded_border(paint, r.left - 2, r.top - 2, r.right + 2, r.bottom + 2, Color{},
                                                  theme.tokens.border.focus, theme.scale.radius_md + 2, 2);
        if (ok && (item.flags & popup_item_avatar))
            ok = paint_stream_push_rounded(paint, r, theme.interactive, picker_media_radius(scene, index, r));
        else if (ok && item.presentation.icon_plus_one) {
            const float inset = (kCommandRailIcon - kPickerRailGlyph) * .5f;
            ok = ui_icon_paint(paint, ui_menu_row_icon(item.presentation),
                               {r.left + inset, r.top + inset, r.right - inset, r.bottom - inset},
                               active ? theme.text_primary : pointed ? theme.text_primary : theme.text_secondary);
        }
    } else if (item.flags & popup_item_command_section) {
        const UiRect icon = picker_media_rectangle(item, *layout.row);
        if (item.flags & popup_item_avatar)
            ok = paint_stream_push_rounded(paint, icon, theme.interactive, kPickerHeaderIcon * .5f);
        else if (item.presentation.icon_plus_one)
            ok = ui_icon_paint(paint, ui_menu_row_icon(item.presentation), icon, theme.text_primary);
    } else if (popup_item_placeholder(item)) {
        const bool command = (item.flags & popup_item_command_row) != 0;
        // The Loading row's bar sits 8 DIPs down its text line.
        ok = paint_placeholder(paint, item,
                               {r.left + kPickerRowPadding, r.top + kPickerRowPadding + (command ? 0.f : kPickerBarGap),
                                r.right - kPickerRowPadding,
                                command ? r.bottom - kPickerRowPadding : r.top + kPickerRowPadding + kPickerBarGap +
                                                                             kPickerBar},
                               command, theme);
    } else if (!(item.flags & popup_item_command_row)) {
        // A Generic row (the selected command's option and choice lists).
        if (pointed)
            ok = paint_stream_push_rounded(paint, r, ui_background_hover_color(theme, theme.surface_high, .28f),
                                           theme.scale.radius_sm);
    } else {
        if (pointed)
            ok = paint_stream_push_rounded(paint, r, ui_background_hover_color(theme, theme.surface_high, .28f),
                                           theme.scale.radius_sm);
        if (ok && (item.flags & popup_item_avatar))
            ok = paint_stream_push_rounded(paint, picker_media_rectangle(item, *layout.row), theme.interactive,
                                           kPickerImage * .5f);
        else if (ok && item.presentation.icon_plus_one)
            ok = ui_icon_paint(paint, ui_menu_row_icon(item.presentation), picker_media_rectangle(item, *layout.row),
                               theme.text_secondary);
        // The usage summary's chips (the composer option chip's fill) and the
        // divider before the optional count.
        const PickerRow row = ok && measure ? picker_row(scene, index, layout, measure) : PickerRow{};
        for (u32 n = 0; ok && n < row.chip_count; ++n)
            if (ui_rect_has_area(row.chip_rectangles[n]))
                ok = paint_stream_push_rounded(paint, row.chip_rectangles[n], theme.tokens.option.fill,
                                               theme.scale.radius_sm);
        if (ok && row.optional.bytes && ui_rect_has_area(row.optional_rectangle))
            ok = paint_stream_push(paint, row.divider, (row.title.top + row.title.bottom - kPickerChipHeight) * .5f,
                                   row.divider + 1, (row.title.top + row.title.bottom + kPickerChipHeight) * .5f,
                                   theme.tokens.border.subtle);
    }
    return paint_stream_pop_clip(paint) && ok;
}
// One picker text run clipped to its item.
bool push_picker_text(UiTextOutput* text, const PopupScene& scene, u32 index, const PopupItemLayout& layout,
                      Utf8View value, UiRect rectangle, Color color, u32 flags,
                      UiTextAlignment horizontal = UiTextAlignment::start) noexcept {
    if (!value.bytes || rectangle.right <= rectangle.left)
        return true;
    return ui_text_output_push(text, {value,
                                      {&scene.widgets, scene.rows[index].label},
                                      rectangle,
                                      ui_rect_intersection(layout.clip_rectangle, rectangle),
                                      color,
                                      flags,
                                      horizontal,
                                      UiTextAlignment::center,
                                      TextWrapping::no_wrap,
                                      TextOverflow::ellipsis});
}
// Section headers, the query title and command rows.
bool picker_text(const PopupScene& scene, u32 index, const Theme& theme, UiTextOutput* text,
                 UiTextMeasureCache* measure) noexcept {
    const PopupItem& item = scene.items[index];
    PopupItemLayout layout{};
    if (!popup_scene_resolve_item_layout(scene, index, &layout))
        return false;
    if (!layout.visible || (item.flags & popup_item_command_category) || popup_item_placeholder(item))
        return true;
    const UiRect r = layout.row->rectangle;
    const float left = picker_text_left(item, *layout.row);
    if (item.flags & popup_item_command_section) {
        if (picker_discovery(scene))
            return push_picker_text(text, scene, index, layout, item.label,
                                    {left, r.top, r.right - kPickerRowPadding, r.bottom}, theme.text_primary,
                                    text_style_bold | kTextStyleSizeCodeSm);
        // The client's title ("COMMANDS MATCHING /QUERY"), with an optional
        // emphasised detail after it.
        constexpr u32 flags = text_style_bold | kTextStyleSizeCodeXs;
        const float right = r.right - kPickerRowPadding;
        const float width = item.detail.bytes ? picker_measure(measure, scene, item.label, flags,
                                                               (right - left) * .5f, 0x4354)
                                              : right - left;
        return push_picker_text(text, scene, index, layout, item.label, {left, r.top, left + width, r.bottom},
                                theme.text_secondary, flags) &&
               push_picker_text(text, scene, index, layout, item.detail, {left + width, r.top, right, r.bottom},
                                theme.text_primary, flags);
    }
    if (!(item.flags & popup_item_enabled))
        return push_picker_text(text, scene, index, layout, item.label,
                                {left, r.top + kPickerRowPadding, r.right - kPickerRowPadding,
                                 r.bottom - kPickerRowPadding},
                                theme.text_secondary, text_style_none);
    if (!(item.flags & popup_item_command_row)) {
        // The Generic row: its text, then the description right-aligned and
        // muted in the rest of the line (.autocompleteRowContentSecondary__13533:
        // 16 DIPs after the text, at least ten characters).
        const float right = r.right - kPickerRowPadding;
        const float detail_minimum = item.detail.bytes ? 70.f : 0.f;
        const float width = picker_measure(measure, scene, item.label, text_style_none,
                                           maximum(1.f, right - left - detail_minimum), 0x4347);
        const float detail_left = minimum(right, left + width + kPickerImageGap);
        return push_picker_text(text, scene, index, layout, item.label, {left, r.top, left + width, r.bottom},
                                theme.text_primary, text_style_none) &&
               push_picker_text(text, scene, index, layout, item.detail, {detail_left, r.top, right, r.bottom},
                                theme.text_secondary, kTextStyleSizeCodeXs, UiTextAlignment::end);
    }
    const PickerRow row = picker_row(scene, index, layout, measure);
    bool ok = push_picker_text(text, scene, index, layout, row.source, row.source_rectangle, theme.text_secondary,
                               kTextStyleSizeCodeXs, UiTextAlignment::end) &&
              push_picker_text(text, scene, index, layout, item.label, row.title, theme.text_primary,
                               text_style_bold) &&
              push_picker_text(text, scene, index, layout, row.description, row.description_rectangle,
                               theme.text_secondary, kTextStyleSizeCodeXs);
    for (u32 n = 0; ok && n < row.chip_count; ++n) {
        const UiRect chip = row.chip_rectangles[n];
        ok = push_picker_text(text, scene, index, layout, row.chips[n],
                              {chip.left + kPickerChipPadding, chip.top, chip.right - kPickerChipPadding, chip.bottom},
                              theme.text_primary, kTextStyleSizeCodeSm);
    }
    return ok && push_picker_text(text, scene, index, layout, row.optional, row.optional_rectangle,
                                  theme.text_secondary, kTextStyleSizeCodeSm);
}
} // namespace

bool popup_presentation_build(const PopupScene& scene, const Theme& theme, PaintStream* paint,
                              UiTextMeasureCache* measure) noexcept {
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
    bool success = ui_surface_paint_plate(paint, panel->rectangle, theme, UiElevation::popover, panel_radius);
    const bool picker = picker_scene(scene);
    // The picker rail is the client's darkest surface, following the panel's left corners.
    if (picker && picker_discovery(scene))
        success = success && paint_stream_push_edge_strip(paint, panel->rectangle, kCommandRailWidth,
                                                          theme.base_lowest, panel_radius);
    // Rows sit the panel padding inside its corners, so their fills share its corner centres.
    const float row_radius = ui_inner_radius(panel_radius, popup_metrics(scene).menu_padding);
    for (u32 index = 0; success && index < scene.item_count; ++index) {
        const PopupItem& item = scene.items[index];
        PopupItemLayout item_layout{};
        if (!popup_scene_resolve_item_layout(scene, index, &item_layout)) {
            success = false;
            break;
        }
        if (!item_layout.visible) {
            continue;
        }
        if (picker) {
            success = paint_picker_item(scene, index, item_layout, theme, paint, measure);
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
                                              highlight, row_radius, 0.0f)) {
            success = false;
            break;
        }
        if (item.flags & popup_item_avatar) {
            const auto avatar = popup_avatar_rectangle(*layout);
            if (!paint_stream_push_rounded(paint, avatar.left, avatar.top, avatar.right, avatar.bottom,
                                           theme.interactive, theme.scale.radius_lg) ||
                (!(item.flags & popup_item_command_row) && !paint_popup_presence(
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
        if (!paint_menu_row_icon(scene, index, theme, paint) ||
            !paint_menu_row_check(scene, index, theme, paint)) {
            success = false;
        }
    }
    const auto feedback = popup_feedback_rectangle(scene);
    if (ui_rect_has_area(feedback))
        success = paint_stream_push(paint, feedback.left, feedback.top, feedback.right, feedback.top + 1,
                                    theme.surface_high) &&
                  success;
    if (const UiRect track = popup_feedback_slider(scene); ui_rect_has_area(track)) {
        const UiSettingsThemeTokens tokens(theme);
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
    // The picker list's thin scrollbar over its body column (the owner
    // publishes the virtual offset and content height).
    if (picker && scene.picker_scroll_extent > 0)
        success = ui_scrollbar_paint(paint,
                                     ui_scrollbar_geometry(popup_scene_picker_body(scene), scene.picker_scroll_offset,
                                                           scene.picker_scroll_extent),
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
    const bool picker = popup_items_command_picker({scene.items.data, scene.item_count});
    for (u32 index = 0; index < scene.item_count; ++index) {
        const PopupItem& item = scene.items[index];
        if (picker) {
            if (!picker_text(scene, index, theme, text, measure))
                return false;
            continue;
        }
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
        const bool shared_row = popup_item_menu_row(item);
        UiMenuRowGeometry menu_geometry{};
        if (shared_row) {
            if (!popup_scene_menu_row_geometry(scene, index, &menu_geometry))
                return false;
            label_rectangle = menu_geometry.primary;
        }
        if (!shared_row && (item.flags & (popup_item_avatar | popup_item_emoji_preview)))
            label_rectangle.left = item_layout.row->rectangle.left + 48;
        const Utf8View detail_text = item.detail;
        if (detail_text.bytes) {
            Color detail_color = theme.text_primary;
            detail_color.alpha *= .65f;
            UiRect detail = shared_row ? menu_geometry.detail : label_rectangle;
            if (!shared_row) {
                detail.left = detail.right - minimum(180.0f, ui_rect_width(label_rectangle) * .42f);
                label_rectangle.right = maximum(label_rectangle.left, detail.left - 12);
            }
            if (!ui_text_output_push(
                    text,
                    {detail_text,
                     {&scene.widgets, row.label},
                     detail,
                     shared_row ? ui_menu_row_slot_clip(menu_geometry, detail) : item_layout.clip_rectangle,
                     detail_color,
                     text_style_none,
                     shared_row && popup_item_menu_presentation(item).secondary_line
                         ? UiTextAlignment::start
                         : UiTextAlignment::end,
                     UiTextAlignment::center,
                     TextWrapping::no_wrap,
                     TextOverflow::ellipsis}))
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
                                      TextWrapping::no_wrap,
                                      TextOverflow::ellipsis}))
                return false;
        }
        if (!ui_text_output_push(
                text, UiTextCommand{
                          item.label, UiWidgetReference{&scene.widgets, row.label}, label_rectangle,
                          shared_row ? ui_menu_row_slot_clip(menu_geometry, label_rectangle)
                                     : item_layout.clip_rectangle,
                          color,
                          (item.flags & popup_item_emoji) != 0 ? text_style_picker_emoji : text_style_none,
                          centered ? UiTextAlignment::center : UiTextAlignment::start,
                          UiTextAlignment::center, TextWrapping::no_wrap, TextOverflow::ellipsis})) {
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
                                        TextWrapping::no_wrap,
                                        TextOverflow::ellipsis}))
            return false;
    }
    return true;
}

PopupMediaPresentationStatus popup_media_presentation_resolve(const PopupScene& scene, u32 index,
                                                              PopupMediaCommand* command) noexcept {
    if (command) {
        reset_value<PopupMediaCommand>(command);
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
    if (command->avatar || (item.flags & popup_item_emoji_preview)) {
        const bool picker = picker_scene(scene);
        command->rectangle = picker ? picker_media_rectangle(item, *layout.row) : popup_avatar_rectangle(*layout.row);
        if (picker && command->avatar && (item.flags & popup_item_command_category))
            command->corner_radius = picker_media_radius(scene, index, command->rectangle);
    }
    return PopupMediaPresentationStatus::ready;
}
} // namespace maicord
