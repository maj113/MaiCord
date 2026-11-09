#include "scene_internal.hpp"
#include "maicord/ui/theme/foreground_color.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/ui/widgets/kit.hpp"
#include "maicord/ui/widgets/badge.hpp"
#include "maicord/ui/theme/background_surface.hpp"
#include "maicord/ui/guild/card_scene.hpp"

namespace maicord {
using namespace settings_scene_detail;

namespace settings_scene_detail {
namespace {
constexpr float kButtonGroupGap = 4;
constexpr u32 text_label = kUiKitTextBody, text_description = kUiKitTextSmall, text_heading = kUiKitTextBodyBold,
              text_button = kUiKitTextLabelBold, text_eyebrow = kUiKitTextEyebrow, text_value = kUiKitTextSmallBold,
              text_dropdown = kUiKitTextLabel;
// The shared spinner clock runs this many phases per turn; the meter's moving bar reads it as a fraction.
constexpr float kSpinnerPhases = 32;

bool field_row(UiSettingsControlKind kind) noexcept {
    return kind != UiSettingsControlKind::category && kind != UiSettingsControlKind::notice &&
           kind != UiSettingsControlKind::tabs && kind != UiSettingsControlKind::error &&
           kind != UiSettingsControlKind::status && kind != UiSettingsControlKind::disclosure &&
           kind != UiSettingsControlKind::markdown && kind != UiSettingsControlKind::footnote &&
           kind != UiSettingsControlKind::preview;
}
u32 default_choice(const UiSettingsPageContent& content, u32 index) noexcept {
    const auto* p = presentation(content, index);
    return p && p->default_choice != kUiSettingsNoDefault ? p->default_choice : kUiSettingsNoChoice;
}
UiSettingsVariant variant(const UiSettingsPageContent& content, u32 index) noexcept {
    const auto* p = presentation(content, index);
    return p && p->variant <= UiSettingsVariant::positive ? p->variant : UiSettingsVariant::standard;
}
// The option that shows the focus ring: the selected one, else the first.
u32 focus_choice(const UiSettingsControl& c) noexcept {
    return c.selected_choice < c.choices.count ? c.selected_choice : 0;
}
// The state of option `i` of a choice row: hovered under the pointer, ringed when it takes the focus.
UiControlState choice_state(const UiSettingsScene& scene, const UiControlState& state, const UiSettingsControl& c,
                            u32 i) noexcept {
    UiControlState option = hover_option(scene, state, i);
    option.focused = state.focused && i == focus_choice(c);
    return option;
}
UiSettingsButtonVariant button_variant(const UiSettingsPageContent& content, u32 index) noexcept {
    const auto tone = variant(content, index);
    return tone == UiSettingsVariant::critical  ? UiSettingsButtonVariant::critical
           : tone == UiSettingsVariant::primary ? UiSettingsButtonVariant::primary
                                                : UiSettingsButtonVariant::secondary;
}
// The icon a status tile shows (its presentation's icon, else `fallback`). On a palette, an icon marks the
// last tile as an action tile (e.g. Appearance's Browse): a neutral surface with that icon, not a theme.
UiIcon row_icon(const UiSettingsPageContent& content, u32 index, UiIcon fallback) noexcept {
    const auto* p = presentation(content, index);
    return p && p->icon_plus_one && p->icon_plus_one - 1 < static_cast<u32>(UiIcon::count)
               ? static_cast<UiIcon>(p->icon_plus_one - 1)
               : fallback;
}
UiChipTone chip_tone_of(u8 tone) noexcept {
    return tone == ui_settings_chip_positive ? UiChipTone::positive
           : tone == ui_settings_chip_brand  ? UiChipTone::brand
           : tone == ui_settings_chip_warning ? UiChipTone::warning
                                              : UiChipTone::neutral;
}
// A disclosure's tag takes its row's variant as the chip tone.
UiChipTone tag_tone(UiSettingsVariant variant) noexcept {
    return variant == UiSettingsVariant::primary    ? UiChipTone::brand
           : variant == UiSettingsVariant::positive ? UiChipTone::positive
           : variant == UiSettingsVariant::critical ? UiChipTone::dev
                                                    : UiChipTone::neutral;
}
} // namespace
// An avatar row whose image the renderer draws (hashed identities, or a
// default avatar: profile_image, hash = CDN path). It shows a plain loading
// placeholder that the image hides, as member list and message avatars do,
// never initials that a transparent image would show through.
bool avatar_backed(const UiSettingsPageContent& content, u32 index) noexcept {
    // Action rows, edit cells (a Server Profile trait's custom emoji on its emoji button) and a banner row's upload
    // tile (its custom banner, without the avatar flag).
    const auto kind = content.controls[index].kind;
    const bool upload = kind == UiSettingsControlKind::swatches && more(content, index, ui_settings_more_upload_tile);
    if (!content.avatar_media || (!upload && !has_flag(content, index, ui_settings_presentation_avatar)) ||
        (kind != UiSettingsControlKind::action && !upload &&
         !(kind == UiSettingsControlKind::edit && more(content, index, ui_settings_more_cell))))
        return false;
    const auto& media = content.avatar_media[index];
    // Emoji and sticker previews are named by their ID alone.
    if (media.key.kind == MediaAssetKind::custom_emoji || media.key.kind == MediaAssetKind::sticker)
        return media.key.id != 0;
    // A profile image may name its picture by URL (a game's cover).
    if (media.key.kind == MediaAssetKind::profile_image && media.url.bytes)
        return media.key.id != 0;
    return (media.key.kind == MediaAssetKind::user_avatar || media.key.kind == MediaAssetKind::guild_icon ||
            media.key.kind == MediaAssetKind::profile_image) &&
           media.key.id && media.hash_bytes && media.hash_bytes <= kUiSettingsAvatarHashBytes;
}
namespace {
// Host colour for a swatch, chip or accent; false when none.
bool resolve_color(const UiSettingsScene& scene, u64 control_id, u32 choice, Color* color) noexcept {
    BackgroundTheme theme{};
    if (!scene.palette.resolve || !scene.palette.resolve(scene.palette.context, control_id, choice, &theme) ||
        !theme.color_count)
        return false;
    *color = color_from_rgb24(theme.colors[0]);
    return true;
}
// A field shows its error state when its row is critical or an error row
// follows it.
bool field_error(const UiSettingsPageContent& content, u32 index) noexcept {
    const auto* p = presentation(content, index);
    return (p && p->variant == UiSettingsVariant::critical) ||
           (index + 1 < content.controls.count && content.controls[index + 1].kind == UiSettingsControlKind::error);
}
// The count column's glyph after its text.
UiRect list_count_icon(const UiSettingsLayoutTokens& t, const UiSettingsControl& c, const UiRect& column) noexcept {
    const float size = t.metric.list_icon * .75f, cy = (column.top + column.bottom) * .5f;
    const float text = c.choices.count ? ui_settings_text_width(c.choices[0], kUiSettingsLabelEm) * 1.04f : 0;
    const float left = minimum(column.left + text + t.metric.chip_gap, column.right - size);
    return {left, cy - size * .5f, left + size, cy + size * .5f};
}
} // namespace
// One option of a tabs, buttons or colors row inside its control rectangle;
// colours report their trailing button as index choices.count.
UiRect option_rect(const UiSettingsLayoutTokens& t, const UiSettingsControl& c, const UiRect& field, u32 index,
                   const UiKitMeasure* measure) noexcept {
    float x = field.left;
    if (c.kind == UiSettingsControlKind::tabs || c.kind == UiSettingsControlKind::buttons) {
        const bool tabs = c.kind == UiSettingsControlKind::tabs;
        for (u32 i = 0; i < c.choices.count; ++i) {
            const float w = tabs ? ui_settings_text_width(c.choices[i], kUiSettingsLabelEm) * 1.04f
                                 : ui_settings_button_width(t, UiSettingsButtonSize::compact,
                                                            button_label_width(measure, c.choices[i]));
            if (i == index)
                return {x, field.top, minimum(field.right, x + w), field.bottom};
            x += w + (tabs ? kTabGap : kButtonGroupGap);
        }
        return {};
    }
    if (c.kind == UiSettingsControlKind::colors) {
        x += index * (kColorSwatchWidth + kColorSwatchGap);
        if (index < c.choices.count)
            return {x, field.top, x + kColorSwatchWidth, field.top + kColorSwatchHeight};
        if (index == c.choices.count && c.action_label.bytes) {
            const float w = ui_settings_button_width(t, UiSettingsButtonSize::compact,
                                                     button_label_width(measure, c.action_label));
            const float top = field.top + (kColorSwatchHeight - t.metric.button_small) * .5f;
            return {x - kColorSwatchGap + kButtonGroupGap, top, x - kColorSwatchGap + kButtonGroupGap + w,
                    top + t.metric.button_small};
        }
    }
    return {};
}
namespace {
// A list row: hover fill, grip, lead glyph, lock, count glyph and the icon
// buttons (on hover or focus), then a hairline before the next list row.
bool paint_list_row(const UiSettingsScene& scene, const Theme& theme, const UiSettingsTokens& t, PaintStream* paint,
                    const RowLayout& r, u32 index, const UiSettingsListItem& item, UiControlState state,
                    bool highlighted) noexcept {
    const auto& c = scene.content.controls[index];
    const auto& tc = t.color;
    const float alpha = c.enabled ? 1.f : t.disabled_alpha;
    const auto parts = list_parts(t, scene.content, index, r.row);
    const u32 hovered_part = state.hovered ? scene.palette_hovered_index : ~0u;
    UiPaint p{paint, theme};
    if (item.style == UiSettingsListStyle::card) {
        const auto box = parts.content;
        const float radius = t.metric.control_radius;
        const float ring = t.metric.focus_gap + t.metric.focus_width;
        p.rounded_border(box, state.hovered && c.enabled ? tc.mod_normal : tc.mod_subtle, tc.border_subtle, radius, 1)
            .rounded(parts.icon, tc.mod_strong, maximum(radius - t.metric.notice_padding, theme.scale.radius_sm));
        if (item.lead != UiIcon::count)
            p.icon(item.lead, ui_rect_shrink(parts.icon, (ui_rect_width(parts.icon) - t.metric.list_icon) * .5f),
                   ui_settings_color_alpha(tc.text_strong, alpha));
        p.icon(UiIcon::chevron_right, parts.chevron,
               ui_settings_color_alpha(state.hovered ? tc.text_strong : tc.text_subtle, alpha));
        if (state.focused)
            p.use<ui_settings_focus_paint>(t, ui_rect_shrink(box, ring), maximum(0.f, radius - ring));
        return p.ok();
    }
    if (!highlighted && state.hovered && c.enabled)
        p.rounded({r.row.left, r.row.top + theme.scale.space_1, r.row.right, r.row.bottom - theme.scale.space_1},
                  ui_settings_color_alpha(tc.mod_subtle, .7f), t.metric.control_radius);
    // Draggable rows always show their handle; it brightens on hover.
    if (has_flag(scene.content, index, ui_settings_presentation_grip))
        p.use<ui_admin_grip_paint>(t, UiRect{parts.content.left, r.row.top, parts.icon.left, r.row.bottom}, state.hovered,
                                   c.enabled);
    if (p && item.lead != UiIcon::count) {
        // The lead glyph wears the row accent (a role's colour) as ink and a
        // light underlay.
        Color ink = tc.text_subtle;
        if (accent(scene.content, index))
            resolve_color(scene, c.id, kUiSettingsAccentChoice, &ink);
        p.use<ui_icon_paint_styled>(item.lead, parts.icon,
                                    UiIconStyle{ui_settings_color_alpha(ink, alpha),
                                                ui_settings_color_alpha(ink, .35f * alpha), 0});
    }
    if (item.locked)
        p.icon(UiIcon::lock, parts.lock, ui_settings_color_alpha(tc.text_subtle, alpha));
    if (item.count_icon != UiIcon::count)
        p.icon(item.count_icon, list_count_icon(t, c, parts.column),
               ui_settings_color_alpha(hovered_part == ui_settings_list_count ? tc.text_strong : tc.text_subtle, alpha));
    // Icon buttons appear with the pointer or keyboard focus on the row.
    for (u32 i = 0; p && (state.hovered || state.focused) && i < kUiSettingsListActions; ++i) {
        if (item.actions[i] == UiIcon::count)
            continue;
        const auto b = parts.actions[i];
        const bool hot = hovered_part == ui_settings_list_action + i;
        p.rounded(b, hot ? (state.pressed ? tc.mod_strong : tc.mod_normal) : tc.mod_subtle, t.metric.control_radius)
            .icon(item.actions[i], ui_rect_shrink(b, (ui_rect_width(b) - t.metric.list_icon * .75f) * .5f),
                  ui_settings_color_alpha(hot ? tc.text_strong : tc.text_default, alpha));
    }
    // The ring stays inside the row so the list's clip keeps all of it.
    if (state.focused) {
        const float ring = t.metric.focus_gap + t.metric.focus_width;
        p.use<ui_settings_focus_paint>(t, ui_rect_shrink(r.row, ring), t.metric.control_radius);
    }
    // Rows of one list are divided by hairlines inside their content edges.
    if (list_style(scene.content, index + 1, UiSettingsListStyle::row))
        p.fill({parts.content.left, r.row.bottom - 1, parts.content.right, r.row.bottom}, tc.separator);
    return p.ok();
}
// The plate of a whole card, drawn once from the card's first row in view (the body clip trims it).
void paint_card(UiPaint& p, const UiSettingsScene& scene, const UiSettingsTokens& t, const RowLayout& r, u32 index,
                u8 edges, const RowContext& context) noexcept {
    if (!(edges & card_member) || !((edges & card_first) || index == scene.rows_first))
        return;
    float top = r.row.top, bottom = r.row.bottom;
    for (u32 i = index; i > 0 && !(card_edges(scene.content, i, context) & card_first); --i)
        top -= row_start(scene, i) - row_start(scene, i - 1);
    for (u32 i = index; i + 1 < scene.content.controls.count && !(card_edges(scene.content, i, context) & card_last); ++i)
        bottom += row_start(scene, i + 2) - row_start(scene, i + 1);
    p.rounded_border({r.row.left, top, r.row.right, bottom}, t.color.card_fill, t.color.card_border, t.metric.card_radius, 1);
}
// A fill over one row of a card that keeps the card's corners (hover, press).
void paint_row_plate(UiPaint& p, const UiSettingsTokens& t, const UiRect& row, u8 edges, const Color& color) noexcept {
    const float radius = t.metric.card_radius - 1;
    const bool first = (edges & card_first) != 0, last = (edges & card_last) != 0;
    if (first && last) {
        p.rounded(ui_rect_shrink(row, 1), color, radius);
        return;
    }
    const UiRect inner{row.left + 1, row.top, row.right - 1, row.bottom};
    if (!first && !last) {
        p.fill(inner, color);
        return;
    }
    p.clip(row).rounded({inner.left, first ? row.top + 1 : row.top - radius, inner.right,
                         last ? row.bottom - 1 : row.bottom + radius},
                        color, radius)
        .unclip();
}
// A bar: the track and, past zero, the brand fill from its start (at least as wide as it is high).
void paint_bar(UiPaint& p, const UiSettingsTokens& t, const UiRect& bar, float fraction) noexcept {
    const float radius = ui_rect_height(bar) * .5f;
    p.rounded(bar, t.color.slider_track, radius);
    if (fraction > 0)
        p.rounded({bar.left, bar.top, maximum(bar.left + radius * 2, bar.left + ui_rect_width(bar) * fraction), bar.bottom},
                  t.color.brand, radius);
}
// The status card's meter: a determinate bar, or a bar sliding along the track while a check runs.
void paint_meter(UiPaint& p, const UiSettingsScene& scene, const UiSettingsTokens& t, const UiRect& meter,
                 u16 level) noexcept {
    if (level != kUiSettingsIndeterminateMeter)
        return paint_bar(p, t, meter, static_cast<float>(minimum<u16>(level, 1000)) / 1000.f);
    const float radius = ui_rect_height(meter) * .5f, width = ui_rect_width(meter);
    const float bar = width * .35f, at = meter.left - bar + (width + bar) * (scene.spinner_phase / kSpinnerPhases);
    p.rounded(meter, t.color.slider_track, radius);
    p.clip(meter).rounded({at, meter.top, at + bar, meter.bottom}, t.color.brand, radius).unclip();
}

bool paint_row(const UiSettingsScene& scene, const Theme& theme, const UiSettingsTokens& t, PaintStream* paint,
               UiWidgetHandle handle, const Entry& entry, u32 dpi) noexcept {
    const u32 index = entry.source_index;
    const auto& c = scene.content.controls[index];
    RowLayout r{};
    UiRect clip{};
    if (!row_layout_for(scene, t, handle, index, &r, &clip) || !ui_rect_has_area(ui_rect_overlap(r.row, clip)))
        return true;
    const auto& tc = t.color;
    const auto state = widget_state(scene, handle, entry.target, c.enabled);
    const u32 hovered_option = state.hovered ? scene.palette_hovered_index : ~0u;
    UiPaint p{paint, theme};
    const auto context = row_context(scene);
    const u8 edges = card_edges(scene.content, index, context);
    const bool card = (edges & card_member) != 0;
    paint_card(p, scene, t, r, index, edges, context);
    // The flash covers the same box as the selection fill.
    if (scene.flash_control == c.id && scene.flash > 0)
        p.rounded({r.row.left, r.row.top + theme.scale.space_1, r.row.right, r.row.bottom - theme.scale.space_1},
                  ui_settings_color_alpha(tc.brand, .16f * scene.flash), t.metric.control_radius);
    // A row whose whole area is its control (switch, dropdown, disclosure) shows its hover and press plate and,
    // for the keyboard, an inset ring on the row; its control then paints plain.
    const bool plain_line = c.kind == UiSettingsControlKind::disclosure && more(scene.content, index, ui_settings_more_plain);
    const bool row_target = card && c.enabled && !plain_line &&
                            (c.kind == UiSettingsControlKind::toggle || c.kind == UiSettingsControlKind::choice ||
                             c.kind == UiSettingsControlKind::disclosure);
    auto control_state = state;
    if (row_target) {
        if (state.hovered || state.pressed)
            paint_row_plate(p, t, r.row, edges, state.pressed ? tc.mod_normal : tc.row_hover);
        if (state.focused)
            p.rounded_border(ui_rect_shrink(r.row, 1), {}, tc.focus_ring, (edges & (card_first | card_last)) ? t.metric.card_radius : 0.f, 2);
        control_state.focused = false;
    }
    const bool dragged = scene.drag_row == index + 1;
    const bool selected = has_flag(scene.content, index, ui_settings_presentation_selected);
    const UiRect band{r.row.left, r.row.top + 2, r.row.right, r.row.bottom - 2};
    if (selected || dragged)
        p.rounded(band, dragged ? ui_settings_color_alpha(tc.brand, .14f) : tc.mod_subtle, t.metric.control_radius);
    // Only list rows that are themselves the action get a hover fill; a row
    // with its own button highlights just that button.
    else if (c.kind == UiSettingsControlKind::action && has_flag(scene.content, index, ui_settings_presentation_compact) &&
             !c.action_label.bytes && state.hovered && c.enabled && !list_item(scene.content, index))
        p.rounded(band, ui_settings_color_alpha(tc.mod_subtle, .7f), t.metric.control_radius);
    if (scene.drop_row == index + 1 && !dragged) {
        const float y = scene.drop_after ? r.row.bottom - 1 : r.row.top + 1;
        p.rounded({r.row.left, y - 1.5f, r.row.right, y + 1.5f}, tc.brand, 1.5f);
    }
    if (const auto* item = c.kind == UiSettingsControlKind::action ? list_item(scene.content, index) : nullptr)
        return p.ok() && paint_list_row(scene, theme, t, paint, r, index, *item, state, dragged || selected);
    const bool cell = more(scene.content, index, ui_settings_more_cell);
    if (p && c.kind == UiSettingsControlKind::action && ui_rect_has_area(r.lead) && !cell) {
        const float alpha = c.enabled ? 1.f : .5f;
        UiRect lead = r.lead;
        if (has_flag(scene.content, index, ui_settings_presentation_grip)) {
            p.use<ui_admin_grip_paint>(t, lead, state.hovered, c.enabled);
            lead.left += t.metric.grip_width + 4;
        }
        Color fill = tc.brand;
        if (accent(scene.content, index))
            resolve_color(scene, c.id, kUiSettingsAccentChoice, &fill);
        if (has_flag(scene.content, index, ui_settings_presentation_avatar)) {
            const UiRect avatar = avatar_rect(t.metric, scene.content, index, r.lead);
            // The renderer hides this placeholder under the loaded image.
            if (avatar_backed(scene.content, index))
                p.rounded(avatar, theme.avatar_placeholder, ui_rect_width(avatar) * .5f);
            else
                p.use<ui_admin_avatar_paint>(t, avatar, fill, alpha);
        } else if (accent(scene.content, index)) {
            // The reserved lead always shows its dot: host colour, else brand.
            // Keep the dot on the label line when a description follows.
            p.use<ui_admin_dot_paint>(t, UiRect{lead.left, r.label.top, lead.right, r.label.bottom}, fill, alpha);
        }
    }
    if (p && c.kind == UiSettingsControlKind::action && ui_rect_has_area(r.tag) &&
        more(scene.content, index, ui_settings_more_name_tag)) {
        // The author line's compact chip: the muted fill and the badge leading the tag.
        p.rounded(r.tag, theme.tokens.fill.muted, theme.scale.radius_sm);
        if (const auto* glyph = glyph_tile(scene.content, index, 0)) {
            const float cy = (r.tag.top + r.tag.bottom) * .5f, x = r.tag.left + kUiBadgeCompactPadding;
            p.use<ui_icon_paint_styled>(glyph->icon, UiRect{x, cy - kUiBadgeCompactIcon * .5f, x + kUiBadgeCompactIcon,
                                                            cy + kUiBadgeCompactIcon * .5f},
                                        UiIconStyle{color_from_rgb24(glyph->ink), color_from_rgb24(glyph->fill), 0});
        }
    }
    if (p && c.kind != UiSettingsControlKind::status && c.kind != UiSettingsControlKind::preview &&
        ui_rect_has_area(r.chips)) {
        // Accent rows (role lists) lead each chip with its colour dot, a
        // neutral one for colourless roles; plain chips (keybinds) have none.
        const bool dots = accent(scene.content, index);
        for (u32 i = 0; p && i < c.choices.count; ++i) {
            const auto chip = ui_admin_chip_rect(t, c.choices, dots, r.chips, i);
            Color dot = tc.text_subtle;
            const bool has_dot = ui_admin_chip_dot(c.choices[i], dots);
            if (has_dot)
                (void)resolve_color(scene, c.id, i, &dot);
            if (ui_rect_has_area(chip))
                p.use<ui_admin_chip_paint>(t, chip, dot, has_dot, c.enabled ? 1.f : .5f);
        }
    }
    if (progress(scene.content, index) && ui_rect_has_area(r.value))
        paint_bar(p, t, r.value, static_cast<float>(minimum<u16>(scene.content.meters[index], 1000)) / 1000.f);
    // Rows of a card are divided by neutral hairlines inset from its edges; elsewhere rows inside one category
    // are separated by full-width dividers; sections end with a divider halfway through their trailing space.
    const u32 next = index + 1;
    if (card) {
        if (!(edges & card_last))
            p.use<ui_card_hairline_paint>(t, r.row);
    } else if (!scene.content.sectioned && !cell && field_row(c.kind) && next < scene.content.controls.count &&
               field_row(scene.content.controls[next].kind))
        p.fill({r.row.left, r.row.bottom - 1, r.row.right, r.row.bottom}, tc.separator);
    if (scene.content.sectioned && c.kind != UiSettingsControlKind::tabs && section_trailing(scene.content, index) > 0) {
        const float y = r.row.bottom - kSectionGap - 1;
        // A grid's last cell rules the whole line.
        UiRect rule = r.row;
        if (const auto place = cell_place(scene.content, index); place.cell) {
            const float width = ui_rect_width(r.row), gap = t.metric.lead_gap;
            rule.left -= static_cast<float>(place.column) * (width + gap);
            rule.right = rule.left + static_cast<float>(place.columns) * width +
                         static_cast<float>(place.columns - 1) * gap;
        }
        p.fill({rule.left, y, rule.right, y + 1}, tc.separator);
    }
    if (!p)
        return false;
    switch (c.kind) {
    case UiSettingsControlKind::toggle:
        p.use<ui_switch_paint>(t, r.control, c.checked, toggle_motion(scene, entry.target, t, c.checked),
                               scene.reduced_motion, control_state);
        break;
    case UiSettingsControlKind::segments:
        if (has_flag(scene.content, index, ui_settings_presentation_tristate))
            p.use<ui_tristate_paint>(t, r.control, c.selected_choice, hovered_option, state);
        else
            p.use<ui_segmented_paint>(t, r.control, c.choices.count, c.selected_choice, hovered_option, state,
                                      segment_motion(scene, c));
        break;
    case UiSettingsControlKind::swatches:
        if (more(scene.content, index, ui_settings_more_banner_tiles)) {
            // Profile banner presets: each tile the banner's glow of its colour, the chosen one ringed. An upload
            // tile (the last, ui_settings_more_upload_tile) is a plain plate under its picture, or an image glyph.
            const float radius = t.metric.control_radius, ring = theme.scale.space_1;
            const bool upload = more(scene.content, index, ui_settings_more_upload_tile);
            for (u32 i = 0; p && i < c.choices.count; ++i) {
                const auto tile = banner_tile_rect(t, r.control, i);
                const float alpha = c.enabled ? 1.f : t.disabled_alpha;
                if (upload && i + 1 == c.choices.count) {
                    p.rounded(tile, ui_settings_color_alpha(tc.mod_normal, alpha), radius);
                    if (!avatar_backed(scene.content, index)) {
                        const float cx = (tile.left + tile.right) * .5f, cy = (tile.top + tile.bottom) * .5f;
                        const float half = t.metric.list_icon * .5f;
                        p.icon(UiIcon::image, {cx - half, cy - half, cx + half, cy + half},
                               ui_settings_color_alpha(tc.text_subtle, alpha));
                    }
                } else {
                    Color fill = tc.mod_strong;
                    resolve_color(scene, c.id, i, &fill);
                    fill.alpha *= alpha;
                    p.use<ui_guild_profile_banner_paint>(tile, fill, radius, 0.f);
                }
                const auto option = choice_state(scene, state, c, i);
                const bool focused = option.focused && scene.focus_visible;
                if (c.selected_choice == i || focused || (option.hovered && c.enabled))
                    p.rounded_border(ui_rect_shrink(tile, -ring * 2), {},
                                     focused                    ? tc.focus_ring
                                     : c.selected_choice == i ? tc.text_strong
                                                              : ui_settings_color_alpha(tc.text_strong, .4f),
                                     radius + ring * 2, ring);
            }
            break;
        }
        if (more(scene.content, index, ui_settings_more_glyphs)) {
            // Badge tiles: the glyph in its two colours on a neutral tile, the chosen one ringed; a locked set dims
            // its tiles and marks each with a lock.
            const float radius = t.metric.control_radius, ring = theme.scale.space_1 * .5f;
            const float alpha = c.enabled ? 1.f : t.disabled_alpha;
            const float inset = (kGlyphTile - kGlyphTileIcon) * .5f, mark_size = kGlyphTile * .4f;
            for (u32 i = 0; p && i < c.choices.count; ++i) {
                const auto tile = glyph_tile_rect(t, r.control, i);
                const auto option = choice_state(scene, state, c, i);
                const bool focused = option.focused && scene.focus_visible;
                p.rounded(tile, option.hovered && c.enabled ? tc.mod_normal : tc.mod_subtle, radius);
                const auto* glyph = glyph_tile(scene.content, index, i);
                if (!glyph)
                    continue;
                p.use<ui_icon_paint_styled>(glyph->icon, ui_rect_shrink(tile, inset),
                                            UiIconStyle{ui_settings_color_alpha(color_from_rgb24(glyph->ink), alpha),
                                                        ui_settings_color_alpha(color_from_rgb24(glyph->fill), alpha),
                                                        0});
                const UiIcon mark = glyph->mark;
                if (mark != UiIcon::count) {
                    const UiRect corner{tile.right - mark_size, tile.top, tile.right, tile.top + mark_size};
                    p.rounded(corner, tc.mod_strong, mark_size * .5f)
                        .icon(mark, ui_rect_shrink(corner, mark_size * .2f), tc.text_strong);
                }
                if (c.selected_choice == i || focused)
                    p.rounded_border(ui_rect_shrink(tile, -ring * 2), {}, focused ? tc.focus_ring : tc.text_strong,
                                     radius + ring * 2, ring * 2);
            }
            break;
        }
        for (u32 i = 0; p && i < c.choices.count; ++i) {
            Color fill = tc.mod_strong;
            resolve_color(scene, c.id, i, &fill);
            p.use<ui_color_grid_paint>(t, ui_color_grid_cell(t, r.control, i), fill, c.selected_choice == i,
                                       choice_state(scene, state, c, i));
        }
        break;
    case UiSettingsControlKind::tabs: {
        // The brand tab bar: a hairline under every tab and a 2 DIP
        // brand underline under the active one.
        p.fill({r.control.left, r.control.bottom - 1, r.control.right, r.control.bottom}, tc.separator);
        const auto tab = option_rect(t, c, r.control, c.selected_choice, scene.measure);
        if (c.selected_choice < c.choices.count && ui_rect_has_area(tab))
            p.rounded({tab.left, r.control.bottom - 2, tab.right, r.control.bottom}, tc.brand, theme.scale.radius_xs);
        if (state.focused && scene.focus_visible && ui_rect_has_area(tab))
            p.use<ui_settings_focus_paint>(t,
                                           UiRect{tab.left - theme.scale.space_2, tab.top, tab.right + theme.scale.space_2,
                                                  tab.top + kUiSettingsLabelLine},
                                           theme.scale.radius_sm);
        break;
    }
    case UiSettingsControlKind::buttons: {
        // The first button is the primary; a critical row's others are critical-secondary (Remove Icon).
        const auto others = variant(scene.content, index) == UiSettingsVariant::critical
                                ? UiSettingsButtonVariant::critical_secondary
                                : UiSettingsButtonVariant::secondary;
        for (u32 i = 0; p && i < c.choices.count; ++i)
            p.use<ui_settings_button_paint>(t, option_rect(t, c, r.control, i, scene.measure),
                                            i ? others : UiSettingsButtonVariant::primary,
                                            choice_state(scene, state, c, i));
        break;
    }
    case UiSettingsControlKind::colors:
        // Profile theme swatches: the colour with a matching
        // border and a pencil at the top end, its caption below.
        for (u32 i = 0; p && i < c.choices.count; ++i) {
            const auto swatch = option_rect(t, c, r.control, i, scene.measure);
            Color fill = tc.mod_strong;
            resolve_color(scene, c.id, i, &fill);
            if (!c.enabled)
                fill.alpha *= .3f;
            const bool focused = state.focused && scene.focus_visible && i == focus_choice(c);
            const float icon = 14;
            p.rounded_border(swatch, fill, focused ? tc.focus_ring : ui_settings_color_alpha(fill, 1),
                             theme.scale.radius_sm, focused ? 2.f : 1.f)
                .icon(UiIcon::edit, {swatch.right - 4 - icon, swatch.top + 4, swatch.right - 4, swatch.top + 4 + icon},
                      ui_settings_color_alpha(ui_surface_foreground(theme, ui_settings_color_alpha(fill, 1)),
                                              c.enabled ? 1.f : .3f));
        }
        if (c.action_label.bytes)
            p.use<ui_settings_button_paint>(t, option_rect(t, c, r.control, c.choices.count, scene.measure),
                                            UiSettingsButtonVariant::secondary,
                                            hover_option(scene, state, c.choices.count));
        break;
    case UiSettingsControlKind::action:
        if (cell) {
            // A game tile's cover plate (the renderer draws the cover over it; the check and the focus ring come
            // after it, ui_settings_scene_paint_cell_marks); without a cover, a controller glyph.
            const float radius = t.metric.control_radius;
            if (avatar_backed(scene.content, index))
                p.rounded(r.lead, theme.avatar_placeholder, radius);
            else {
                const float cx = (r.lead.left + r.lead.right) * .5f, cy = (r.lead.top + r.lead.bottom) * .5f;
                const float glyph = t.metric.list_icon;
                p.rounded(r.lead, tc.mod_subtle, radius)
                    .icon(UiIcon::controller, {cx - glyph * .5f, cy - glyph * .5f, cx + glyph * .5f, cy + glyph * .5f},
                          ui_settings_color_alpha(tc.text_subtle, c.enabled ? 1.f : t.disabled_alpha));
            }
        } else if (c.action_label.bytes)
            p.use<ui_settings_button_paint>(t, r.control, button_variant(scene.content, index), state);
        break;
    case UiSettingsControlKind::choice:
        p.use<ui_select_paint>(t, r.control, false, control_state, field_error(scene.content, index));
        break;
    case UiSettingsControlKind::edit:
    case UiSettingsControlKind::multiline: {
        auto view = c.editor->view;
        view.active = true;
        view.focused = c.enabled && view.focused && ui_settings_target_equal(scene.focus, entry.target);
        view.caret_visible = view.caret_visible && view.focused;
        UiSettingsControlGeometry geometry{};
        if (!ui_settings_scene_control_geometry(scene, entry.target, &geometry))
            break;
        if (cell) {
            // A trait: the emoji button (a smiley until an emoji is set; the text pass draws a standard emoji, the
            // renderer a server emoji's picture) and the clear button, inside the field.
            p.use<ui_input_shell_paint>(t, r.control, UiControlState{c.enabled, state.hovered, false, view.focused},
                                        field_error(scene.content, index));
            const auto lead = hover_option(scene, state, 1), clear = hover_option(scene, state, 0);
            if (lead.hovered && c.enabled)
                p.rounded(r.lead, tc.mod_subtle, theme.scale.radius_sm);
            if (!c.detail.bytes && !avatar_backed(scene.content, index))
                p.icon(UiIcon::emoji, ui_rect_shrink(r.lead, theme.scale.space_1),
                       lead.hovered && c.enabled ? tc.text_strong : tc.text_subtle);
            if (ui_rect_has_area(r.value))
                p.icon(UiIcon::circle_x, ui_rect_shrink(r.value, theme.scale.space_1),
                       clear.hovered && c.enabled ? tc.text_strong : tc.text_subtle);
            p.use<ui_edit_field_paint_content>(ui_rect_overlap(geometry.text, geometry.clip), view, c.editor->geometry,
                                               theme, dpi);
            break;
        }
        if (ui_rect_has_area(r.value)) {
            UiControlState button = hover_option(scene, state, 0);
            button.focused = false;
            p.use<ui_settings_button_paint>(t, r.value, UiSettingsButtonVariant::primary, button);
        }
        p.use<ui_input_shell_paint>(t, r.control, UiControlState{c.enabled, state.hovered, false, view.focused},
                                    field_error(scene.content, index))
            .use<ui_edit_field_paint_content>(ui_rect_overlap(geometry.text, geometry.clip), view, c.editor->geometry,
                                              theme, dpi);
        break;
    }
    case UiSettingsControlKind::slider: {
        const auto g = ui_slider_geometry(t, r.control, c.choices.count, c.selected_choice, marks(scene.content, index));
        if (scene.content.meters && scene.content.meters[index] != kUiSettingsNoMeter)
            p.use<ui_slider_meter_paint>(t, g, c.choices.count, scene.content.meters[index], state);
        else
            p.use<ui_slider_paint>(t, g, c.choices.count, default_choice(scene.content, index), state);
        break;
    }
    case UiSettingsControlKind::radio:
        for (u32 i = 0; p && i < c.choices.count; ++i)
            p.use<ui_radio_paint>(t, ui_radio_indicator(t, ui_radio_option(t, r.control, i)), c.selected_choice == i,
                                  choice_state(scene, state, c, i));
        break;
    case UiSettingsControlKind::palette: {
        const u32 columns = ui_swatch_columns(t, ui_rect_width(r.control));
        const auto palette = scene.palette;
        const UiIcon action_icon = row_icon(scene.content, index, UiIcon::count);
        for (u32 i = 0; p && i < c.choices.count; ++i) {
            const auto swatch = ui_swatch_rect(t, r.control, i, columns);
            BackgroundTheme colors{};
            const u32 before_resolve = scene.revision;
            const bool resolved = palette.resolve && palette.resolve(palette.context, c.id, i, &colors);
            if (palette.resolve &&
                (!current(scene) || scene.palette.context != palette.context ||
                 scene.palette.resolve != palette.resolve || scene.palette.revision != palette.revision ||
                 scene.revision != before_resolve))
                return false;
            UiControlState option_state = hover_option(scene, state, i);
            option_state.focused = state.focused && c.selected_choice == i;
            const bool action = !resolved && i + 1 == c.choices.count && action_icon != UiIcon::count;
            Color fill = action ? tc.mod_normal : theme.base_lowest;
            if (resolved && background_theme_valid(colors) && colors.color_count >= 2 &&
                colors.colors[0] == colors.colors[1])
                fill = color_from_rgb24(colors.colors[0]);
            const bool gradient = resolved && background_theme_valid(colors) && colors.colors[0] != colors.colors[1];
            p.use<ui_swatch_paint>(t, swatch, fill, gradient ? &colors : nullptr, c.selected_choice == i, option_state);
            if (action) {
                const float cx = (swatch.left + swatch.right) * .5f, cy = (swatch.top + swatch.bottom) * .5f;
                p.icon(action_icon, {cx - 12, cy - 12, cx + 12, cy + 12},
                       ui_settings_color_alpha(option_state.hovered ? tc.text_strong : tc.text_subtle,
                                               c.enabled ? 1.f : t.disabled_alpha));
            }
        }
        break;
    }
    case UiSettingsControlKind::notice: {
        const auto tone = variant(scene.content, index);
        const Color fill = tone == UiSettingsVariant::critical   ? tc.critical_tint
                           : tone == UiSettingsVariant::positive ? ui_settings_color_alpha(tc.positive, .12f)
                                                                 : tc.mod_subtle;
        const UiRect box{r.row.left, r.row.top + t.metric.row_padding * .5f, r.row.right,
                         r.row.bottom - t.metric.row_padding * .5f};
        // Critical notices outline the tinted box, sharing its corners.
        if (tone == UiSettingsVariant::critical)
            p.rounded_border(box, fill, ui_settings_color_alpha(tc.critical_text, .5f), t.metric.control_radius, 1);
        else
            p.rounded(box, fill, t.metric.control_radius);
        if (c.action_label.bytes)
            p.use<ui_settings_button_paint>(t, r.control, UiSettingsButtonVariant::secondary, state);
        if (c.checked) {
            const float size = ui_rect_height(box) - theme.scale.space_4 * 2;
            const float side = minimum(size, 20.f), cy = (box.top + box.bottom) * .5f;
            const float right = box.right - theme.scale.space_4;
            p.spinner({right - side, cy - side * .5f, right, cy + side * .5f}, tc.text_subtle, scene.spinner_phase);
        }
        break;
    }
    case UiSettingsControlKind::category:
        if (link(scene.content, index)) {
            const float icon = 18, cy = (r.label.top + r.label.bottom) * .5f;
            const float ring = theme.scale.space_2;
            p.icon(UiIcon::chevron_right, {r.row.right - icon, cy - icon * .5f, r.row.right, cy + icon * .5f},
                   state.hovered ? tc.text_strong : tc.text_subtle);
            if (state.focused)
                p.rounded_border({r.row.left - ring, r.label.top - ring, r.row.right + ring, r.label.bottom + ring}, {},
                                 tc.focus_ring, theme.scale.radius_sm, 2);
        }
        break;
    case UiSettingsControlKind::status: {
        const auto tone = variant(scene.content, index);
        Color tile = tc.mod_normal, ink = tc.text_default;
        if (tone == UiSettingsVariant::positive)
            tile = tc.positive_tint, ink = tc.positive;
        else if (tone == UiSettingsVariant::primary)
            tile = tc.emphasis_selected, ink = tc.emphasis_outline;
        else if (tone == UiSettingsVariant::critical)
            tile = tc.critical_tint, ink = tc.critical_text;
        const float cx = (r.lead.left + r.lead.right) * .5f, cy = (r.lead.top + r.lead.bottom) * .5f;
        const UiRect glyph{cx - 12, cy - 12, cx + 12, cy + 12};
        p.rounded(r.lead, tile, t.metric.card_radius);
        if (c.checked)
            p.spinner(glyph, ink, scene.spinner_phase);
        else
            p.icon(row_icon(scene.content, index, UiIcon::download), glyph, ink);
        if (ui_rect_has_area(r.chips)) {
            UiRect chips[8];
            float height = 0;
            flow_chips(c, scene.measure, r.chips.left, r.chips.top, ui_rect_width(r.chips), chips, &height);
            for (u32 i = 0; p && i < c.choices.count && i < 8; ++i)
                p.use<ui_chip_paint>(t, chips[i], chip_tone_of(chip_tone(scene.content, index, i)));
        }
        if (ui_rect_has_area(r.meter))
            paint_meter(p, scene, t, r.meter, scene.content.meters[index]);
        if (ui_rect_has_area(r.control)) {
            if (progress_fill(scene.content, index))
                p.use<ui_settings_button_progress_paint>(
                    t, r.control, static_cast<float>(minimum<u16>(scene.content.meters[index], 1000)) / 1000.f, state);
            else
                p.use<ui_settings_button_paint>(
                    t, r.control, tone == UiSettingsVariant::primary ? UiSettingsButtonVariant::primary : UiSettingsButtonVariant::secondary,
                    state);
            // A running action keeps its place: a spinner leads the (disabled) button's label.
            if (c.checked)
                p.spinner({r.control.left + 12, (r.control.top + r.control.bottom) * .5f - 8, r.control.left + 28,
                           (r.control.top + r.control.bottom) * .5f + 8},
                          ui_settings_button_text(t, UiSettingsButtonVariant::secondary, state), scene.spinner_phase);
            else if (status_glyph(scene.content, index)) {
                UiRect glyph;
                status_button_label(scene.measure, c.action_label, r.control, &glyph);
                p.icon(UiIcon::reset, glyph,
                       ui_settings_button_text(t, tone == UiSettingsVariant::primary ? UiSettingsButtonVariant::primary
                                                                                      : UiSettingsButtonVariant::secondary,
                                               state));
            }
        }
        break;
    }
    case UiSettingsControlKind::disclosure:
        if (ui_rect_has_area(r.lead))
            p.icon(c.checked ? UiIcon::chevron_down : UiIcon::chevron_right, r.lead,
                   state.hovered ? tc.text_strong : tc.text_subtle);
        if (ui_rect_has_area(r.tag))
            p.use<ui_chip_paint>(t, r.tag, tag_tone(variant(scene.content, index)));
        break;
    case UiSettingsControlKind::markdown:
    case UiSettingsControlKind::footnote:
        if (c.kind == UiSettingsControlKind::footnote)
            p.icon(UiIcon::info, r.lead, tc.text_subtle);
        // The link at the end keeps a ring for the keyboard.
        if (state.focused && ui_rect_has_area(r.control))
            p.use<ui_settings_focus_paint>(t, ui_rect_shrink(r.control, -theme.scale.space_2), theme.scale.radius_sm);
        break;
    case UiSettingsControlKind::error:
        break;
    case UiSettingsControlKind::preview:
        if (const auto* s = scene.content.preview) {
            p.rounded_border(r.body, tc.card_fill, tc.card_border, t.metric.card_radius, 1);
            if (ui_rect_has_area(r.lead))
                p.rounded(r.lead, tc.brand, ui_rect_width(r.lead) * .5f);
            // Role Colors "Next to names": an 8-DIP dot before each name.
            if (s->role_style == 1 && s->role_rgb) {
                const Color dot = color_from_rgb24(s->role_rgb);
                const UiRect lines[]{s->compact ? r.description : r.tag, s->compact ? r.value : UiRect{}};
                for (const auto& line : lines)
                    if (ui_rect_has_area(line)) {
                        const float middle = (line.top + line.bottom) * .5f;
                        p.rounded({line.left, middle - 4, line.left + 8, middle + 4}, dot, 4);
                    }
            }
            // The first message's reactions: yours (highlighted) and another.
            for (u32 i = 0; ui_rect_has_area(r.chips) && i < 2; ++i) {
                const UiRect pill{r.chips.left + i * 56.f, r.chips.top, r.chips.left + i * 56.f + 52, r.chips.bottom};
                p.rounded_border(pill, i ? tc.mod_subtle : tc.emphasis_selected, i ? tc.card_border : tc.emphasis_outline,
                                 theme.scale.radius_md, 1);
            }
            // Example avatars (idle, do not disturb, online) with their status dots.
            const Color fills[]{tc.brand, tc.mod_strong, tc.positive};
            const Color statuses[]{theme.tokens.status.idle, theme.tokens.status.dnd, theme.tokens.status.online};
            for (u32 i = 0; ui_rect_has_area(r.icon) && i < 3; ++i) {
                const UiRect avatar{r.icon.left + i * 40.f, r.icon.top, r.icon.left + i * 40.f + 32, r.icon.bottom};
                p.rounded(avatar, fills[i], 16)
                    .rounded({avatar.right - 12, avatar.bottom - 12, avatar.right + 2, avatar.bottom + 2}, tc.card_fill, 7)
                    .rounded({avatar.right - 9, avatar.bottom - 9, avatar.right - 1, avatar.bottom - 1}, statuses[i], 4);
            }
            if (ui_rect_has_area(r.control))
                p.use<ui_settings_button_paint>(t, r.control, UiSettingsButtonVariant::primary, decltype(state){});
        }
        break;
    }
    return p.ok();
}
// The sidebar row of widget slot `i` when it is a navigation entry in view:
// its handle, laid-out node and row.
const UiLayoutNode* visible_nav(const UiSettingsScene& scene, u32 i, UiWidgetHandle* handle, NavRow* row) noexcept {
    const auto& entry = entries(scene)[i];
    if (entry.role != EntryRole::nav)
        return nullptr;
    *handle = {static_cast<u16>(i), scene.widgets.generation};
    const auto* node = ui_widget_manager_layout_node(scene.widgets, *handle);
    return node && nav_row(scene, entry.source_index, row) &&
                   ui_rect_has_area(ui_rect_overlap(node->rectangle, node->clip_rectangle))
               ? node
               : nullptr;
}
// The label of a nav badge count: a digit, then "9+".
Utf8View nav_badge_label(u16 badge) noexcept {
    static constexpr char kLabels[] = "123456789+";
    return badge < 10 ? Utf8View{kLabels + badge - 1, 1u} : Utf8View{kLabels + 8, 2u};
}
// The scrolling rows of the sidebar, or with `pinned` the rows of its foot.
bool paint_nav(const UiSettingsScene& scene, const Theme& theme, const UiSettingsTokens& t, PaintStream* paint,
               bool pinned) noexcept {
    UiRect area{};
    if (pinned) {
        area = frame(scene, t.metric).nav_foot;
        if (!ui_rect_has_area(area))
            return true;
    } else {
        const auto* nav = ui_widget_manager_layout_node(scene.widgets, scene.sidebar);
        if (!nav)
            return false;
        area = ui_rect_overlap(nav->rectangle, nav->clip_rectangle);
    }
    UiPaint p{paint, theme};
    if (!p.clip(area))
        return false;
    for (u32 i = 0; p && i < scene.widgets.count; ++i) {
        UiWidgetHandle handle{};
        NavRow row{};
        const auto* node = visible_nav(scene, i, &handle, &row);
        if (!node || row.pinned != pinned || row.kind == UiSettingsNavKind::header)
            continue;
        if (row.kind == UiSettingsNavKind::separator) {
            p.use<ui_settings_separator_paint>(t, node->rectangle);
            continue;
        }
        const bool selected = nav_selected(scene, row);
        const auto state = widget_state(scene, handle, row.target, true);
        p.use<ui_settings_nav_item_paint>(t, node->rectangle, selected, row.destructive, state);
        if (row.icon < UiIcon::count)
            p.icon(row.icon, ui_settings_nav_icon_rect(t, node->rectangle),
                   ui_settings_nav_item_text(t, selected, row.destructive, state));
        if (row.badge) {
            const bool dot = row.badge == kUiSettingsNavDot;
            p.use<ui_settings_nav_badge_paint>(
                t, ui_settings_nav_badge_rect(node->rectangle, dot,
                                              dot ? 0.f : ui_kit_text_width(scene.measure, nav_badge_label(row.badge), kUiKitTextSmallBold)));
        }
    }
    return p.end();
}
} // namespace
} // namespace settings_scene_detail

bool ui_settings_scene_paint(const UiSettingsScene& scene, const Theme& theme, PaintStream* paint, u32 dpi,
                             UiSettingsPaintRanges* ranges) noexcept {
    if (ranges)
        *ranges = {paint ? paint->count : 0, paint ? paint->count : 0};
    if (!scene.active)
        return true;
    if (!paint || !current(scene))
        return false;
    const UiSettingsThemeTokens t(theme);
    const auto& tc = t.color;
    const auto& l = scene.layout;
    const bool dialog = scene.presentation == UiSettingsPresentation::dialog;
    const bool modal = scene.presentation == UiSettingsPresentation::modal || dialog;
    const auto viewport = scene.viewport;
    UiPaint p{paint, theme};
    if (!p.clip(viewport))
        return false;
    if (modal && !l.compact)
        p.fill(viewport, tc.scrim);
    if (ranges)
        ranges->modal_first_quad = paint->count;
    const float radius = modal && !l.compact ? t.metric.modal_radius : 0;
    if (modal) {
        if (!l.compact)
            p.use<ui_settings_shadow_paint>(l.modal, radius, 12.f, .3f);
        p.use<ui_background_paint_surface>(l.modal, theme, tc.modal_background, !l.compact ? tc.border_subtle : Color{},
                                           radius, !l.compact ? 1.f : 0.f, .9f, UiBackgroundSurfaceMode::gradient,
                                           UiBackgroundTier::higher, UiBackgroundBorderTone::palette);
        // The sidebar keeps the modal's rounded left corners and a square inner edge.
        if (!dialog) {
            p.use<ui_background_paint_surface>(l.sidebar, theme, tc.sidebar_background, Color{}, radius, 0.f, .9f,
                                               UiBackgroundSurfaceMode::gradient, UiBackgroundTier::high,
                                               UiBackgroundBorderTone::semantic);
            if (radius > 0)
                p.use<ui_background_paint_surface>(
                    UiRect{l.sidebar.right - radius, l.sidebar.top, l.sidebar.right, l.sidebar.bottom}, theme,
                    tc.sidebar_background, Color{}, 0.f, 0.f, .9f, UiBackgroundSurfaceMode::gradient,
                    UiBackgroundTier::high, UiBackgroundBorderTone::semantic);
        }
    } else {
        // Full-window layers sit on the app canvas with a lifted sidebar region.
        p.use<ui_background_paint_surface>(viewport, theme, theme.base_lowest, Color{}, 0.f, 0.f, 1.f,
                                           UiBackgroundSurfaceMode::gradient, UiBackgroundTier::chat,
                                           UiBackgroundBorderTone::semantic)
            .use<ui_background_paint_surface>(l.sidebar, theme, theme.base_lower, Color{}, 0.f, 0.f, 1.f,
                                              UiBackgroundSurfaceMode::gradient, UiBackgroundTier::app_frame,
                                              UiBackgroundBorderTone::semantic);
    }
    if (p && scene.search_field.generation) {
        if (const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.search_field)) {
            const UiSettingsTarget target{UiSettingsTargetKind::search, {}, 0};
            auto state = widget_state(scene, scene.search_field, target, true);
            state.focused = scene.search.view.focused || state.focused;
            p.use<ui_settings_search_paint>(t, node->rectangle, scene.search.view.text.bytes != 0, state);
            UiSettingsControlGeometry geometry{};
            if (p && scene.search.view.focused && ui_settings_scene_search_geometry(scene, &geometry)) {
                auto view = scene.search.view;
                view.active = true;
                p.use<ui_edit_field_paint>(node->rectangle, geometry.text, view, scene.search.geometry, Color{}, theme,
                                           dpi, t.metric.control_radius);
            }
        }
    }
    if (p && (!paint_nav(scene, theme, t, paint, false) || !paint_nav(scene, theme, t, paint, true)))
        p.fail();
    if (const auto foot_rule = frame(scene, t.metric).nav_rule; p && ui_rect_has_area(foot_rule))
        p.fill(foot_rule, tc.hairline);
    const auto close_state =
        widget_state(scene, scene.close, UiSettingsTarget{UiSettingsTargetKind::close, {}, 0}, true);
    if (dialog)
        p.use<ui_settings_dialog_close_paint>(t, l.close, close_state);
    else
        p.use<ui_settings_close_paint>(t, l.close, close_state);
    if (const auto* body = ui_widget_manager_layout_node(scene.widgets, scene.body); p && body) {
        p.clip(ui_rect_overlap(body->rectangle, body->clip_rectangle));
        for (u32 i = 0; p && i < scene.widgets.count; ++i) {
            const auto& entry = entries(scene)[i];
            if (entry.role == EntryRole::row && entry.source_index < scene.content.controls.count &&
                !paint_row(scene, theme, t, paint, {static_cast<u16>(i), scene.widgets.generation}, entry, dpi))
                p.fail();
        }
        p.unclip();
    }
    // Under a scrolled page the header band gets a hairline where the rows slide under it.
    if (p && scene.presentation == UiSettingsPresentation::modal && !scene.content.host && scene.content_offset > 0)
        p.fill(frame(scene, t.metric).rule, tc.hairline);
    if (p && scene.footer.visible) {
        if (const auto* footer = ui_widget_manager_layout_node(
                scene.widgets, ui_widget_manager_parent(scene.widgets, scene.footer_status))) {
            const float dx = scene.footer_shift_q / 16.f, alert = scene.footer_alert_q / 255.f;
            const UiRect f{footer->rectangle.left + dx, footer->rectangle.top, footer->rectangle.right + dx,
                           footer->rectangle.bottom};
            if (dialog) {
                // The band takes the modal's lower corners; its top edge stays square.
                const UiRect band = footer->rectangle;
                const Color fill = ui_settings_color_mix(tc.sidebar_background, tc.critical, alert * .5f);
                p.use<ui_background_paint_surface>(band, theme, fill, Color{}, radius, 0.f, .9f,
                                                   UiBackgroundSurfaceMode::gradient, UiBackgroundTier::high,
                                                   UiBackgroundBorderTone::semantic);
                if (radius > 0)
                    p.use<ui_background_paint_surface>(UiRect{band.left, band.top, band.right, band.top + radius}, theme,
                                                       fill, Color{}, 0.f, 0.f, .9f, UiBackgroundSurfaceMode::gradient,
                                                       UiBackgroundTier::high, UiBackgroundBorderTone::semantic);
            } else {
                p.use<ui_settings_shadow_paint>(f, t.metric.control_radius, 6.f, .25f)
                    .rounded_border(f, ui_settings_color_mix(theme.base_lowest, tc.critical, alert),
                                    scene.footer.failed || alert > 0 ? tc.critical_text : tc.border_subtle,
                                    t.metric.control_radius, 1);
            }
            const UiSettingsTarget reset{UiSettingsTargetKind::reset, scene.selected_page, 0};
            const UiSettingsTarget save{UiSettingsTargetKind::save, scene.selected_page, 0};
            const auto moved = [dx](UiRect r) noexcept { return UiRect{r.left + dx, r.top, r.right + dx, r.bottom}; };
            if (const auto* reset_node = ui_widget_manager_layout_node(scene.widgets, scene.footer_reset)) {
                const auto state = widget_state(scene, scene.footer_reset, reset, target_exists(scene, reset));
                if (state.hovered || state.focused)
                    p.use<ui_settings_button_paint>(t, moved(reset_node->rectangle), UiSettingsButtonVariant::secondary,
                                                    state);
            }
            if (const auto* save_node = ui_widget_manager_layout_node(scene.widgets, scene.footer_save))
                p.use<ui_settings_button_paint>(t, moved(save_node->rectangle), UiSettingsButtonVariant::primary,
                                                widget_state(scene, scene.footer_save, save, target_exists(scene, save)));
        }
    }
    p.scrollbar(ui_settings_scene_scrollbar(scene, true)).scrollbar(ui_settings_scene_scrollbar(scene, false));
    if (ranges)
        ranges->modal_end_quad = paint->count;
    return p.end();
}

bool ui_settings_scene_paint_cell_marks(const UiSettingsScene& scene, const Theme& theme, PaintStream* paint) noexcept {
    if (!scene.active)
        return true;
    if (!paint || !current(scene))
        return false;
    const UiSettingsThemeTokens t(theme);
    const auto& tc = t.color;
    const auto* body = ui_widget_manager_layout_node(scene.widgets, scene.body);
    if (!body)
        return true;
    UiPaint p{paint, theme};
    p.clip(ui_rect_overlap(body->rectangle, body->clip_rectangle));
    for (u32 i = 0; p && i < scene.widgets.count; ++i) {
        const auto& entry = entries(scene)[i];
        const u32 index = entry.source_index;
        if (entry.role != EntryRole::row || index >= scene.content.controls.count)
            continue;
        const auto& c = scene.content.controls[index];
        const UiWidgetHandle handle{static_cast<u16>(i), scene.widgets.generation};
        RowLayout r{};
        UiRect clip{};
        if (c.kind == UiSettingsControlKind::swatches && more(scene.content, index, ui_settings_more_upload_tile) &&
            c.choices.count && row_layout_for(scene, t, handle, index, &r, &clip)) {
            // The official upload tile: hovering it (enabled) shows the scrim and a 20 DIP pencil over it.
            const auto tile = banner_tile_rect(t, r.control, c.choices.count - 1);
            // The pointer's option (palette_hovered_index, set only over the hovered row's options) is the upload tile.
            if (!c.enabled || scene.palette_hovered_index + 1 != c.choices.count ||
                !ui_rect_has_area(ui_rect_overlap(tile, clip)))
                continue;
            const float cx = (tile.left + tile.right) * .5f, cy = (tile.top + tile.bottom) * .5f;
            const float half = t.metric.list_icon * .5f * (20.f / 24.f);
            p.rounded(tile, tc.scrim, t.metric.control_radius)
                .icon(UiIcon::edit, {cx - half, cy - half, cx + half, cy + half},
                      ui_surface_foreground(theme, tc.scrim));
            continue;
        }
        if (c.kind != UiSettingsControlKind::action || !more(scene.content, index, ui_settings_more_cell))
            continue;
        if (!row_layout_for(scene, t, handle, index, &r, &clip) || !ui_rect_has_area(ui_rect_overlap(r.row, clip)))
            continue;
        const auto state = widget_state(scene, handle, entry.target, c.enabled);
        const float radius = t.metric.control_radius, ring = theme.scale.space_1;
        // Over the cover: the hover outline or keyboard ring, then a chosen game's check.
        if (state.focused && scene.focus_visible)
            p.rounded_border(ui_rect_shrink(r.lead, -ring * 2), {}, tc.focus_ring, radius + ring * 2, ring);
        else if (state.hovered && c.enabled)
            p.rounded_border(r.lead, {}, ui_settings_color_alpha(tc.text_strong, .4f), radius, ring);
        if (c.checked && ui_rect_has_area(r.icon))
            p.rounded(r.icon, ui_settings_color_alpha(tc.brand, c.enabled ? 1.f : t.disabled_alpha), theme.scale.radius_sm)
                .icon(UiIcon::check, ui_rect_shrink(r.icon, theme.scale.space_1), ui_surface_foreground(theme, tc.brand));
    }
    p.unclip();
    return p.end();
}

namespace settings_scene_detail {
namespace {
void row_text(UiTextPen<>& pen, const UiSettingsScene& scene, const UiSettingsTokens& t, UiWidgetHandle handle,
              const Entry& entry) noexcept {
    const u32 index = entry.source_index;
    const auto& c = scene.content.controls[index];
    RowLayout r{};
    UiRect clip{};
    if (!row_layout_for(scene, t, handle, index, &r, &clip) || !ui_rect_has_area(ui_rect_overlap(r.row, clip)))
        return;
    auto text = pen.in(clip);
    const auto& tc = t.color;
    const auto state = widget_state(scene, handle, entry.target, c.enabled);
    const float dim = c.enabled ? 1.f : .5f;
    if (c.kind == UiSettingsControlKind::category) {
        if (c.choices.count && ui_rect_has_area(r.value))
            text(handle, c.choices[0], r.value, tc.text_subtle, text_eyebrow);
        text(handle, c.label, r.label,
             link(scene.content, index) && state.hovered ? tc.text_strong : ui_settings_color_alpha(tc.text_strong, dim),
             text_heading);
        if (ui_rect_has_area(r.tag))
            text(handle, c.detail, r.tag, tc.text_subtle, text_description);
        text(handle, c.description, r.description, tc.text_subtle, text_description, ui_text_wrap);
        return;
    }
    if (c.kind == UiSettingsControlKind::error) {
        text(handle, c.label, r.label, tc.critical_text, text_description, ui_text_wrap);
        return;
    }
    if (c.kind == UiSettingsControlKind::status) {
        text(handle, c.label, r.label, tc.text_strong, kUiKitTextHeading);
        if (more(scene.content, index, ui_settings_more_meta_parts)) {
            // Whole parts flow left to right; the identity leads in bold and the brighter ink.
            StatusMetaPart parts[kStatusMetaParts];
            u32 count = 0;
            status_meta_flow(scene.measure, c.description, r.description, parts, &count);
            for (u32 i = 0; i < count; ++i)
                text(handle, parts[i].text, parts[i].rect, parts[i].lead ? tc.text_default : tc.text_subtle,
                     parts[i].lead ? kUiKitTextSmallBold : text_description);
        } else
            text(handle, c.description, r.description, tc.text_subtle, text_description, ui_text_wrap);
        if (ui_rect_has_area(r.chips)) {
            UiRect chips[8];
            float height = 0;
            flow_chips(c, scene.measure, r.chips.left, r.chips.top, ui_rect_width(r.chips), chips, &height);
            for (u32 i = 0; i < c.choices.count && i < 8; ++i) {
                const auto tone = chip_tone_of(chip_tone(scene.content, index, i));
                text(handle, c.choices[i], ui_chip_label_rect(chips[i]), ui_chip_ink(t, tone),
                     text_description);
            }
        }
        if (ui_rect_has_area(r.control)) {
            const bool filled = progress_fill(scene.content, index);
            UiRect label = r.control;
            u32 mode = ui_text_center;
            // A running action keeps its place: the spinner leads the label. Other actions lead with their glyph.
            if (c.checked)
                label.left += 28;
            else if (status_glyph(scene.content, index)) {
                UiRect glyph;
                label = status_button_label(scene.measure, c.action_label, r.control, &glyph);
                mode = 0;
            }
            text(handle, c.action_label, label,
                 filled ? ui_settings_color_alpha(tc.text_strong, dim)
                        : ui_settings_button_text(t, variant(scene.content, index) == UiSettingsVariant::primary
                                                         ? UiSettingsButtonVariant::primary
                                                         : UiSettingsButtonVariant::secondary,
                                                  state),
                 text_button, mode);
        }
        text(handle, c.detail, r.value, tc.text_subtle, text_description, ui_text_end);
        return;
    }
    if (c.kind == UiSettingsControlKind::disclosure) {
        const bool plain = more(scene.content, index, ui_settings_more_plain);
        text(handle, c.label, r.label, ui_settings_color_alpha(plain ? tc.text_default : tc.text_strong, dim),
             plain ? kUiKitTextLabel : text_heading, plain ? ui_text_wrap : 0);
        text(handle, c.description, r.description, tc.text_subtle, text_description, ui_text_end);
        if (ui_rect_has_area(r.tag)) {
            const auto tone = tag_tone(variant(scene.content, index));
            text(handle, c.detail, ui_chip_label_rect(r.tag), ui_chip_ink(t, tone), text_description);
        }
        return;
    }
    if (c.kind == UiSettingsControlKind::preview && scene.content.preview) {
        const auto& s = *scene.content.preview;
        const UiKitMeasure* m = scene.measure;
        const float em = s.text_size > 0 ? s.text_size : 16.f;
        text(handle, c.label, r.label, tc.text_subtle, kUiKitTextBody);
        const Color name_ink = s.role_style == 0 && s.role_rgb ? color_from_rgb24(s.role_rgb) : tc.text_strong;
        const float dot = s.role_style == 1 && s.role_rgb ? 12.f : 0.f;
        const float name_width = ui_kit_chat_text_width(m, s.author, kUiKitTextBodyBold, em);
        // A line led by the name (compact rows: every line) then text runs, each after the one before. The sample is
        // chat text: the chat face at the chat size.
        const auto runs = [&](const UiRect& line, bool named, const Utf8View* parts, u32 count, bool link_last) noexcept {
            float x = line.left;
            if (named) {
                text(handle, s.author, {x + dot, line.top, minimum(line.right, x + dot + name_width + 2), line.bottom},
                     name_ink, kUiKitTextBodyBold, ui_text_chat);
                x += dot + name_width + 6;
            }
            for (u32 i = 0; i < count && x < line.right; ++i) {
                const bool link = link_last && i + 1 == count;
                const u32 style = kUiKitTextBody | (link && s.underline ? text_style_underline : 0u);
                text(handle, parts[i], {x, line.top, line.right, line.bottom}, link ? tc.link : tc.text_default, style,
                     ui_text_chat);
                x += ui_kit_chat_text_width(m, parts[i], kUiKitTextBody, em);
            }
        };
        if (c.choices.count >= 3) {
            const Utf8View first[]{c.choices[0]}, second[]{c.choices[1], c.choices[2]};
            if (s.compact) {
                text(handle, s.time, r.tag, tc.text_subtle, kUiKitTextSmall, ui_text_end | ui_text_chat);
                runs(r.description, true, first, 1, false);
                runs(r.value, true, second, 2, true);
            } else {
                const char initial[2]{s.author.bytes ? s.author.data[0] : '?', 0};
                text(handle, {initial, 1}, r.lead, tc.on_brand, kUiKitTextBodyBold, ui_text_center);
                runs(r.tag, true, nullptr, 0, false);
                text(handle, s.time, {r.tag.left + dot + name_width + 8, r.tag.top, r.tag.right, r.tag.bottom},
                     tc.text_subtle, kUiKitTextSmall, ui_text_chat);
                runs(r.description, false, first, 1, false);
                runs(r.value, false, second, 2, true);
            }
        }
        // The reactions: 🥀 3 (yours) and 🪤 1. A reaction pill is a fixed box with a fixed count, as in the chat.
        constexpr Utf8View reactions[]{utf8_literal("\xF0\x9F\xA5\x80 3"), utf8_literal("\xF0\x9F\xAA\xA4 1")};
        for (u32 i = 0; ui_rect_has_area(r.chips) && i < 2; ++i)
            text(handle, reactions[i], {r.chips.left + i * 56.f, r.chips.top, r.chips.left + i * 56.f + 52, r.chips.bottom},
                 i ? tc.text_default : tc.emphasis_outline, kUiKitTextLabelBold, ui_text_center);
        text(handle, c.action_label, r.control, tc.on_brand, kUiKitTextLabelBold, ui_text_center);
        return;
    }
    if (c.kind == UiSettingsControlKind::markdown) {
        auto* block = markdown_block(scene.content, index, row_context(scene));
        const auto visible = ui_rect_overlap(r.body, clip);
        if (block && ui_rect_has_area(visible)) {
            // Notes shown clipped fade out over their last 48 DIP.
            const float fade = ui_markdown_height(block, ui_rect_width(r.body), scene.measure) > ui_rect_height(r.body) + 1 ? 48.f : 0.f;
            for (u32 part = 0, parts = ui_markdown_part_count(*block); part < parts; ++part)
                for (u32 marker = 0; marker < 2; ++marker) {
                    const auto command = ui_markdown_command(block, part, marker != 0, scene.measure,
                                                             {&scene.widgets, handle}, r.body, visible, tc.text_default,
                                                             tc.text_subtle, fade);
                    if (command.text.bytes)
                        pen.emit(command);
                }
        }
        if (ui_rect_has_area(r.control))
            text(handle, c.detail, r.control, tc.link, text_button);
        return;
    }
    if (c.kind == UiSettingsControlKind::footnote) {
        text(handle, c.label, r.label, tc.text_subtle, text_description, ui_text_wrap);
        if (ui_rect_has_area(r.control))
            text(handle, c.action_label, r.control, state.hovered ? tc.text_strong : tc.link, text_description);
        return;
    }
    if (c.kind == UiSettingsControlKind::action && more(scene.content, index, ui_settings_more_cell)) {
        // A game tile's name under its cover, cut to one line.
        text(handle, c.label, r.label, ui_settings_color_alpha(tc.text_strong, dim), kUiKitTextSmallBold);
        return;
    }
    if (c.kind == UiSettingsControlKind::tabs) {
        for (u32 i = 0; i < c.choices.count; ++i) {
            const auto tab = option_rect(t, c, r.control, i, scene.measure);
            const bool hovered = state.hovered && scene.palette_hovered_index == i;
            text(handle, c.choices[i], {tab.left, tab.top, tab.right, tab.top + kUiSettingsLabelLine},
                 ui_settings_color_alpha(c.selected_choice == i ? tc.text_strong
                                         : hovered               ? tc.text_default
                                                                 : tc.text_subtle,
                                         dim),
                 text_label, ui_text_center);
        }
        return;
    }
    if (list_style(scene.content, index, UiSettingsListStyle::card)) {
        text(handle, c.label, r.label, ui_settings_color_alpha(tc.text_strong, dim), text_heading);
        text(handle, c.description, r.description, ui_settings_color_alpha(tc.text_subtle, dim), text_description,
             ui_text_wrap);
        return;
    }
    if (list_style(scene.content, index, UiSettingsListStyle::row)) {
        const bool count_hovered = state.hovered && scene.palette_hovered_index == ui_settings_list_count;
        text(handle, c.label, r.label, ui_settings_color_alpha(tc.text_strong, dim), text_label);
        if (c.choices.count)
            text(handle, c.choices[0], r.value,
                 ui_settings_color_alpha(count_hovered ? tc.text_strong : tc.text_default, dim), text_label);
        return;
    }
    if (c.kind == UiSettingsControlKind::action && more(scene.content, index, ui_settings_more_name_tag)) {
        // A name previewed in chat: the name in its colour, the tag in its chip, the message under them.
        Color name = tc.text_strong;
        if (accent(scene.content, index))
            (void)resolve_color(scene, c.id, kUiSettingsAccentChoice, &name);
        text(handle, c.label, r.label, name, text_heading);
        if (ui_rect_has_area(r.tag))
            text(handle, c.detail,
                 {r.tag.left + kUiBadgeCompactPadding + kUiBadgeCompactIcon + kUiBadgeCompactIconGap, r.tag.top,
                  r.tag.right, r.tag.bottom},
                 tc.text_default, text_value);
        text(handle, c.description, r.description, tc.text_default, text_description, ui_text_wrap);
    } else {
        text(handle, c.label, r.label,
             c.kind == UiSettingsControlKind::notice && variant(scene.content, index) == UiSettingsVariant::critical
                 ? tc.critical_text
                 : ui_settings_color_alpha(tc.text_strong, dim),
             text_label);
        text(handle, c.description, r.description, ui_settings_color_alpha(tc.text_subtle, dim), text_description,
             ui_text_wrap);
    }
    // Initials only for avatar rows without an image to draw.
    if (c.kind == UiSettingsControlKind::action && has_flag(scene.content, index, ui_settings_presentation_avatar) &&
        !avatar_backed(scene.content, index)) {
        char storage[16];
        text(handle, ui_admin_initials(c.label, {storage, sizeof(storage)}),
             avatar_rect(t.metric, scene.content, index, r.lead), tc.on_brand, text_heading, ui_text_center);
    }
    if (ui_rect_has_area(r.chips)) {
        const bool dots = accent(scene.content, index);
        for (u32 i = 0; i < c.choices.count; ++i) {
            const auto chip = ui_admin_chip_rect(t, c.choices, dots, r.chips, i);
            if (ui_rect_has_area(chip))
                text(handle, c.choices[i], ui_admin_chip_label_rect(t, chip, ui_admin_chip_dot(c.choices[i], dots)),
                     ui_settings_color_alpha(tc.text_default, dim), text_description);
        }
    }
    switch (c.kind) {
    case UiSettingsControlKind::segments:
        if (!has_flag(scene.content, index, ui_settings_presentation_tristate)) {
            // The labels cross-fade with the plate; the weight follows once the label is mostly the selected one.
            const auto motion = segment_motion(scene, c);
            for (u32 i = 0; i < c.choices.count; ++i) {
                const float shown = segment_selection(motion, c.selected_choice, i);
                text(handle, c.choices[i], ui_segmented_option(t, r.control, c.choices.count, i),
                     ui_settings_color_alpha(shown <= 0   ? tc.text_default
                                             : shown >= 1 ? tc.text_strong
                                                          : ui_settings_color_mix(tc.text_default, tc.text_strong, shown),
                                             dim),
                     shown >= .5f ? text_button : text_dropdown, ui_text_center);
            }
        }
        break;
    case UiSettingsControlKind::buttons: {
        const auto others = variant(scene.content, index) == UiSettingsVariant::critical
                                ? UiSettingsButtonVariant::critical_secondary
                                : UiSettingsButtonVariant::secondary;
        for (u32 i = 0; i < c.choices.count; ++i)
            text(handle, c.choices[i], option_rect(t, c, r.control, i, scene.measure),
                 ui_settings_button_text(t, i ? others : UiSettingsButtonVariant::primary,
                                         hover_option(scene, state, i)),
                 text_button, ui_text_center);
        break;
    }
    case UiSettingsControlKind::colors:
        for (u32 i = 0; i < c.choices.count; ++i) {
            const auto swatch = option_rect(t, c, r.control, i, scene.measure);
            text(handle, c.choices[i], {swatch.left - 8, swatch.bottom + 4, swatch.right + 8, swatch.bottom + kColorCaption},
                 ui_settings_color_alpha(tc.text_default, dim), text_description, ui_text_center);
        }
        if (c.action_label.bytes)
            text(handle, c.action_label, option_rect(t, c, r.control, c.choices.count, scene.measure),
                 ui_settings_button_text(t, UiSettingsButtonVariant::secondary, hover_option(scene, state, c.choices.count)),
                 text_button, ui_text_center);
        break;
    case UiSettingsControlKind::action:
    case UiSettingsControlKind::notice:
        if (c.action_label.bytes && ui_rect_has_area(r.control))
            text(handle, c.action_label, r.control,
                 ui_settings_button_text(t,
                                         c.kind == UiSettingsControlKind::notice ? UiSettingsButtonVariant::secondary
                                                                                 : button_variant(scene.content, index),
                                         state),
                 text_button, ui_text_center);
        break;
    case UiSettingsControlKind::choice: {
        // An unselected dropdown shows the "Select…" placeholder.
        const bool none = c.selected_choice >= c.choices.count;
        text(handle, none ? utf8_literal("Select\xE2\x80\xA6") : c.choices[c.selected_choice],
             ui_select_text_rect(t, r.control), ui_settings_color_alpha(none ? tc.text_subtle : tc.text_strong, dim),
             text_dropdown);
        break;
    }
    case UiSettingsControlKind::radio:
        for (u32 i = 0; i < c.choices.count; ++i)
            text(handle, c.choices[i], ui_radio_label(t, ui_radio_option(t, r.control, i)),
                 ui_settings_color_alpha(c.selected_choice == i ? tc.text_strong : tc.text_default, dim), text_label);
        break;
    case UiSettingsControlKind::slider:
        if (marks(scene.content, index)) {
            const auto g = ui_slider_geometry(t, r.control, c.choices.count, c.selected_choice, true);
            const u32 fallback = default_choice(scene.content, index);
            for (u32 i = 0; i < c.choices.count; ++i)
                text(handle, c.choices[i], ui_slider_mark_label(t, g, c.choices.count, i),
                     ui_settings_color_alpha(i == fallback                ? tc.positive
                                             : i == c.selected_choice ? tc.text_strong
                                                                      : tc.text_subtle,
                                             dim),
                     text_description, ui_text_center);
        } else if (c.selected_choice < c.choices.count)
            text(handle, c.choices[c.selected_choice], r.value, ui_settings_color_alpha(tc.text_default, dim),
                 text_value, ui_text_end);
        break;
    case UiSettingsControlKind::palette: {
        const u32 columns = ui_swatch_columns(t, ui_rect_width(r.control));
        for (u32 i = 0; i < c.choices.count; ++i) {
            const auto swatch = ui_swatch_rect(t, r.control, i, columns);
            text(handle, c.choices[i], {swatch.left - 8, swatch.bottom + 6, swatch.right + 8, swatch.bottom + ui_swatch_label_height()},
                 ui_settings_color_alpha(c.selected_choice == i ? tc.text_strong : tc.text_subtle, dim), text_description,
                 ui_text_wrap | ui_text_center);
        }
        break;
    }
    case UiSettingsControlKind::edit:
    case UiSettingsControlKind::multiline: {
        if (more(scene.content, index, ui_settings_more_cell)) {
            // A trait's emoji sits on its button; the clear button is a glyph.
            if (c.detail.bytes)
                text(handle, c.detail, r.lead, ui_settings_color_alpha(tc.text_strong, dim), text_label, ui_text_center);
        } else if (ui_rect_has_area(r.value) && c.action_label.bytes)
            text(handle, c.action_label, r.value,
                 ui_settings_button_text(t, UiSettingsButtonVariant::primary, hover_option(scene, state, 0)), text_button,
                 ui_text_center);
        const auto& editor = *c.editor;
        if (!geometry_valid(editor.geometry)) {
            pen.fail();
            return;
        }
        UiSettingsControlGeometry geometry{};
        if (!ui_settings_scene_control_geometry(scene, entry.target, &geometry))
            return;
        const bool entered = editor.view.text.bytes != 0;
        const bool measured = entered && editor.geometry && ui_rect_has_area(editor.geometry->text);
        const auto text_clip = ui_rect_overlap(geometry.text, geometry.clip);
        if (c.kind == UiSettingsControlKind::multiline && scene.content.sectioned && c.action_label.bytes)
            text(handle, c.action_label,
                 {geometry.field.right - 14 - 64, geometry.field.bottom - 10 - kUiSettingsLabelLine,
                  geometry.field.right - 14, geometry.field.bottom - 10},
                 variant(scene.content, index) == UiSettingsVariant::critical ? tc.critical_text : tc.text_default,
                 text_button, ui_text_end);
        // A grid cell's text that does not fit ends in an ellipsis while the field is not being edited.
        const bool cut = more(scene.content, index, ui_settings_more_cell) && !editor.view.focused;
        if (ui_rect_has_area(text_clip))
            pen.emit({entered ? editor.view.text : editor.placeholder,
                      {&scene.widgets, handle},
                      measured && !cut ? editor.geometry->text : geometry.text,
                      text_clip,
                      entered ? ui_settings_color_alpha(tc.text_strong, dim) : tc.text_subtle,
                      0,
                      UiTextAlignment::start,
                      c.kind == UiSettingsControlKind::multiline || (measured && !cut) ? UiTextAlignment::start
                                                                                      : UiTextAlignment::center,
                      c.kind == UiSettingsControlKind::multiline ? TextWrapping::wrap : TextWrapping::no_wrap,
                      cut ? TextOverflow::ellipsis : TextOverflow::clip});
        break;
    }
    default:
        break;
    }
}
} // namespace
} // namespace settings_scene_detail

bool ui_settings_scene_text_output(const UiSettingsScene& scene, const Theme& theme, UiTextOutput* stream) noexcept {
    if (!scene.active)
        return true;
    if (!stream || !current(scene))
        return false;
    const UiSettingsThemeTokens t(theme);
    const auto& tc = t.color;
    UiTextPen text{stream, scene.widgets, kUiNoRect};
    // Chrome text first, then the page's.
    for (u32 i = 0; i < scene.widgets.count; ++i) {
        UiWidgetHandle handle{};
        NavRow row{};
        const auto* node = visible_nav(scene, i, &handle, &row);
        if (!node || row.kind == UiSettingsNavKind::separator)
            continue;
        auto nav = text.in(node->clip_rectangle);
        if (row.kind == UiSettingsNavKind::header) {
            const float inset = t.metric.nav_item_padding;
            // Bottom-aligned 12 bold caps: the group gap stays above the label.
            nav(handle, row.label,
                {node->rectangle.left + inset, node->rectangle.bottom - 4 - 16, node->rectangle.right - inset,
                 node->rectangle.bottom - 4},
                tc.nav_header, text_eyebrow);
            continue;
        }
        const bool selected = nav_selected(scene, row);
        const auto state = widget_state(scene, handle, row.target, true);
        nav(handle, row.label, ui_settings_nav_label_rect(t, node->rectangle, row.icon < UiIcon::count),
            ui_settings_nav_item_text(t, selected, row.destructive, state), text_label);
        if (row.badge && row.badge != kUiSettingsNavDot) {
            const auto digits = nav_badge_label(row.badge);
            nav(handle, digits,
                ui_settings_nav_badge_rect(node->rectangle, false, ui_kit_text_width(scene.measure, digits, text_value)),
                tc.on_brand, text_value, ui_text_center);
        }
    }
    // The sidebar note: one start-aligned 12/16 line per '\n'-separated part.
    if (const UiRect note = frame(scene, t.metric).nav_note; ui_rect_has_area(note)) {
        const Utf8View all = scene.navigation_note;
        u32 start = 0, line = 0;
        for (u32 i = 0; i <= all.bytes && line < kUiSettingsNavigationNoteLines; ++i) {
            if (i < all.bytes && all.data[i] != '\n')
                continue;
            const float top = note.top + static_cast<float>(line++) * kNavNoteLine;
            text.in(note)(scene.sidebar, {all.data + start, i - start}, {note.left, top, note.right, top + kNavNoteLine},
                          tc.text_subtle, kTextStyleSizeCodeXs);
            start = i + 1;
        }
    }
    if (scene.search_field.generation) {
        UiSettingsControlGeometry geometry{};
        if (ui_settings_scene_search_geometry(scene, &geometry)) {
            const auto& search = scene.search;
            const bool entered = search.view.text.bytes != 0;
            const bool measured = entered && search.view.focused && search.geometry &&
                                  ui_rect_has_area(search.geometry->text);
            const auto clip = ui_rect_overlap(geometry.text, geometry.clip);
            if (ui_rect_has_area(clip))
                text.emit({entered ? search.view.text
                                   : (search.placeholder.bytes ? search.placeholder : utf8_literal("Search settings")),
                           {&scene.widgets, scene.search_field},
                           measured ? search.geometry->text : geometry.text,
                           clip,
                           entered ? tc.text_strong : tc.text_subtle,
                           text_label,
                           UiTextAlignment::start,
                           measured ? UiTextAlignment::start : UiTextAlignment::center,
                           TextWrapping::no_wrap,
                           TextOverflow::clip});
            // The passive "Ctrl F" hint stands at the end of an empty field.
            if (!entered)
                text.at(scene.search_field)(scene.search_field, utf8_literal("Ctrl F"),
                                            ui_settings_search_hint_rect(geometry.field), tc.nav_header, text_eyebrow,
                                            ui_text_center);
        }
    }
    const auto* title = ui_widget_manager_layout_node(scene.widgets, scene.title);
    const auto* description = ui_widget_manager_layout_node(scene.widgets, scene.description);
    if (!title || !description)
        return false;
    // A host page draws its own heading.
    if (!scene.content.host) {
        text.in(title->clip_rectangle)(scene.title, scene.content.title, title->rectangle, tc.text_strong,
                                       scene.presentation == UiSettingsPresentation::modal ? kUiKitTextTitle
                                                                                           : kUiKitTextHeading);
        text.in(description->clip_rectangle)(scene.description, scene.content.description, description->rectangle,
                                             tc.text_subtle, text_description, ui_text_wrap);
    }
    for (u32 i = 0; i < scene.widgets.count; ++i) {
        const auto& entry = entries(scene)[i];
        if (entry.role == EntryRole::row && entry.source_index < scene.content.controls.count)
            row_text(text, scene, t, {static_cast<u16>(i), scene.widgets.generation}, entry);
    }
    UiRect aside_heading{}, aside_clip{};
    if (scene.content.aside_label.bytes && ui_settings_scene_aside(scene, &aside_heading, nullptr, &aside_clip))
        text.in(aside_clip)(scene.body, scene.content.aside_label, aside_heading, tc.text_strong, text_label);
    if (scene.footer.visible) {
        const float dx = scene.footer_shift_q / 16.f, alert = scene.footer_alert_q / 255.f;
        const auto moved = [dx](UiRect r) noexcept { return UiRect{r.left + dx, r.top, r.right + dx, r.bottom}; };
        const Color on_alert = ui_settings_color_mix(tc.text_strong, tc.on_brand, alert);
        if (const auto* status = ui_widget_manager_layout_node(scene.widgets, scene.footer_status)) {
            // Dialog footers carry actions, not a draft: no unsaved-changes prompt.
            const auto label = scene.footer.status.bytes ? scene.footer.status
                               : scene.footer.saving     ? utf8_literal("Saving…")
                               : scene.footer.failed     ? utf8_literal("Couldn't save. Try again.")
                               : scene.presentation == UiSettingsPresentation::dialog
                                   ? Utf8View{}
                                   : utf8_literal("Careful — you have unsaved changes!");
            text.in(status->clip_rectangle)(
                scene.footer_status, label, moved(status->rectangle),
                scene.footer.failed && !scene.footer.saving && !scene.footer_alert_q ? tc.critical_text : on_alert,
                text_label);
        }
        const UiSettingsTarget reset{UiSettingsTargetKind::reset, scene.selected_page, 0};
        const UiSettingsTarget save{UiSettingsTargetKind::save, scene.selected_page, 0};
        if (const auto* reset_node = ui_widget_manager_layout_node(scene.widgets, scene.footer_reset))
            text.in(reset_node->clip_rectangle)(
                scene.footer_reset, scene.footer.reset_label.bytes ? scene.footer.reset_label : utf8_literal("Reset"),
                moved(reset_node->rectangle), target_exists(scene, reset) ? on_alert : ui_settings_color_alpha(on_alert, .5f),
                text_button, ui_text_center);
        if (const auto* save_node = ui_widget_manager_layout_node(scene.widgets, scene.footer_save))
            text.in(save_node->clip_rectangle)(
                scene.footer_save,
                scene.footer.saving               ? utf8_literal("Saving…")
                : scene.footer.save_label.bytes ? scene.footer.save_label
                                                : utf8_literal("Save changes"),
                moved(save_node->rectangle),
                ui_settings_button_text(t, UiSettingsButtonVariant::primary, {target_exists(scene, save), false, false, false}),
                text_button, ui_text_center);
    }
    return text.ok();
}
} // namespace maicord
