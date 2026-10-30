#include "maicord/ui/theme_style_projection.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/transition.hpp"
#include "maicord/ui/spinner.hpp"
#include "maicord/ui/text_style.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/theme_styles.hpp"
#include "maicord/ui/visit_ref.hpp"

namespace maicord {
namespace {
constexpr u16 interactive = ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable;
// Title line of the 20 DIP page heading; the official scroll-into-view paddings.
constexpr float kTitleLine = 26, kRevealPadding = 16, kSidebarRevealPadding = 12;
constexpr u32 text_label = text_style_none, text_description = text_style_subtext,
              text_heading = text_style_bold, text_button = text_style_subtext | text_style_bold,
              text_eyebrow = text_style_subtext | text_style_bold;

enum class EntryRole : u8 {
    normal,
    nav,
    row,
    footer_status
};
struct Entry {
    UiSettingsTarget target;
    u32 source_index;
    EntryRole role;
};

Entry* entries(UiSettingsScene& scene) noexcept {
    return static_cast<Entry*>(scene.storage.sidecar);
}
const Entry* entries(const UiSettingsScene& scene) noexcept {
    return static_cast<const Entry*>(scene.storage.sidecar);
}
float* prefixes(UiSettingsScene& scene) noexcept {
    return reinterpret_cast<float*>(entries(scene) + scene.capacity);
}
const float* prefixes(const UiSettingsScene& scene) noexcept {
    return reinterpret_cast<const float*>(entries(scene) + scene.capacity);
}
bool editor_kind(UiSettingsControlKind kind) noexcept {
    return kind == UiSettingsControlKind::edit || kind == UiSettingsControlKind::multiline;
}
bool side_control(UiSettingsControlKind kind) noexcept {
    return kind == UiSettingsControlKind::toggle || kind == UiSettingsControlKind::action ||
           kind == UiSettingsControlKind::segments;
}
bool field_row(UiSettingsControlKind kind) noexcept {
    return kind != UiSettingsControlKind::category && kind != UiSettingsControlKind::notice &&
           kind != UiSettingsControlKind::tabs && kind != UiSettingsControlKind::error;
}
// Customization geometry (profile editor): tab bar items (20 DIP
// labels over 16 DIP padding and a hairline, then a 16 DIP margin), section
// dividers 24 DIP below their section, captioned 69x50 swatches and the
// aside column (300 DIP beside the form with a 36 DIP gap).
constexpr float kTabBarHeight = 37, kTabBarMargin = 16, kTabGap = 32;
constexpr float kSectionGap = 24;
constexpr float kColorSwatchWidth = 69, kColorSwatchHeight = 50, kColorSwatchGap = 16, kColorCaption = 20;
constexpr float kAsideGap = 36, kAsideFormMinimum = 280, kAsideHeadingGap = 8;
constexpr float kButtonGroupGap = 4;
bool option_kind(UiSettingsControlKind kind) noexcept {
    return kind == UiSettingsControlKind::segments || kind == UiSettingsControlKind::swatches ||
           kind == UiSettingsControlKind::tabs || kind == UiSettingsControlKind::buttons ||
           kind == UiSettingsControlKind::colors;
}
// Sectioned pages: the space after a row that ends its section (the divider
// sits in its middle); none while error rows follow and after the last.
float section_trailing(const UiSettingsPageContent& content, u32 index) noexcept {
    if (!content.sectioned)
        return 0;
    const u32 next = index + 1;
    if (next >= content.controls.count || content.controls[next].kind == UiSettingsControlKind::error)
        return 0;
    return kSectionGap * 2 + 1;
}
// The scene's layout tokens, computed in place (a by-value return would be copied).
struct SceneTokens : UiSettingsLayoutTokens {
    explicit SceneTokens(const UiSettingsScene& scene) noexcept {
        ui_settings_fill_layout_tokens(scene.theme_styles, this);
    }
};
const UiSettingsControlPresentation* presentation(const UiSettingsPageContent& content, u32 index) noexcept {
    return content.presentation && index < content.controls.count ? &content.presentation[index] : nullptr;
}
bool marks(const UiSettingsPageContent& content, u32 index) noexcept {
    const auto* p = presentation(content, index);
    return p && (p->flags & ui_settings_presentation_marks) && content.controls[index].choices.count <= 32;
}
bool link(const UiSettingsPageContent& content, u32 index) noexcept {
    const auto* p = presentation(content, index);
    return p && (p->flags & ui_settings_presentation_link);
}
u32 default_choice(const UiSettingsPageContent& content, u32 index) noexcept {
    const auto* p = presentation(content, index);
    return p && p->default_choice != kUiSettingsNoDefault ? p->default_choice : kUiSettingsNoChoice;
}
UiSettingsVariant variant(const UiSettingsPageContent& content, u32 index) noexcept {
    const auto* p = presentation(content, index);
    return p && p->variant <= UiSettingsVariant::positive ? p->variant : UiSettingsVariant::standard;
}
bool has_flag(const UiSettingsPageContent& content, u32 index, u8 flag) noexcept {
    const auto* p = presentation(content, index);
    return p && (p->flags & flag);
}
bool accent(const UiSettingsPageContent& content, u32 index) noexcept {
    return has_flag(content, index, ui_settings_presentation_accent);
}
// An avatar row whose image the renderer draws (hashed identities, or a
// default avatar: profile_image, hash = CDN path). It shows a plain loading
// placeholder that the image hides, as member list and message avatars do,
// never initials that a transparent image would show through.
bool avatar_backed(const UiSettingsPageContent& content, u32 index) noexcept {
    if (!content.avatar_media || !has_flag(content, index, ui_settings_presentation_avatar) ||
        content.controls[index].kind != UiSettingsControlKind::action)
        return false;
    const auto& media = content.avatar_media[index];
    // Emoji and sticker previews are named by their ID alone.
    if (media.key.kind == MediaAssetKind::custom_emoji || media.key.kind == MediaAssetKind::sticker)
        return media.key.id != 0;
    return (media.key.kind == MediaAssetKind::user_avatar || media.key.kind == MediaAssetKind::guild_icon ||
            media.key.kind == MediaAssetKind::profile_image) &&
           media.key.id && media.hash_bytes && media.hash_bytes <= kUiSettingsAvatarHashBytes;
}
// Host colour for a swatch, chip or accent; false when none.
bool resolve_color(const UiSettingsScene& scene, u64 control_id, u32 choice, Color* color) noexcept {
    BackgroundTheme theme{};
    if (!scene.palette.resolve || !scene.palette.resolve(scene.palette.context, control_id, choice, &theme) ||
        !theme.color_count)
        return false;
    *color = color_from_rgb24(theme.colors[0]);
    return true;
}
// A styled list entry that fits its control: rows and cards on action rows,
// headers on category rows.
const UiSettingsListItem* list_item(const UiSettingsPageContent& content, u32 index) noexcept {
    if (!content.list_items || index >= content.controls.count)
        return nullptr;
    const auto& item = content.list_items[index];
    const auto kind = content.controls[index].kind;
    const bool fits = item.style == UiSettingsListStyle::header
                          ? kind == UiSettingsControlKind::category
                          : (item.style == UiSettingsListStyle::row || item.style == UiSettingsListStyle::card) &&
                                kind == UiSettingsControlKind::action;
    return fits ? &item : nullptr;
}
bool list_style(const UiSettingsPageContent& content, u32 index, UiSettingsListStyle style) noexcept {
    const auto* item = list_item(content, index);
    return item && item->style == style;
}
// Action rows may carry list decorations: chips are their choices (list
// rows use choices[0] as their count instead).
bool chips(const UiSettingsPageContent& content, u32 index) noexcept {
    const auto& c = content.controls[index];
    return c.kind == UiSettingsControlKind::action && c.choices.data && c.choices.count && c.choices.count <= 16 &&
           !list_item(content, index);
}
// Action rows with a meter level show it as a progress bar (thousandths).
bool progress(const UiSettingsPageContent& content, u32 index) noexcept {
    return content.controls[index].kind == UiSettingsControlKind::action && content.meters &&
           content.meters[index] != kUiSettingsNoMeter && !list_item(content, index);
}
float button_label_width(const Utf8View& label) noexcept {
    return ui_settings_text_width(label, kUiSettingsDescriptionEm) * 1.08f;
}
// A field shows its error state when its row is critical or an error row
// follows it.
bool field_error(const UiSettingsPageContent& content, u32 index) noexcept {
    const auto* p = presentation(content, index);
    return (p && p->variant == UiSettingsVariant::critical) ||
           (index + 1 < content.controls.count && content.controls[index + 1].kind == UiSettingsControlKind::error);
}
// Slider labels are estimated generously so a value never ellipsizes.
float slider_label_width(const Utf8View& label) noexcept {
    return ui_settings_text_width(label, kUiSettingsDescriptionEm) * 1.25f + 4;
}
// The value label of a slider without marks sits at the end of its label
// line, as wide as its widest end or current label.
float slider_value_width(const UiSettingsControl& c) noexcept {
    if (!c.choices.count)
        return 0;
    const Utf8View ends[]{c.choices[0], c.choices[c.choices.count - 1],
                          c.choices[minimum(c.selected_choice, c.choices.count - 1)]};
    float width = 0;
    for (const auto label : ends)
        width = maximum(width, slider_label_width(label));
    return width;
}
// Mark sliders inset their track so the end labels, centred on the end
// ticks, stay inside the column.
float slider_mark_inset(const UiSettingsLayoutTokens& t, const UiSettingsControl& c) noexcept {
    if (!c.choices.count)
        return 0;
    const float half = maximum(slider_label_width(c.choices[0]), slider_label_width(c.choices[c.choices.count - 1])) * .5f;
    return maximum(0.f, half - t.metric.slider_grabber * .5f);
}

// Multi-line fields grow with their text from three lines (four with a
// counter) to the style's height, then scroll; lines are estimated like other
// settings text.
float multiline_height(const UiSettingsStyle& style, const UiSettingsControl& c, float width) noexcept {
    const float inner = maximum(1.f, width - style.field_padding * 2);
    const Utf8View text = c.editor ? c.editor->view.text : Utf8View{};
    u32 lines = 0, start = 0;
    for (u32 i = 0; i <= text.bytes; ++i)
        if (i == text.bytes || text.data[i] == '\n') {
            lines += maximum(1u, ui_settings_text_lines({text.data + start, i - start}, kUiSettingsLabelEm, inner));
            start = i + 1;
        }
    const u32 minimum_lines = c.action_label.bytes ? 4u : 3u;
    const float grown =
        static_cast<float>(maximum(lines, minimum_lines)) * kUiSettingsLabelLine + style.field_padding * 2;
    return minimum(grown, maximum(style.multiline_height, style.single_height));
}

// One row's geometry, shared by measurement, paint, text and hit-testing.
struct RowLayout {
    UiRect row, label, description, control, lead, chips, value;
    float height;
};
// Parts of a list row or card, from its row rectangle.
struct ListParts {
    UiRect content, icon, lock, column, chevron;
    UiRect actions[kUiSettingsListActions];
};
ListParts list_parts(const UiSettingsLayoutTokens& t, const UiSettingsPageContent& content, u32 index,
                     const UiRect& row) noexcept {
    const auto& m = t.metric;
    ListParts parts{};
    const auto* item = list_item(content, index);
    if (!item)
        return parts;
    const float cy = (row.top + row.bottom) * .5f;
    if (item->style == UiSettingsListStyle::card) {
        // A raised card inside the row's vertical padding.
        const float inset = m.row_padding * .5f;
        parts.content = {row.left, row.top + inset, row.right, row.bottom - inset};
        const float pad = m.notice_padding, tile = m.avatar_size;
        parts.icon = {parts.content.left + pad, cy - tile * .5f, parts.content.left + pad + tile, cy + tile * .5f};
        const float right = parts.content.right - pad;
        parts.chevron = {right - m.list_icon, cy - m.list_icon * .5f, right, cy + m.list_icon * .5f};
        return parts;
    }
    parts.content = {row.left + m.lead_gap, row.top, row.right - m.lead_gap, row.bottom};
    // Every row keeps the grip's slot so glyphs align whether it drags or not.
    float x = parts.content.left + m.grip_width + m.chip_gap;
    parts.icon = {x, cy - m.list_icon * .5f, x + m.list_icon, cy + m.list_icon * .5f};
    // The lock keeps a fixed slot so names line up on every row.
    x = parts.icon.right + m.chip_gap * 2;
    const float lock = m.list_icon * .75f;
    parts.lock = {x, cy - lock * .5f, x + lock, cy + lock * .5f};
    float right = parts.content.right;
    for (u32 i = kUiSettingsListActions; i-- > 0;) {
        parts.actions[i] = {right - m.list_action, cy - m.list_action * .5f, right, cy + m.list_action * .5f};
        right -= m.list_action + m.chip_gap;
    }
    parts.column = {right - m.list_column, cy - m.list_action * .5f, right - m.chip_gap, cy + m.list_action * .5f};
    return parts;
}
// The count column's glyph after its text.
UiRect list_count_icon(const UiSettingsLayoutTokens& t, const UiSettingsControl& c, const UiRect& column) noexcept {
    const float size = t.metric.list_icon * .75f, cy = (column.top + column.bottom) * .5f;
    const float text = c.choices.count ? ui_settings_text_width(c.choices[0], kUiSettingsLabelEm) * 1.04f : 0;
    const float left = minimum(column.left + text + t.metric.chip_gap, column.right - size);
    return {left, cy - size * .5f, left + size, cy + size * .5f};
}
// The part of a list row under a point (UiSettingsListPart), or ~0u.
u32 list_part(const UiSettingsLayoutTokens& t, const UiSettingsPageContent& content, u32 index, const UiRect& row, float x,
              float y) noexcept {
    const auto* item = list_item(content, index);
    if (!item || !ui_rect_contains(row, x, y))
        return ~0u;
    if (item->style != UiSettingsListStyle::row)
        return ui_settings_list_row;
    const auto parts = list_parts(t, content, index, row);
    for (u32 i = 0; i < kUiSettingsListActions; ++i)
        if (item->actions[i] != UiIcon::count && ui_rect_contains(parts.actions[i], x, y))
            return ui_settings_list_action + i;
    const auto& c = content.controls[index];
    if ((item->count_icon != UiIcon::count || (c.choices.count && c.choices[0].bytes)) &&
        ui_rect_contains(parts.column, x, y))
        return ui_settings_list_count;
    return ui_settings_list_row;
}
// Width taken by a row's leading handle, avatar or dot.
float lead_width(const UiSettingsLayoutTokens& t, const UiSettingsPageContent& content, u32 index) noexcept {
    if (content.controls[index].kind != UiSettingsControlKind::action)
        return 0;
    const auto& m = t.metric;
    float width = has_flag(content, index, ui_settings_presentation_grip) ? m.grip_width + 4 : 0;
    if (has_flag(content, index, ui_settings_presentation_avatar))
        width += m.avatar_size + m.lead_gap + 4;
    else if (accent(content, index))
        width += m.accent_dot + m.lead_gap;
    return width;
}
float text_block(const UiSettingsMetrics& m, const Utf8View& label, const Utf8View& description, float width) noexcept {
    float height = label.bytes ? kUiSettingsLabelLine : 0;
    if (description.bytes)
        height += (label.bytes ? m.label_gap : 0) +
                  ui_settings_text_lines(description, kUiSettingsDescriptionEm * 1.04f, width) *
                      kUiSettingsDescriptionLine;
    return height;
}
float swatch_rows_height(const UiSettingsLayoutTokens& t, u32 count, float width) noexcept {
    const u32 columns = ui_swatch_columns(t, width);
    const u32 rows = (count + columns - 1) / columns;
    return rows ? rows * (t.metric.swatch + ui_swatch_label_height()) + (rows - 1) * t.metric.swatch_gap * .5f + 4
                : 0;
}
RowLayout row_layout(const UiSettingsLayoutTokens& t, const UiSettingsStyle& style, const UiSettingsPageContent& content,
                     u32 index, float left, float top, float width) noexcept {
    const auto& m = t.metric;
    const auto& c = content.controls[index];
    RowLayout r{};
    if (c.kind == UiSettingsControlKind::category) {
        const float gap = index ? m.category_gap : 0;
        float right = left + width - (link(content, index) ? 24 : 0);
        if (list_style(content, index, UiSettingsListStyle::header)) {
            // The column title sits over the list rows' count column.
            const float column_right = left + width - m.lead_gap - (m.list_action + m.chip_gap) * kUiSettingsListActions;
            r.value = {column_right - m.list_column, top + gap, column_right - m.chip_gap,
                       top + gap + kUiSettingsLabelLine};
            right = minimum(right, r.value.left - m.control_gap);
        }
        r.label = {left, top + gap, right, top + gap + kUiSettingsLabelLine};
        float bottom = r.label.bottom;
        if (c.description.bytes) {
            const float lines = text_block(m, {}, c.description, right - left);
            r.description = {left, bottom + m.label_gap, right, bottom + m.label_gap + lines};
            bottom = r.description.bottom;
        }
        r.height = bottom - top + m.category_heading_gap;
        r.row = {left, top, left + width, top + r.height};
        return r;
    }
    if (c.kind == UiSettingsControlKind::notice) {
        const float pad = m.notice_padding;
        const bool button = c.action_label.bytes != 0;
        const float button_width =
            button ? ui_settings_button_width(t, UiSettingsButtonSize::compact, button_label_width(c.action_label)) : 0;
        const float text_width = width - pad * 2 - (button ? button_width + pad : 0);
        const float text_height = maximum(kUiSettingsDescriptionLine, text_block(m, c.label, c.description, text_width));
        const float inner = maximum(text_height, button ? m.button_small : 0.f);
        const float box_top = top + m.row_padding * .5f;
        r.row = {left, top, left + width, box_top + inner + pad * 2 + m.row_padding * .5f};
        r.control = {left, box_top, left + width, box_top + inner + pad * 2};
        r.label = {left + pad, box_top + pad, left + pad + text_width, box_top + pad + kUiSettingsLabelLine};
        if (c.description.bytes) {
            const float label_bottom = c.label.bytes ? r.label.bottom + m.label_gap : box_top + pad;
            r.description = {left + pad, label_bottom, left + pad + text_width, box_top + pad + text_height};
        }
        if (button) {
            const float height = m.button_small, middle = (r.control.top + r.control.bottom) * .5f;
            r.control = {r.control.right - pad - button_width, middle - height * .5f, r.control.right - pad,
                         middle + height * .5f};
        } else
            reset_value(&r.control);
        r.height = ui_rect_height(r.row);
        return r;
    }
    if (c.kind == UiSettingsControlKind::tabs) {
        r.control = {left, top, left + width, top + kTabBarHeight};
        r.height = kTabBarHeight + kTabBarMargin;
        r.row = {left, top, left + width, top + r.height};
        return r;
    }
    if (c.kind == UiSettingsControlKind::error) {
        const float lines = static_cast<float>(ui_settings_text_lines(c.label, kUiSettingsDescriptionEm * 1.04f, width));
        r.label = {left, top + 8, left + width, top + 8 + lines * kUiSettingsDescriptionLine};
        r.height = 8 + lines * kUiSettingsDescriptionLine + section_trailing(content, index);
        r.row = {left, top, left + width, top + r.height};
        return r;
    }
    if (const auto* item = list_item(content, index)) {
        if (item->style == UiSettingsListStyle::card) {
            const float pad = m.notice_padding, inset = m.row_padding * .5f;
            const float text_left = left + pad + m.avatar_size + m.lead_gap;
            const float text_width = maximum(1.f, left + width - pad - m.list_icon - m.control_gap - text_left);
            const float text_height = text_block(m, c.label, c.description, text_width);
            const float inner = maximum(m.avatar_size, text_height);
            r.height = inset * 2 + pad * 2 + inner;
            r.row = {left, top, left + width, top + r.height};
            const float text_top = top + inset + pad + (inner - text_height) * .5f;
            r.label = {text_left, text_top, text_left + text_width, text_top + kUiSettingsLabelLine};
            if (c.description.bytes)
                r.description = {text_left, r.label.bottom + m.label_gap, text_left + text_width,
                                 text_top + text_height};
            return r;
        }
        const float pad = m.row_padding * .5f, inner = maximum(kUiSettingsLabelLine, m.list_action);
        r.height = pad * 2 + inner;
        r.row = {left, top, left + width, top + r.height};
        const auto parts = list_parts(t, content, index, r.row);
        const float text_left = parts.lock.right + m.lead_gap;
        const float label_top = top + pad + (inner - kUiSettingsLabelLine) * .5f;
        r.lead = {parts.content.left, top + pad, text_left, top + pad + inner};
        r.label = {text_left, label_top, maximum(text_left, parts.column.left - m.control_gap),
                   label_top + kUiSettingsLabelLine};
        r.value = parts.column;
        return r;
    }
    const bool compact = has_flag(content, index, ui_settings_presentation_compact);
    const float pad = compact ? m.row_padding * .5f : m.row_padding;
    // Sections start at their title and end with the divider spacing.
    const float pad_top = content.sectioned ? 0.f : pad;
    const float pad_bottom = content.sectioned ? section_trailing(content, index) : pad;
    const float row_left = left, row_width = width;
    // Compact list rows inset their content inside the hover and selection fill.
    if (compact)
        left += m.lead_gap, width = maximum(1.f, width - m.lead_gap * 2);
    float control_width = 0, control_height = 0;
    if (c.kind == UiSettingsControlKind::toggle)
        control_width = m.switch_width, control_height = m.switch_height;
    else if (c.kind == UiSettingsControlKind::action && c.action_label.bytes)
        control_width = ui_settings_button_width(t, UiSettingsButtonSize::compact, button_label_width(c.action_label)),
        control_height = m.button_small;
    else if (c.kind == UiSettingsControlKind::segments)
        control_width = has_flag(content, index, ui_settings_presentation_tristate) ? ui_tristate_width(t)
                                                                                     : ui_segments_width(t, c.choices.count),
        control_height = m.segment_size;
    const bool side = side_control(c.kind);
    const float lead = lead_width(t, content, index);
    const float text_left = left + lead;
    const float text_width = side ? maximum(1.f, width - lead - control_width - m.control_gap) : width;
    float text_height = text_block(m, c.label, c.description, text_width);
    const float content_top = top + pad_top;
    r.label = {text_left, content_top, text_left + text_width, content_top + kUiSettingsLabelLine};
    if (c.kind == UiSettingsControlKind::slider && !marks(content, index)) {
        // The value shares the label line; the label stops a gap before it.
        const float value = minimum(slider_value_width(c), text_width * .5f);
        r.value = {r.label.right - value, r.label.top, r.label.right, r.label.bottom};
        r.label.right = maximum(r.label.left, r.value.left - m.control_gap);
    }
    if (c.description.bytes) {
        const float desc_top = c.label.bytes ? r.label.bottom + m.label_gap : content_top;
        r.description = {text_left, desc_top, text_left + text_width, content_top + text_height};
    }
    if (chips(content, index)) {
        const float height = ui_admin_chips_height(t, ui_admin_chip_rows(t, c.choices, accent(content, index), text_width));
        r.chips = {text_left, content_top + text_height + 8, text_left + text_width,
                   content_top + text_height + 8 + height};
        text_height += 8 + height;
    }
    if (progress(content, index)) {
        // The progress bar runs under the text, as wide as it.
        const float bar_top = content_top + text_height + m.field_gap;
        r.value = {text_left, bar_top, text_left + text_width, bar_top + m.slider_bar};
        text_height += m.field_gap + m.slider_bar;
    }
    const bool avatar = c.kind == UiSettingsControlKind::action &&
                        has_flag(content, index, ui_settings_presentation_avatar);
    const bool decorated = avatar || ui_rect_has_area(r.chips);
    float inner = text_height;
    if (side) {
        inner = maximum(maximum(text_height, control_height), avatar ? m.avatar_size : 0.f);
        const float control_top =
            c.description.bytes && !decorated ? content_top + (kUiSettingsLabelLine - control_height) * .5f
                                              : content_top + (inner - control_height) * .5f;
        r.control = {left + width - control_width, maximum(content_top - 2, control_top), left + width,
                     maximum(content_top - 2, control_top) + control_height};
        if (!c.description.bytes || (decorated && inner > text_height)) {
            // Center the text block against a taller control or avatar.
            const float shift = (inner - text_height) * .5f;
            r.label.top += shift, r.label.bottom += shift;
            if (c.description.bytes)
                r.description.top += shift, r.description.bottom += shift;
            if (ui_rect_has_area(r.chips))
                r.chips.top += shift, r.chips.bottom += shift;
        }
    } else {
        float height = maximum(m.control_height, style.single_height);
        if (c.kind == UiSettingsControlKind::multiline)
            height = multiline_height(style, c, width);
        else if (c.kind == UiSettingsControlKind::radio)
            height = ui_radio_group_height(t, c.choices.count);
        else if (c.kind == UiSettingsControlKind::slider)
            height = ui_slider_height(t, marks(content, index));
        else if (c.kind == UiSettingsControlKind::palette)
            height = swatch_rows_height(t, c.choices.count, width);
        else if (c.kind == UiSettingsControlKind::swatches)
            height = ui_color_grid_height(t, c.choices.count, width - 8);
        else if (c.kind == UiSettingsControlKind::buttons)
            height = m.button_small;
        else if (c.kind == UiSettingsControlKind::colors)
            height = kColorSwatchHeight + kColorCaption;
        const float control_top = content_top + text_height + (text_height > 0 ? m.field_gap : 0);
        const float control_right =
            c.kind == UiSettingsControlKind::choice ? left + minimum(width, 360.f) : left + width;
        r.control = {left, control_top, control_right, control_top + height};
        if (c.kind == UiSettingsControlKind::edit && c.action_label.bytes) {
            // The button beside the field (a list search's create button).
            const float button =
                ui_settings_button_width(t, UiSettingsButtonSize::compact, button_label_width(c.action_label));
            const float middle = (r.control.top + r.control.bottom) * .5f;
            r.value = {r.control.right - button, middle - m.button_small * .5f, r.control.right,
                       middle + m.button_small * .5f};
            r.control.right = maximum(r.control.left + 1, r.value.left - m.control_gap);
        }
        if (c.kind == UiSettingsControlKind::slider && marks(content, index)) {
            const float inset = slider_mark_inset(t, c);
            r.control.left += inset, r.control.right -= inset;
            r.control.top += 4, r.control.bottom += 4, height += 4;
        }
        inner = text_height + (text_height > 0 ? m.field_gap : 0) + height;
    }
    r.lead = {left, content_top, text_left, content_top + inner};
    r.height = pad_top + pad_bottom + inner;
    r.row = {row_left, top, row_left + row_width, top + r.height};
    return r;
}

// Sidebar rows: bound navigation, or a header plus every registered page.
struct NavRow {
    UiSettingsNavKind kind;
    bool destructive;
    UiIcon icon;
    Utf8View label;
    UiSettingsTarget target;
};
bool page_registered(const UiSettingsScene& scene, UiPageKey key) noexcept {
    for (u32 i = 0; i < scene.registry->count; ++i)
        if (ui_page_key_equal(scene.registry->storage[i].key, key))
            return true;
    return false;
}
// Contributed rows list owners the bound entries never name (plugins), so
// unlisted pages of a named owner (search results) stay hidden.
bool owner_named(const UiSettingsScene& scene, u64 owner) noexcept {
    for (u32 i = 0; i < scene.navigation.count; ++i)
        if (scene.navigation[i].kind == UiSettingsNavKind::page && scene.navigation[i].page.owner_id == owner)
            return true;
    return false;
}
// Flat sidebar rows: bound entries with contributed entries expanded, or a
// header plus every registered page when nothing is bound.
void visit_nav(const UiSettingsScene& scene, UiVisitRef<const NavRow&> visit) noexcept {
    if (scene.presentation == UiSettingsPresentation::dialog)
        return;
    if (!scene.navigation.count) {
        if (!visit(NavRow{UiSettingsNavKind::header, false, UiIcon::count,
                          scene.content.navigation_label.bytes ? scene.content.navigation_label
                                                               : utf8_literal("Settings"),
                          {}}))
            return;
        for (u32 i = 0; i < scene.registry->count; ++i) {
            const auto& page = scene.registry->storage[i];
            if (!visit(NavRow{UiSettingsNavKind::page, false, UiIcon::count, page.label,
                              {UiSettingsTargetKind::page, page.key, 0}}))
                return;
        }
        return;
    }
    for (u32 i = 0; i < scene.navigation.count; ++i) {
        const auto& e = scene.navigation[i];
        if (e.kind == UiSettingsNavKind::contributed) {
            for (u32 p = 0; p < scene.registry->count; ++p) {
                const auto& page = scene.registry->storage[p];
                if (!owner_named(scene, page.key.owner_id) && !visit(NavRow{UiSettingsNavKind::page, false, UiIcon::count,
                                                                  page.label, {UiSettingsTargetKind::page, page.key, 0}}))
                    return;
            }
            continue;
        }
        NavRow row{e.kind, e.destructive, e.icon, e.label, {}};
        if (e.kind == UiSettingsNavKind::page) {
            if (!page_registered(scene, e.page))
                continue;
            row.target = {UiSettingsTargetKind::page, e.page, 0};
        } else if (e.kind == UiSettingsNavKind::action)
            row.target = {UiSettingsTargetKind::action, {}, e.action};
        if (!visit(row))
            return;
    }
}
u32 nav_count(const UiSettingsScene& scene) noexcept {
    u32 count = 0;
    visit_nav(scene, [&](const NavRow&) noexcept {
        ++count;
        return true;
    });
    return count;
}
bool nav_row(const UiSettingsScene& scene, u32 index, NavRow* row) noexcept {
    u32 position = 0;
    bool found = false;
    visit_nav(scene, [&](const NavRow& candidate) noexcept {
        if (position++ != index)
            return true;
        *row = candidate;
        found = true;
        return false;
    });
    return found;
}
// Group headers after the first open their group with extra space (the official
// client separates sidebar sections), so a header never sits flush under a row.
constexpr float kNavGroupGap = 8;
float nav_height(const UiSettingsMetrics& m, UiSettingsNavKind kind, u32 index) noexcept {
    return kind == UiSettingsNavKind::header      ? m.nav_header_height + (index ? kNavGroupGap : 0)
           : kind == UiSettingsNavKind::separator ? 1 + m.separator_margin * 2
                                                  : m.nav_item_height + m.nav_item_gap;
}
float nav_extent(const UiSettingsScene& scene, const UiSettingsMetrics& m) noexcept {
    float extent = 0;
    u32 index = 0;
    visit_nav(scene, [&](const NavRow& row) noexcept {
        extent += nav_height(m, row.kind, index++);
        return true;
    });
    return extent;
}

struct Frame {
    UiRect nav, body, title, description, footer;
};
// Dialog footer band: buttons between the shared notice padding.
float dialog_footer_height(const UiSettingsMetrics& m) noexcept {
    return m.button_small + m.notice_padding * 2 + 8;
}
float description_height(const UiSettingsScene& scene, const UiSettingsMetrics& m) noexcept {
    return scene.content.description.bytes
               ? text_block(m, {}, scene.content.description, ui_rect_width(scene.layout.column)) + m.label_gap
               : 0;
}
Frame frame(const UiSettingsScene& scene, const UiSettingsMetrics& m) noexcept {
    const auto& l = scene.layout;
    Frame f{};
    const bool modal = scene.presentation == UiSettingsPresentation::modal;
    float nav_top = l.sidebar.top + m.sidebar_top + (modal ? m.search_height + m.sidebar_padding : 0);
    const float inset = modal ? m.sidebar_padding : 0;
    f.nav = {l.sidebar.left + inset, nav_top, l.sidebar.right - inset, l.sidebar.bottom - m.sidebar_top};
    if (!modal) {
        // Layer sidebars right-align a fixed column inside a growing region.
        f.nav.left = maximum(l.sidebar.left + 8, l.sidebar.right - m.sidebar_width + 8);
        f.nav.right = l.sidebar.right - 8;
        f.nav.top = l.sidebar.top + 56;
        f.nav.bottom = l.sidebar.bottom - 24;
    }
    if (scene.presentation == UiSettingsPresentation::dialog)
        f.nav = {l.modal.left, l.modal.top, l.modal.left, l.modal.top};
    f.title = {l.column.left, l.column.top, l.column.right, l.column.top + kTitleLine};
    const bool dialog = scene.presentation == UiSettingsPresentation::dialog;
    if (dialog)
        f.title.right = maximum(f.title.left + 1, minimum(f.title.right, l.close.left - 8));
    const float description = description_height(scene, m);
    f.description = {l.column.left, f.title.bottom + (description ? 4 : 0), l.column.right,
                     f.title.bottom + (description ? 4 : 0) + maximum(0.f, description - m.label_gap)};
    const float body_top = (description ? f.description.bottom : f.title.bottom) + m.title_gap;
    const float footer_height = m.button_small + m.notice_padding * 2, footer_gap = m.notice_padding;
    if (dialog) {
        // A full-width band closes the dialog; the body scrolls above it.
        const float band = scene.footer.visible ? dialog_footer_height(m) : 0;
        f.body = {l.column.left, body_top, l.panel.right - kUiSettingsBodyEdgeInset,
                  maximum(body_top + 1, l.panel.bottom - band)};
        if (scene.footer.visible)
            f.footer = {l.modal.left, f.body.bottom, l.modal.right, l.modal.bottom};
        return f;
    }
    const float footer = scene.footer.visible ? footer_height + footer_gap * 2 : 0;
    f.body = {l.column.left, body_top, l.panel.right - kUiSettingsBodyEdgeInset,
              maximum(body_top + 1, l.panel.bottom - footer)};
    if (scene.footer.visible)
        f.footer = {l.column.left, f.body.bottom + footer_gap, l.column.right, f.body.bottom + footer_gap + footer_height};
    return f;
}
float body_width(const UiSettingsScene& scene) noexcept {
    return maximum(1.f, ui_rect_width(scene.layout.column));
}
// The host aside: beside rows from aside_row on when the column fits both,
// else a block above that row. block includes the heading line.
struct AsideLayout {
    bool active, beside;
    float width, form, block;
};
AsideLayout aside_layout(const UiSettingsScene& scene) noexcept {
    const auto& c = scene.content;
    AsideLayout a{};
    if (!(c.aside_width > 0) || !(c.aside_height > 0) || c.aside_row > c.controls.count)
        return a;
    const float full = body_width(scene);
    a.active = true;
    a.width = minimum(c.aside_width, full);
    a.block = (c.aside_label.bytes ? kUiSettingsLabelLine + kAsideHeadingGap : 0) + c.aside_height;
    a.beside = full >= c.aside_width + kAsideGap + kAsideFormMinimum;
    a.form = a.beside ? full - c.aside_width - kAsideGap : full;
    return a;
}
float row_width(const UiSettingsScene& scene, const AsideLayout& aside, u32 index) noexcept {
    return aside.active && aside.beside && index >= scene.content.aside_row ? aside.form : body_width(scene);
}
float row_start(const UiSettingsScene& scene, u32 row) noexcept {
    return prefixes(scene)[row];
}
float content_extent(const UiSettingsScene& scene, const UiSettingsMetrics& m) noexcept {
    float rows = row_start(scene, scene.content.controls.count);
    const auto aside = aside_layout(scene);
    if (aside.active && aside.beside)
        rows = maximum(rows, row_start(scene, scene.content.aside_row) + aside.block);
    return rows +
           (scene.presentation == UiSettingsPresentation::dialog ? kUiSettingsDialogBodyEnd : m.panel_padding_bottom);
}
u32 first_row(const UiSettingsScene& scene, float offset) noexcept {
    u32 first = 0, end = scene.content.controls.count;
    while (first < end) {
        const u32 middle = first + (end - first) / 2;
        if (prefixes(scene)[middle + 1] <= offset)
            first = middle + 1;
        else
            end = middle;
    }
    return first;
}
float visible_height(const UiSettingsScene& scene, bool sidebar, const UiSettingsMetrics& m) noexcept {
    const auto f = frame(scene, m);
    return maximum(1.f, ui_rect_height(sidebar ? f.nav : f.body));
}
float clamp_offset(const UiSettingsScene& scene, bool sidebar, float offset, const UiSettingsMetrics& m) noexcept {
    const float extent = sidebar ? nav_extent(scene, m) : content_extent(scene, m);
    return maximum(0.0f, minimum(offset, maximum(0.0f, extent - visible_height(scene, sidebar, m))));
}

bool geometry_valid(const UiEditGeometry* geometry) noexcept {
    if (!geometry)
        return true;
    // A borrowed renderer bank is empty before its first native measure, and
    // a hidden caret may have zero width. Neither represents invalid bounds.
    if (geometry->selection_count > 48 || !ui_rect_valid_or_empty(geometry->text) ||
        !ui_rect_valid_or_empty(geometry->caret))
        return false;
    for (u32 i = 0; i < geometry->selection_count; ++i)
        if (!ui_rect_valid(geometry->selections[i]))
            return false;
    return true;
}
bool current(const UiSettingsScene& scene) noexcept {
    if (!scene.active || !scene.registry || scene.registry->revision != scene.registry_revision)
        return false;
    const auto* selected = ui_page_registry_selected(*scene.registry);
    if (!selected)
        return !scene.selected_page.owner_id;
    if (!ui_page_key_equal(selected->key, scene.selected_page) || !selected->context)
        return false;
    const auto* content = static_cast<const UiSettingsPageContent*>(selected->context);
    return content->format == kUiSettingsPageFormat && content->revision == scene.content_revision;
}
bool valid_text(const Utf8View& text) noexcept {
    return (!text.bytes || text.data) && utf8_validate(text).valid;
}
bool valid_content(const UiSettingsPageContent& content) noexcept {
    if (content.format != kUiSettingsPageFormat || !content.title.bytes || !valid_text(content.title) ||
        !valid_text(content.description) || !valid_text(content.navigation_label) ||
        content.controls.count > 8192 || (content.controls.count && !content.controls.data))
        return false;
    for (u32 i = 0; i < content.controls.count; ++i) {
        const auto& c = content.controls[i];
        if (!c.id || c.kind > UiSettingsControlKind::error || !valid_text(c.label) ||
            !valid_text(c.description) || !valid_text(c.action_label))
            return false;
        // Notices may be text only; a radio group's options can stand alone
        // under the category row that names them (report steps).
        // A search field may stand on its placeholder alone.
        if (!c.label.bytes && !(c.kind == UiSettingsControlKind::notice && c.description.bytes) &&
            c.kind != UiSettingsControlKind::radio &&
            !(c.kind == UiSettingsControlKind::edit && c.editor && c.editor->placeholder.bytes))
            return false;
        const bool choices = c.kind == UiSettingsControlKind::choice || c.kind == UiSettingsControlKind::palette ||
                             c.kind == UiSettingsControlKind::slider || c.kind == UiSettingsControlKind::radio ||
                             c.kind == UiSettingsControlKind::segments || c.kind == UiSettingsControlKind::swatches ||
                             c.kind == UiSettingsControlKind::tabs || c.kind == UiSettingsControlKind::buttons ||
                             c.kind == UiSettingsControlKind::colors;
        if (choices) {
            // A dropdown may start unselected (a "pick one to add" list).
            const bool optional = c.kind == UiSettingsControlKind::palette || c.kind == UiSettingsControlKind::radio ||
                                  c.kind == UiSettingsControlKind::swatches || c.kind == UiSettingsControlKind::choice ||
                                  c.kind == UiSettingsControlKind::buttons || c.kind == UiSettingsControlKind::colors;
            if (!c.choices.data || !c.choices.count || c.choices.count > 1024 ||
                (c.selected_choice >= c.choices.count && !(optional && c.selected_choice == kUiSettingsNoChoice)) ||
                (c.kind == UiSettingsControlKind::palette && c.choices.count > 12) ||
                (c.kind == UiSettingsControlKind::radio && c.choices.count > 16) ||
                (c.kind == UiSettingsControlKind::slider && c.choices.count < 2) ||
                (c.kind == UiSettingsControlKind::segments && (c.choices.count < 2 || c.choices.count > 4)) ||
                (c.kind == UiSettingsControlKind::swatches && c.choices.count > 32) ||
                (c.kind == UiSettingsControlKind::tabs && (c.choices.count < 2 || c.choices.count > 8)) ||
                (c.kind == UiSettingsControlKind::buttons && c.choices.count > 4) ||
                (c.kind == UiSettingsControlKind::colors && c.choices.count > 4))
                return false;
            for (u32 j = 0; j < c.choices.count; ++j)
                if (!c.choices[j].bytes || !valid_text(c.choices[j]))
                    return false;
        } else if (chips(content, i))
            for (u32 j = 0; j < c.choices.count; ++j)
                if (!c.choices[j].bytes || !valid_text(c.choices[j]))
                    return false;
        // Buttonless action rows are list items and need a list decoration.
        if (c.kind == UiSettingsControlKind::action && !c.action_label.bytes && !chips(content, i) &&
            !accent(content, i) && !list_item(content, i) &&
            !has_flag(content, i, ui_settings_presentation_avatar | ui_settings_presentation_grip))
            return false;
        if (const auto* item = list_item(content, i)) {
            if (item->lead > UiIcon::count || item->count_icon > UiIcon::count ||
                item->actions[0] > UiIcon::count || item->actions[1] > UiIcon::count)
                return false;
            if (c.choices.count && (!c.choices.data || !valid_text(c.choices[0])))
                return false;
        }
        if (editor_kind(c.kind) && (!c.editor || !valid_text(c.editor->view.text) ||
                                    !valid_text(c.editor->placeholder) || !geometry_valid(c.editor->geometry)))
            return false;
    }
    if (content.footer && (!valid_text(content.footer->status) || !valid_text(content.footer->save_label) ||
                           !valid_text(content.footer->reset_label)))
        return false;
    return true;
}
bool valid_navigation(ConstSlice<UiSettingsNavEntry> navigation) noexcept {
    if (navigation.count > kUiSettingsNavigationMaximum || (navigation.count && !navigation.data))
        return false;
    for (u32 i = 0; i < navigation.count; ++i) {
        const auto& e = navigation[i];
        if (e.kind > UiSettingsNavKind::contributed || !valid_text(e.label) ||
            (e.kind != UiSettingsNavKind::separator && e.kind != UiSettingsNavKind::contributed && !e.label.bytes) ||
            (e.kind == UiSettingsNavKind::action && !e.action) ||
            (e.icon > UiIcon::count))
            return false;
    }
    return true;
}
const UiSettingsControl* find_control(const UiSettingsScene& scene, const UiSettingsTarget& target,
                                      u32* index = nullptr) noexcept {
    if (target.kind != UiSettingsTargetKind::control || !ui_page_key_equal(target.page, scene.selected_page))
        return nullptr;
    for (u32 i = 0; i < scene.widgets.count; ++i) {
        const auto& entry = entries(scene)[i];
        if (!ui_settings_target_equal(entry.target, target) || entry.source_index >= scene.content.controls.count)
            continue;
        const auto& control = scene.content.controls[entry.source_index];
        if (control.id != target.control_id)
            continue;
        if (index)
            *index = entry.source_index;
        return &control;
    }
    for (u32 i = 0; i < scene.content.controls.count; ++i) {
        if (scene.content.controls[i].id != target.control_id)
            continue;
        if (index)
            *index = i;
        return &scene.content.controls[i];
    }
    return nullptr;
}
bool focusable_control(const UiSettingsScene& scene, u32 index) noexcept {
    const auto& c = scene.content.controls[index];
    if (!c.enabled)
        return false;
    if (c.kind == UiSettingsControlKind::category)
        return link(scene.content, index);
    if (c.kind == UiSettingsControlKind::notice)
        return c.action_label.bytes != 0;
    return c.kind != UiSettingsControlKind::error;
}
bool target_exists(const UiSettingsScene& scene, const UiSettingsTarget& target) noexcept {
    switch (target.kind) {
    case UiSettingsTargetKind::close:
        return !target.page.owner_id && !target.page.page_id && !target.control_id;
    case UiSettingsTargetKind::search:
    case UiSettingsTargetKind::search_clear:
        return scene.presentation == UiSettingsPresentation::modal && !target.page.owner_id && !target.control_id &&
               (target.kind == UiSettingsTargetKind::search || scene.search.view.text.bytes);
    case UiSettingsTargetKind::page:
        return !target.control_id && page_registered(scene, target.page);
    case UiSettingsTargetKind::action: {
        for (u32 i = 0; i < scene.navigation.count; ++i)
            if (scene.navigation[i].kind == UiSettingsNavKind::action && scene.navigation[i].action == target.control_id)
                return !target.page.owner_id;
        return false;
    }
    case UiSettingsTargetKind::save:
    case UiSettingsTargetKind::reset:
        return !target.control_id && ui_page_key_equal(target.page, scene.selected_page) && scene.footer.visible &&
               !scene.footer.saving && (target.kind == UiSettingsTargetKind::reset || scene.footer.can_save);
    case UiSettingsTargetKind::control: {
        u32 index = 0;
        return find_control(scene, target, &index) && focusable_control(scene, index);
    }
    default:
        return false;
    }
}
UiSettingsTarget fallback_focus(const UiSettingsScene& scene) noexcept {
    if (scene.presentation == UiSettingsPresentation::modal)
        return {UiSettingsTargetKind::search, {}, 0};
    return {UiSettingsTargetKind::close, {}, 0};
}
UiWidgetHandle target_handle(const UiSettingsScene& scene, const UiSettingsTarget& target) noexcept {
    for (u32 i = 0; i < scene.widgets.count; ++i)
        if (ui_settings_target_equal(entries(scene)[i].target, target))
            return {static_cast<u16>(i), scene.widgets.generation};
    return ui_widget_invalid_handle();
}
UiWidgetHandle add(UiSettingsScene* scene, UiWidgetHandle parent, UiWidgetKind kind, const Utf8View& name,
                   const UiLayoutStyle& style, const UiSettingsTarget& target = {}, u32 source_index = 0,
                   bool enabled = true, EntryRole role = EntryRole::normal) noexcept {
    const u32 id = scene->widgets.count + 1;
    const auto result = ui_widget_manager_add_prevalidated(
        &scene->widgets, parent,
        {id,
         kind,
         static_cast<u16>(target.kind != UiSettingsTargetKind::none && enabled ? interactive : ui_widget_none),
         {name, style, {}, 0, id}});
    if (!result.ok())
        return ui_widget_invalid_handle();
    entries(*scene)[result.handle.slot] = {target, source_index, role};
    return result.handle;
}
UiLayoutStyle sized(float width, float height) noexcept {
    auto style = ui_layout_style();
    style.width = width < 0 ? ui_layout_fill() : ui_layout_fixed(width);
    style.height = height < 0 ? ui_layout_fill() : ui_layout_fixed(height);
    style.flags |= ui_layout_node_allow_zero_size;
    return style;
}
UiLayoutStyle placed(const UiRect& rect, const UiRect& origin) noexcept {
    auto style = sized(maximum(0.f, ui_rect_width(rect)), maximum(0.f, ui_rect_height(rect)));
    style.margin.left = rect.left - origin.left;
    style.margin.top = rect.top - origin.top;
    return style;
}

bool row_pointer_region(const void* context, const UiWidgetManager&, UiWidgetHandle handle, float x,
                        float y) noexcept;
bool build(UiSettingsScene* scene) noexcept {
    const SceneTokens t(*scene);
    const auto& m = t.metric;
    const u32 navs = nav_count(*scene);
    const float min_row = kUiSettingsLabelLine + m.row_padding * 2 - 8;
    const u32 visible_rows = static_cast<u32>(visible_height(*scene, false, m) / maximum(24.f, min_row)) + 3;
    const u32 required = 24 + navs + visible_rows;
    const u32 prefix_required = scene->content.controls.count + 1;
    if (required > kUiSettingsMaterializedWidgetLimit)
        return false;
    if (scene->capacity < required || scene->prefix_capacity < prefix_required) {
        ui_fixed_widget_storage_release(&scene->storage);
        reset_value(&scene->widgets);
        scene->capacity = 0;
        scene->prefix_capacity = 0;
        if (!ui_fixed_widget_storage_initialize(&scene->storage, required,
                                                required * static_cast<u32>(sizeof(Entry)) +
                                                    prefix_required * static_cast<u32>(sizeof(float)),
                                                alignof(Entry)))
            return false;
        scene->capacity = required;
        scene->prefix_capacity = prefix_required;
        if (!ui_widget_manager_initialize(&scene->widgets, scene->storage.widgets, scene->storage.layout_nodes))
            return false;
    } else
        ui_widget_manager_reset(&scene->widgets);
    scene->widgets.hit_filter = row_pointer_region;
    scene->widgets.hit_filter_context = scene;
    const float width = body_width(*scene);
    const auto aside = aside_layout(*scene);
    auto* prefix = prefixes(*scene);
    // Stacked asides take a block (and a section gap) before their row.
    const float stacked = aside.active && !aside.beside ? aside.block + kSectionGap : 0;
    prefix[0] = scene->content.aside_row == 0 ? stacked : 0;
    for (u32 i = 0; i < scene->content.controls.count; ++i)
        prefix[i + 1] = prefix[i] +
                        row_layout(t, scene->style, scene->content, i, 0, 0, row_width(*scene, aside, i)).height +
                        (i + 1 == scene->content.aside_row ? stacked : 0);
    scene->content_offset = clamp_offset(*scene, false, scene->content_offset, m);
    scene->sidebar_offset = clamp_offset(*scene, true, scene->sidebar_offset, m);
    const auto f = frame(*scene, m);
    const auto viewport = scene->viewport;
    auto style = sized(-1, -1);
    style.flow = UiLayoutFlow::overlay;
    style.flags |= ui_layout_node_clip_children;
    const auto root =
        add(scene, ui_widget_invalid_handle(), UiWidgetKind::container, utf8_literal("Settings"), style);
    scene->search_field = ui_widget_invalid_handle();
    if (scene->presentation == UiSettingsPresentation::modal) {
        const UiRect search{f.nav.left, scene->layout.sidebar.top + m.sidebar_top, f.nav.right,
                            scene->layout.sidebar.top + m.sidebar_top + m.search_height};
        scene->search_field = add(scene, root, UiWidgetKind::text_field, utf8_literal("Search settings"),
                                  placed(search, viewport), {UiSettingsTargetKind::search, {}, 0});
        if (!scene->search_field.generation)
            return false;
    }
    style = placed(f.nav, viewport);
    style.flow = UiLayoutFlow::vertical;
    style.flags |= ui_layout_node_clip_children | ui_layout_node_scroll_vertical;
    scene->sidebar = add(scene, root, UiWidgetKind::scroll_view, utf8_literal("Pages"), style);
    if (!scene->sidebar.generation)
        return false;
    bool nav_ok = true;
    u32 nav_index = 0;
    visit_nav(*scene, [&](const NavRow& row) noexcept {
        const u32 i = nav_index++;
        const bool button = row.kind == UiSettingsNavKind::page || row.kind == UiSettingsNavKind::action;
        auto item = sized(-1, button ? m.nav_item_height : nav_height(m, row.kind, i));
        if (button)
            item.margin.bottom = m.nav_item_gap;
        nav_ok = add(scene, scene->sidebar,
                     row.target.kind != UiSettingsTargetKind::none ? UiWidgetKind::button : UiWidgetKind::container,
                     utf8_literal("Navigation"), item, row.target, i, true, EntryRole::nav)
                     .generation != 0;
        return nav_ok;
    });
    if (!nav_ok)
        return false;
    scene->title = add(scene, root, UiWidgetKind::text, utf8_literal("Page title"), placed(f.title, viewport));
    scene->description =
        add(scene, root, UiWidgetKind::text, utf8_literal("Page description"), placed(f.description, viewport));
    style = placed(f.body, viewport);
    style.flow = UiLayoutFlow::vertical;
    style.flags |= ui_layout_node_clip_children | ui_layout_node_scroll_vertical;
    scene->body = add(scene, root, UiWidgetKind::scroll_view, utf8_literal("Controls"), style);
    if (!scene->title.generation || !scene->description.generation || !scene->body.generation)
        return false;
    const u32 count = scene->content.controls.count;
    const u32 first = first_row(*scene, scene->content_offset);
    u32 end = first;
    const float bottom = scene->content_offset + ui_rect_height(f.body);
    while (end < count && row_start(*scene, end) < bottom)
        ++end;
    end = minimum(count, end + 1);
    if (!add(scene, scene->body, UiWidgetKind::container, utf8_literal("Before"), sized(width, row_start(*scene, first)))
             .generation)
        return false;
    for (u32 i = first; i < end; ++i) {
        const auto& c = scene->content.controls[i];
        const UiSettingsTarget target{UiSettingsTargetKind::control, scene->selected_page, c.id};
        const bool interactive_row = focusable_control(*scene, i);
        const auto kind = editor_kind(c.kind) ? UiWidgetKind::text_field : UiWidgetKind::button;
        if (!add(scene, scene->body, interactive_row ? kind : UiWidgetKind::container, utf8_literal("Setting"),
                 sized(row_width(*scene, aside, i), row_start(*scene, i + 1) - row_start(*scene, i)),
                 target,
                 i, interactive_row, EntryRole::row)
                 .generation)
            return false;
    }
    if (!add(scene, scene->body, UiWidgetKind::container, utf8_literal("After"),
             sized(width, content_extent(*scene, m) - row_start(*scene, end)))
             .generation)
        return false;
    scene->footer_status = scene->footer_save = scene->footer_reset = ui_widget_invalid_handle();
    if (scene->footer.visible) {
        style = placed(f.footer, viewport);
        // The bar centres its buttons; pin its own box with the bottom margin
        // so the overlay root cannot re-centre it below the frame.
        style.margin.bottom = maximum(0.f, viewport.bottom - f.footer.bottom);
        style.flow = UiLayoutFlow::horizontal;
        style.gap = 8;
        style.padding = {kUiSettingsFooterLeadingInset, kUiSettingsFooterInset, kUiSettingsFooterInset,
                         kUiSettingsFooterInset};
        if (scene->presentation == UiSettingsPresentation::dialog) {
            const float inset = (ui_rect_height(f.footer) - m.button_small) * .5f;
            style.padding = {kUiSettingsDialogPadding, inset, kUiSettingsDialogPadding, inset};
        }
        style.vertical_alignment = UiLayoutAlignment::center;
        const auto footer = add(scene, root, UiWidgetKind::container, utf8_literal("Unsaved footer"), style);
        scene->footer_status = add(scene, footer, UiWidgetKind::text, utf8_literal("Save status"), sized(-1, -1), {},
                                   0, false, EntryRole::footer_status);
        const float reset_width = ui_settings_button_width(
            t, UiSettingsButtonSize::compact,
            button_label_width(scene->footer.reset_label.bytes ? scene->footer.reset_label : utf8_literal("Reset")));
        const float save_width = ui_settings_button_width(
            t, UiSettingsButtonSize::compact,
            button_label_width(scene->footer.save_label.bytes ? scene->footer.save_label
                                                              : utf8_literal("Save changes")));
        scene->footer_reset = add(scene, footer, UiWidgetKind::button, utf8_literal("Reset draft"),
                                  sized(reset_width, m.button_small),
                                  {UiSettingsTargetKind::reset, scene->selected_page, 0}, 0, !scene->footer.saving);
        scene->footer_save =
            add(scene, footer, UiWidgetKind::button, utf8_literal("Save draft"), sized(save_width, m.button_small),
                {UiSettingsTargetKind::save, scene->selected_page, 0}, 0,
                scene->footer.can_save && !scene->footer.saving);
        if (!footer.generation || !scene->footer_status.generation || !scene->footer_reset.generation ||
            !scene->footer_save.generation)
            return false;
    }
    scene->close = add(scene, root, UiWidgetKind::button, utf8_literal("Close settings"),
                       placed(scene->layout.close, viewport), {UiSettingsTargetKind::close, {}, 0});
    if (!scene->close.generation ||
        !ui_layout_engine_set_scroll(&scene->widgets.layout, scene->widgets.storage[scene->sidebar.slot].layout,
                                     {0, scene->sidebar_offset}) ||
        !ui_layout_engine_set_scroll(&scene->widgets.layout, scene->widgets.storage[scene->body.slot].layout,
                                     {0, scene->content_offset}) ||
        !ui_widget_manager_layout_rectangle(&scene->widgets, scene->viewport).ok())
        return false;
    if (!target_exists(*scene, scene->focus))
        scene->focus = fallback_focus(*scene);
    ui_widget_manager_focus(&scene->widgets, target_handle(*scene, scene->focus));
    ++scene->revision;
    return true;
}
bool row_layout_for(const UiSettingsScene& scene, const UiSettingsLayoutTokens& t, UiWidgetHandle handle, u32 source,
                    RowLayout* layout, UiRect* clip = nullptr) noexcept {
    const auto* node = ui_widget_manager_layout_node(scene.widgets, handle);
    if (!node || source >= scene.content.controls.count)
        return false;
    *layout = row_layout(t, scene.style, scene.content, source, node->rectangle.left, node->rectangle.top,
                         ui_rect_width(node->rectangle));
    if (clip)
        *clip = node->clip_rectangle;
    return true;
}
// Settings controls answer the pointer only on their interactive
// part: the button, switch, select, field, slider track or an option. Rows
// that are themselves the action (navigation categories, button-less list
// rows) keep the whole row, and a grip keeps its handle for dragging.
bool row_pointer_region(const void* context, const UiWidgetManager&, UiWidgetHandle handle, float x,
                        float y) noexcept {
    const auto& scene = *static_cast<const UiSettingsScene*>(context);
    if (handle.slot >= scene.widgets.count)
        return false;
    const auto& entry = entries(scene)[handle.slot];
    if (entry.role != EntryRole::row || entry.target.kind != UiSettingsTargetKind::control ||
        entry.source_index >= scene.content.controls.count)
        return true;
    const u32 index = entry.source_index;
    const auto& c = scene.content.controls[index];
    UiSettingsActivation activation{};
    switch (c.kind) {
    case UiSettingsControlKind::category:
        return true;
    case UiSettingsControlKind::slider:
        return ui_settings_scene_slider_hit(scene, entry.target, x, y);
    case UiSettingsControlKind::palette:
        return ui_settings_scene_palette_pointer(scene, entry.target, x, y, &activation);
    case UiSettingsControlKind::radio:
        return ui_settings_scene_radio_pointer(scene, entry.target, x, y, &activation);
    case UiSettingsControlKind::segments:
    case UiSettingsControlKind::swatches:
    case UiSettingsControlKind::tabs:
    case UiSettingsControlKind::buttons:
    case UiSettingsControlKind::colors:
        return ui_settings_scene_option_pointer(scene, entry.target, x, y, &activation);
    case UiSettingsControlKind::error:
        return false;
    case UiSettingsControlKind::action:
        if (!c.action_label.bytes || ui_settings_scene_grip_hit(scene, entry.target, x, y))
            return true;
        break;
    default:
        break;
    }
    // Toggles, selects, editors, notice and row buttons: the control itself
    // (and the button beside an edit field).
    RowLayout row{};
    UiRect clip{};
    return row_layout_for(scene, SceneTokens(scene), handle, index, &row, &clip) &&
           (ui_rect_contains(ui_rect_overlap(row.control, clip), x, y) ||
            (c.kind == UiSettingsControlKind::edit && ui_rect_contains(ui_rect_overlap(row.value, clip), x, y)));
}
u64 layout_signature(const UiSettingsScene& scene, const UiSettingsPageContent& content, const UiSettingsStyle& style,
                     UiRect viewport, UiPageKey page, u32 registry_revision) noexcept {
    const auto aside = aside_layout(scene);
    u64 hash = hash64_bytes(kHash64Seed, &viewport, sizeof(viewport));
    hash = hash64_mix(hash, page.owner_id);
    hash = hash64_mix(hash, page.page_id);
    hash = hash64_mix(hash, registry_revision);
    hash = hash64_mix(hash, content.controls.count);
    hash = hash64_mix(hash, content.footer && content.footer->visible);
    hash = hash64_mix(hash, static_cast<u64>(scene.presentation));
    hash = hash64_mix(hash, scene.navigation_revision);
    hash = hash64_mix(hash, scene.navigation.count);
    hash = hash64_bytes(hash, &style, sizeof(style));
    hash = hash64_mix(hash, content.description.bytes);
    if (content.description.bytes)
        hash = hash64_bytes(hash, content.description.data, content.description.bytes);
    hash = hash64_mix(hash, content.sectioned);
    hash = hash64_bytes(hash, &content.aside_width, sizeof(content.aside_width));
    hash = hash64_bytes(hash, &content.aside_height, sizeof(content.aside_height));
    hash = hash64_mix(hash, content.aside_row);
    hash = hash64_mix(hash, content.aside_label.bytes);
    for (u32 i = 0; i < content.controls.count; ++i) {
        const auto& c = content.controls[i];
        hash = hash64_mix(hash, c.id);
        hash = hash64_mix(hash, static_cast<u32>(c.kind) | (c.enabled ? 0x100u : 0u));
        hash = hash64_mix(hash, c.choices.count);
        hash = hash64_bytes(hash, c.label.data, c.label.bytes);
        hash = hash64_bytes(hash, c.description.data, c.description.bytes);
        hash = hash64_bytes(hash, c.action_label.data, c.action_label.bytes);
        if (content.presentation)
            hash = hash64_bytes(hash, &content.presentation[i], sizeof(UiSettingsControlPresentation));
        if (const auto* item = list_item(content, i))
            hash = hash64_bytes(hash, item, sizeof(UiSettingsListItem));
        if (chips(content, i))
            for (u32 j = 0; j < c.choices.count; ++j)
                hash = hash64_bytes(hash, c.choices[j].data, c.choices[j].bytes);
        // A multi-line field relayouts when its text grows or shrinks a line.
        if (c.kind == UiSettingsControlKind::multiline)
            hash = hash64_mix(hash, static_cast<u64>(multiline_height(style, c, row_width(scene, aside, i)) * 4));
    }
    return hash;
}
void synchronize_input(UiSettingsScene& scene) noexcept {
    for (u32 i = 0; i < scene.widgets.count; ++i) {
        const auto target = entries(scene)[i].target;
        scene.widgets.storage[i].flags =
            target.kind != UiSettingsTargetKind::none && target_exists(scene, target) ? interactive : ui_widget_none;
    }
    ui_widget_manager_reconcile_input(&scene.widgets);
    if (!target_exists(scene, scene.focus))
        scene.focus = fallback_focus(scene);
    ui_widget_manager_focus(&scene.widgets, target_handle(scene, scene.focus));
}
bool text(UiTextOutput* stream, const UiSettingsScene& scene, UiWidgetHandle handle, const Utf8View& label, const UiRect& rectangle,
          const Color& color, u32 style = text_style_none, UiTextAlignment alignment = UiTextAlignment::start,
          TextWrapping wrapping = TextWrapping::no_wrap,
          UiTextAlignment vertical = UiTextAlignment::center) noexcept {
    const auto* node = ui_widget_manager_layout_node(scene.widgets, handle);
    if (!node)
        return false;
    const auto clip = ui_rect_overlap(rectangle, node->clip_rectangle);
    if (!label.bytes || !ui_rect_has_area(clip))
        return true;
    return ui_text_output_push(stream, {label,
                                        {&scene.widgets, handle},
                                        rectangle,
                                        clip,
                                        color,
                                        style,
                                        alignment,
                                        vertical,
                                        wrapping,
                                        TextOverflow::ellipsis});
}
// A wrapped, top-aligned description.
bool describe(UiTextOutput* stream, const UiSettingsScene& scene, UiWidgetHandle handle, const Utf8View& label,
              const UiRect& rectangle, const Color& color) noexcept {
    return text(stream, scene, handle, label, rectangle, color, text_description, UiTextAlignment::start,
                TextWrapping::wrap, UiTextAlignment::start);
}
// The control's state as seen by option `index`: hovered only when the pointer is on that option.
UiControlState hover_option(const UiSettingsScene& scene, UiControlState state, u32 index) noexcept {
    state.hovered = state.hovered && scene.palette_hovered_index == index;
    return state;
}
UiControlState widget_state(const UiSettingsScene& scene, UiWidgetHandle handle, const UiSettingsTarget& target,
                            bool enabled) noexcept {
    const bool hovered = ui_widget_manager_input_enabled(scene.widgets, handle) &&
                         ui_widget_handle_equal(scene.widgets.hovered, handle);
    const bool pressed = hovered && ui_widget_handle_equal(scene.widgets.captured, handle);
    return {enabled, hovered, pressed, scene.focus_visible && ui_settings_target_equal(scene.focus, target)};
}
float toggle_motion(const UiSettingsScene& scene, const UiSettingsTarget& target, const UiSettingsLayoutTokens& t,
                    bool checked) noexcept {
    for (const auto& animation : scene.animations) {
        if (!animation.started_ms || animation.control_id != target.control_id)
            continue;
        const u32 duration = scene.reduced_motion ? t.duration.switch_reduced_ms : t.duration.switch_ms;
        if (animation.checked != checked)
            return 1;
        const float elapsed = scene.clock_ms > animation.started_ms
                                  ? static_cast<float>(scene.clock_ms - animation.started_ms)
                                  : 0.f;
        return duration ? minimum(1.f, elapsed / static_cast<float>(duration)) : 1.f;
    }
    return 1;
}
void reveal_range(UiSettingsScene* scene, bool sidebar, float first, float last, const UiSettingsMetrics& m) noexcept {
    auto& offset = sidebar ? scene->sidebar_offset : scene->content_offset;
    const float visible = visible_height(*scene, sidebar, m);
    const float padding = sidebar ? kSidebarRevealPadding : kRevealPadding;
    if (first - padding < offset)
        offset = first - padding;
    else if (last + padding > offset + visible)
        offset = last - first + padding * 2 > visible ? first - padding : last + padding - visible;
    offset = clamp_offset(*scene, sidebar, offset, m);
}
float nav_offset_of(const UiSettingsScene& scene, const UiSettingsMetrics& m, const UiSettingsTarget& target,
                    float* height) noexcept {
    float offset = 0;
    u32 index = 0;
    bool found = false;
    visit_nav(scene, [&](const NavRow& row) noexcept {
        const float h = nav_height(m, row.kind, index++);
        found = ui_settings_target_equal(row.target, target);
        if (found)
            *height = h;
        else
            offset += h;
        return !found;
    });
    return found ? offset : -1;
}
bool apply_scroll(UiSettingsScene* scene, bool sidebar, float previous) noexcept {
    const float offset = sidebar ? scene->sidebar_offset : scene->content_offset;
    if (offset == previous)
        return true;
    if (!sidebar && first_row(*scene, previous) != first_row(*scene, offset))
        return build(scene);
    const auto scroller = sidebar ? scene->sidebar : scene->body;
    if (!ui_layout_engine_set_scroll(&scene->widgets.layout, scene->widgets.storage[scroller.slot].layout,
                                     {0, offset}) ||
        !ui_widget_manager_layout_rectangle(&scene->widgets, scene->viewport).ok())
        return false;
    ++scene->revision;
    return true;
}
// Ordered focus targets per zone, appended through a visitor.
bool visit_targets(const UiSettingsScene& scene, UiSettingsFocusZone zone,
                   UiVisitRef<UiSettingsTarget> visit) noexcept {
    if (zone == UiSettingsFocusZone::search)
        return scene.presentation == UiSettingsPresentation::modal && visit(UiSettingsTarget{UiSettingsTargetKind::search, {}, 0});
    if (zone == UiSettingsFocusZone::close)
        return visit(UiSettingsTarget{UiSettingsTargetKind::close, {}, 0});
    if (zone == UiSettingsFocusZone::sidebar) {
        bool visited = false;
        visit_nav(scene, [&](const NavRow& row) noexcept {
            visited = row.target.kind != UiSettingsTargetKind::none && visit(row.target);
            return !visited;
        });
        return visited;
    }
    for (u32 i = 0; i < scene.content.controls.count; ++i)
        if (focusable_control(scene, i) &&
            visit(UiSettingsTarget{UiSettingsTargetKind::control, scene.selected_page, scene.content.controls[i].id}))
            return true;
    if (scene.footer.visible) {
        const UiSettingsTarget reset{UiSettingsTargetKind::reset, scene.selected_page, 0};
        const UiSettingsTarget save{UiSettingsTargetKind::save, scene.selected_page, 0};
        if ((target_exists(scene, reset) && visit(reset)) || (target_exists(scene, save) && visit(save)))
            return true;
    }
    return false;
}
UiSettingsFocusZone zone_of(const UiSettingsTarget& target) noexcept {
    switch (target.kind) {
    case UiSettingsTargetKind::search:
    case UiSettingsTargetKind::search_clear:
        return UiSettingsFocusZone::search;
    case UiSettingsTargetKind::page:
    case UiSettingsTargetKind::action:
        return UiSettingsFocusZone::sidebar;
    case UiSettingsTargetKind::control:
    case UiSettingsTargetKind::save:
    case UiSettingsTargetKind::reset:
        return UiSettingsFocusZone::panel;
    case UiSettingsTargetKind::close:
        return UiSettingsFocusZone::close;
    default:
        return UiSettingsFocusZone::none;
    }
}
} // namespace

UiSettingsModalLayout ui_settings_modal_layout(const UiSettingsLayoutTokens& tokens, const UiRect& viewport,
                                               UiSettingsPresentation presentation) noexcept {
    const auto& m = tokens.metric;
    UiSettingsModalLayout l{};
    const float width = ui_rect_width(viewport), height = ui_rect_height(viewport);
    if (presentation == UiSettingsPresentation::dialog) {
        // One centred column; ui_settings_dialog_fit shrinks it to the content.
        l.compact = width < m.modal_compact_width || height < 420;
        l.modal = viewport;
        if (!l.compact) {
            const float dialog_width = minimum(kUiSettingsDialogWidth, width - m.modal_inset_side * 2);
            const float left = viewport.left + (width - dialog_width) * .5f;
            const float top = viewport.top + m.modal_inset_top;
            l.modal = {left, top, left + dialog_width,
                       top + maximum(1.f, height - m.modal_inset_top - m.modal_inset_bottom)};
        }
        l.sidebar = {l.modal.left, l.modal.top, l.modal.left, l.modal.bottom};
        l.panel = l.modal;
        const float pad = kUiSettingsDialogPadding;
        // A plain close glyph sits in the top-right corner, level with the title.
        l.close = ui_settings_close_rect(tokens, l.panel.right - pad + 4 - m.close_size * .5f, l.panel.top + pad - 6);
        l.column = {l.panel.left + pad, l.panel.top + pad, maximum(l.panel.left + pad + 1, l.panel.right - pad),
                    l.panel.bottom};
        return l;
    }
    if (presentation == UiSettingsPresentation::modal) {
        l.compact = width < m.modal_compact_width || height < 420;
        l.modal = viewport;
        if (!l.compact) {
            const float available_width = width - m.modal_inset_side * 2;
            const float available_height = height - m.modal_inset_top - m.modal_inset_bottom;
            float modal_width = available_width;
            if (width >= m.modal_ratio_threshold)
                modal_width = maximum(width * m.modal_viewport_ratio, m.modal_ratio_threshold - m.modal_inset_side * 2);
            modal_width = minimum(minimum(modal_width, available_width), m.modal_max_width);
            const float left = viewport.left + (width - modal_width) * .5f;
            const float top = viewport.top + m.modal_inset_top;
            l.modal = {left, top, left + modal_width, top + maximum(1.f, available_height)};
        }
        const float sidebar = minimum(m.sidebar_width, ui_rect_width(l.modal) * (l.compact ? .32f : .36f));
        l.sidebar = {l.modal.left, l.modal.top, l.modal.left + sidebar, l.modal.bottom};
        l.panel = {l.sidebar.right, l.modal.top, l.modal.right, l.modal.bottom};
        const float close_right = l.panel.right - 20;
        l.close = ui_settings_close_rect(tokens, close_right - m.close_size * .5f, l.panel.top + 20);
        // Small windows halve the panel's side padding so the form keeps
        // room for a label beside its control.
        const float left = l.panel.left + (l.compact ? m.panel_padding_x * .5f : m.panel_padding_x);
        const float right_limit = l.close.left - 16;
        l.column = {left, l.panel.top + m.panel_padding_top, maximum(left + 1, minimum(right_limit, left + m.panel_max_width)),
                    l.panel.bottom};
        return l;
    }
    // Layer: growing sidebar region with a right-aligned column, then content.
    // Below sidebar + 640 DIP the sidebar, padding and close gap shrink
    // together (down to the 632 DIP minimum window) so the form column
    // keeps at least ~360 DIP instead of collapsing beside a full sidebar.
    l.modal = viewport;
    const float extra = maximum(0.f, width - m.sidebar_width - 800);
    const float squeeze = minimum(1.f, maximum(0.f, m.sidebar_width + 640 - width) / 256.f);
    const float sidebar = maximum(minimum(width * .4f, kUiSettingsLayerMinimumSidebar),
                                  minimum(width * .4f, m.sidebar_width + extra * .5f) -
                                      (m.sidebar_width - kUiSettingsLayerMinimumSidebar) * squeeze);
    l.sidebar = {viewport.left, viewport.top, viewport.left + sidebar, viewport.bottom};
    l.panel = {l.sidebar.right, viewport.top, viewport.right, viewport.bottom};
    const float padding = m.panel_padding_x * (1 - .5f * squeeze), close_gap = 24 - 8 * squeeze;
    const float left = l.panel.left + padding;
    const float right = minimum(l.panel.right - padding - m.close_size - close_gap, left + m.panel_max_width);
    l.column = {left, viewport.top + 56, maximum(left + 1, right), viewport.bottom};
    l.close = ui_settings_close_rect(tokens, l.column.right + close_gap + m.close_size * .5f, viewport.top + 56);
    return l;
}

void ui_settings_dialog_fit(UiSettingsModalLayout* layout, const UiRect& viewport, float content_height) noexcept {
    if (!layout || layout->compact || !(content_height > 0))
        return;
    const float spare = ui_rect_height(layout->modal) - content_height;
    if (spare <= 0)
        return;
    // Centre within the viewport, inside the insets of the full-height dialog.
    const float centred = viewport.top + (ui_rect_height(viewport) - content_height) * .5f;
    const float top = minimum(layout->modal.top + spare, maximum(layout->modal.top, centred));
    const float shift = top - layout->modal.top;
    const auto move = [&](UiRect& r, bool bottom_follows_modal) noexcept {
        r.top += shift;
        r.bottom = bottom_follows_modal ? top + content_height : r.bottom + shift;
    };
    move(layout->modal, true);
    move(layout->sidebar, true);
    move(layout->panel, true);
    move(layout->column, true);
    move(layout->close, false);
}

bool ui_settings_style_valid(const UiSettingsStyle& s) noexcept {
    const float values[]{s.form_width,       s.field_padding, s.group_gap,      s.single_height,
                         s.multiline_height, s.field_radius,  s.control_radius, s.footer_height};
    for (float value : values)
        if (!ui_extent_valid(value))
            return false;
    return s.form_width >= 240 && s.form_width <= 2048 && s.field_padding >= 4 && s.field_padding <= 24 &&
           s.group_gap >= 8 && s.group_gap <= 128 && s.single_height >= 32 && s.single_height <= 128 &&
           s.multiline_height >= 96 && s.multiline_height <= 1024 && s.field_radius <= 32 &&
           s.control_radius <= 32 && s.footer_height >= 64 && s.footer_height <= 128 &&
           s.field_padding * 2 < s.multiline_height;
}

bool ui_settings_scene_bind_theme_styles(UiSettingsScene* scene, const UiResolvedThemeStyles* styles) noexcept {
    if (!scene)
        return false;
    const u32 revision = styles && styles->ready ? styles->geometry_revision : 0;
    if (scene->theme_styles == styles && scene->theme_geometry_revision == revision)
        return false;
    scene->theme_styles = styles;
    scene->theme_geometry_revision = revision;
    scene->layout_signature = 0;
    return true;
}

bool ui_settings_scene_set_presentation(UiSettingsScene* scene, UiSettingsPresentation presentation) noexcept {
    if (!scene || presentation > UiSettingsPresentation::dialog || scene->presentation == presentation)
        return false;
    scene->presentation = presentation;
    scene->layout_signature = 0;
    return true;
}

bool ui_settings_scene_bind_navigation(UiSettingsScene* scene, ConstSlice<UiSettingsNavEntry> navigation,
                                       u32 revision) noexcept {
    if (!scene || !valid_navigation(navigation))
        return false;
    if (scene->navigation.data == navigation.data && scene->navigation.count == navigation.count &&
        scene->navigation_revision == revision)
        return false;
    scene->navigation = navigation;
    scene->navigation_revision = revision;
    scene->layout_signature = 0;
    return true;
}

bool ui_settings_scene_set_search(UiSettingsScene* scene, const UiSettingsEditor& search) noexcept {
    if (!scene || !valid_text(search.view.text) || !valid_text(search.placeholder) || !geometry_valid(search.geometry))
        return false;
    const auto& a = scene->search;
    if (a.view.text.data == search.view.text.data && a.view.text.bytes == search.view.text.bytes &&
        a.view.anchor == search.view.anchor && a.view.caret == search.view.caret &&
        a.view.focused == search.view.focused && a.view.caret_visible == search.view.caret_visible &&
        a.geometry == search.geometry && a.placeholder.data == search.placeholder.data)
        return false;
    const bool clear_changed = (a.view.text.bytes != 0) != (search.view.text.bytes != 0);
    scene->search = search;
    if (clear_changed && scene->active)
        synchronize_input(*scene);
    ++scene->revision;
    return true;
}

bool ui_settings_scene_search_geometry(const UiSettingsScene& scene, UiSettingsControlGeometry* output) noexcept {
    if (output)
        reset_value(output);
    const auto* node = output && current(scene) && scene.search_field.generation
                           ? ui_widget_manager_layout_node(scene.widgets, scene.search_field)
                           : nullptr;
    if (!node)
        return false;
    const SceneTokens t(scene);
    const UiSettingsTarget target{UiSettingsTargetKind::search, {}, 0};
    *output = {target,
               {&scene.widgets, scene.search_field},
               node->rectangle,
               node->rectangle,
               ui_settings_search_text_rect(t, node->rectangle),
               node->clip_rectangle,
               scene.revision,
               scene.content_revision,
               true,
               false};
    return ui_rect_has_area(output->clip);
}

bool ui_settings_scene_set_flash(UiSettingsScene* scene, u64 control_id, float strength) noexcept {
    if (!scene)
        return false;
    strength = strength > 0 ? minimum(strength, 1.f) : 0.f;
    if (!strength)
        control_id = 0;
    if (scene->flash_control == control_id && scene->flash == strength)
        return false;
    scene->flash_control = control_id;
    scene->flash = strength;
    ++scene->motion_revision;
    return true;
}

bool ui_settings_scene_animate_toggle(UiSettingsScene* scene, const UiSettingsTarget& target, bool checked,
                                      u64 now_ms) noexcept {
    if (!scene || target.kind != UiSettingsTargetKind::control || !now_ms)
        return false;
    if (!ui_page_key_equal(target.page, scene->selected_page))
        return false;
    u32 slot = 0;
    for (u32 i = 0; i < kUiSettingsAnimationSlots; ++i) {
        if (scene->animations[i].control_id == target.control_id) {
            slot = i;
            break;
        }
        if (scene->animations[i].started_ms < scene->animations[slot].started_ms)
            slot = i;
    }
    scene->animations[slot] = {target.control_id, now_ms, checked};
    scene->clock_ms = now_ms;
    ++scene->motion_revision;
    return true;
}

bool ui_settings_scene_tick(UiSettingsScene* scene, u64 now_ms, bool reduced_motion) noexcept {
    if (!scene)
        return false;
    scene->reduced_motion = reduced_motion;
    bool animating = false;
    const SceneTokens t(*scene);
    const u32 duration = reduced_motion ? t.duration.switch_reduced_ms : t.duration.switch_ms;
    for (auto& animation : scene->animations) {
        if (!animation.started_ms)
            continue;
        if (now_ms >= animation.started_ms + duration + kUiTransitionFrameMs)
            reset_value(&animation);
        else
            animating = true;
    }
    if (scene->clock_ms != now_ms && (animating || scene->clock_ms)) {
        scene->clock_ms = animating ? now_ms : 0;
        ++scene->motion_revision;
    }
    return animating;
}

bool ui_settings_scene_animating(const UiSettingsScene& scene) noexcept {
    for (const auto& animation : scene.animations)
        if (animation.started_ms)
            return true;
    return false;
}

bool ui_settings_page_content_valid(const UiSettingsPageContent& content) noexcept {
    return valid_content(content);
}
bool ui_settings_scene_update(UiSettingsScene* scene, UiPageRegistry* registry, const UiRect& viewport_view) noexcept {
    const UiRect viewport = viewport_view; // the scene is written below
    if (!scene || !registry || !ui_rect_valid(viewport) || ui_rect_width(viewport) < 320 ||
        ui_rect_height(viewport) < 200 || ui_rect_height(viewport) > 8192 || registry->count > 16384)
        return false;
    const auto* page = ui_page_registry_selected(*registry);
    UiSettingsPageContent content{utf8_literal("Settings"), {}, {}, 0};
    UiPageKey selected{};
    if (page) {
        if (!page->context)
            return false;
        content = *static_cast<const UiSettingsPageContent*>(page->context);
        selected = page->key;
    }
    const bool unchanged =
        scene->active && scene->registry == registry && scene->registry_revision == registry->revision &&
        ui_page_key_equal(selected, scene->selected_page) && scene->content_revision == content.revision &&
        ui_rect_equal(scene->viewport, viewport) &&
        scene->projected_theme_geometry_revision == scene->theme_geometry_revision &&
        scene->projected_navigation_revision == scene->navigation_revision && scene->layout_signature;
    if (unchanged)
        return true;
    if (!valid_content(content))
        return false;
    UiSettingsStyle style =
        content.style && ui_settings_style_valid(*content.style) ? *content.style : kUiDefaultSettingsStyle;
    if (scene->theme_styles && scene->theme_styles->ready)
        style = ui_theme_settings_style(scene->theme_styles);
    const bool changed_page = scene->registry != registry || !ui_page_key_equal(selected, scene->selected_page);
    if ((!scene->active || changed_page) && scene->page_generation == ~u64{0})
        return false;
    // Validate once before touching retained state, then reuse the same geometry.
    const SceneTokens tokens(*scene);
    const auto layout = ui_settings_modal_layout(tokens, viewport, scene->presentation);
    const float width = maximum(1.f, ui_rect_width(layout.column));
    float rows = 0;
    for (u32 i = 0; i < content.controls.count; ++i) {
        rows += row_layout(tokens, style, content, i, 0, 0, width).height;
        if (rows > kMaximumUiCoordinate - 8192)
            return false;
    }
    const bool previous_active = scene->active;
    const auto* previous_registry = scene->registry;
    const u64 previous_signature = scene->layout_signature;
    const u32 previous_theme = scene->projected_theme_geometry_revision;
    scene->registry = registry;
    copy_value(&scene->content, content);
    scene->style = style;
    scene->footer = content.footer ? *content.footer : UiSettingsFooter{};
    scene->viewport = viewport;
    scene->layout = layout;
    if (scene->presentation == UiSettingsPresentation::dialog) {
        // Fit the dialog to its title, rows and footer band.
        const auto f = frame(*scene, tokens.metric);
        ui_settings_dialog_fit(&scene->layout, viewport,
                               f.body.top - scene->layout.modal.top + rows + kUiSettingsDialogBodyEnd +
                                   (scene->footer.visible ? dialog_footer_height(tokens.metric)
                                                          : kUiSettingsDialogPadding));
    }
    const u64 signature = layout_signature(*scene, content, style, viewport, selected, registry->revision);
    const bool retain_layout = previous_active && previous_registry == registry && previous_signature == signature &&
                               previous_theme == scene->theme_geometry_revision;
    if (!scene->active || changed_page)
        ++scene->page_generation;
    if (!scene->active)
        ++scene->opens;
    scene->registry_revision = registry->revision;
    scene->content_revision = content.revision;
    scene->projected_theme_geometry_revision = scene->theme_geometry_revision;
    scene->projected_navigation_revision = scene->navigation_revision;
    scene->layout_signature = signature;
    scene->selected_page = selected;
    if (changed_page) {
        scene->content_offset = 0;
        scene->flash_control = 0;
        scene->flash = 0;
        for (auto& animation : scene->animations)
            reset_value(&animation);
    }
    if (retain_layout) {
        synchronize_input(*scene);
        ++scene->revision;
        return true;
    }
    if (!build(scene)) {
        ui_settings_scene_release(scene);
        return false;
    }
    scene->active = true;
    return true;
}
void ui_settings_scene_close(UiSettingsScene* scene) noexcept {
    ui_settings_scene_release(scene);
}
void ui_settings_scene_release(UiSettingsScene* scene) noexcept {
    if (!scene)
        return;
    const u32 revision = scene->revision + 1;
    const u64 page_generation = scene->page_generation;
    const auto presentation = scene->presentation;
    const u64 opens = scene->opens;
    ui_fixed_widget_storage_release(&scene->storage);
    reset_value(scene);
    scene->revision = revision;
    scene->page_generation = page_generation;
    scene->presentation = presentation;
    scene->opens = opens;
}
UiSettingsTarget ui_settings_scene_hit_test(const UiSettingsScene& scene, float x, float y) noexcept {
    if (!current(scene))
        return {};
    const auto handle = ui_widget_manager_hit_test(scene.widgets, x, y);
    if (!handle.generation)
        return {};
    const auto target = entries(scene)[handle.slot].target;
    if (target.kind == UiSettingsTargetKind::search && scene.search.view.text.bytes) {
        const SceneTokens t(scene);
        const auto* node = ui_widget_manager_layout_node(scene.widgets, handle);
        if (node && ui_rect_contains(ui_settings_search_clear_rect(t, node->rectangle), x, y))
            return {UiSettingsTargetKind::search_clear, {}, 0};
    }
    return target;
}
UiSettingsTarget ui_settings_scene_focused(const UiSettingsScene& scene) noexcept {
    return current(scene) ? scene.focus : UiSettingsTarget{};
}
UiSettingsFocusZone ui_settings_scene_focus_zone(const UiSettingsScene& scene) noexcept {
    return current(scene) ? zone_of(scene.focus) : UiSettingsFocusZone::none;
}
bool ui_settings_scene_reveal(UiSettingsScene* scene, const UiSettingsTarget& target) noexcept {
    if (!scene || !current(*scene))
        return false;
    const SceneTokens t(*scene);
    const auto& m = t.metric;
    if (target.kind == UiSettingsTargetKind::page || target.kind == UiSettingsTargetKind::action) {
        float height = 0;
        const float top = nav_offset_of(*scene, m, target, &height);
        if (top < 0)
            return false;
        const float previous = scene->sidebar_offset;
        reveal_range(scene, true, top, top + height, m);
        return apply_scroll(scene, true, previous);
    }
    u32 row = 0;
    if (target.kind != UiSettingsTargetKind::control || !find_control(*scene, target, &row))
        return target.kind != UiSettingsTargetKind::none;
    const float previous = scene->content_offset;
    reveal_range(scene, false, row_start(*scene, row), row_start(*scene, row + 1), m);
    return apply_scroll(scene, false, previous);
}
bool ui_settings_scene_focus(UiSettingsScene* scene, UiSettingsTarget target) noexcept {
    if (!scene || !current(*scene) || !target_exists(*scene, target))
        return false;
    if (target.kind == UiSettingsTargetKind::search_clear)
        target = {UiSettingsTargetKind::search, {}, 0};
    if (!ui_settings_scene_reveal(scene, target))
        return false;
    const auto handle = target_handle(*scene, target);
    if (handle.generation && !ui_widget_manager_focus(&scene->widgets, handle))
        return false;
    if (!ui_settings_target_equal(scene->focus, target)) {
        scene->focus = target;
        ++scene->revision;
    }
    if (!handle.generation)
        return build(scene);
    return true;
}
bool ui_settings_scene_move_focus(UiSettingsScene* scene, bool reverse) noexcept {
    if (!scene || !current(*scene))
        return false;
    UiSettingsTarget order[256];
    u32 count = 0;
    const UiSettingsFocusZone zones[]{UiSettingsFocusZone::search, UiSettingsFocusZone::sidebar,
                                      UiSettingsFocusZone::panel, UiSettingsFocusZone::close};
    for (const auto zone : zones)
        visit_targets(*scene, zone, [&](UiSettingsTarget target) noexcept {
            if (count < 256 && target_exists(*scene, target))
                order[count++] = target;
            return false;
        });
    if (!count)
        return false;
    u32 position = count;
    for (u32 i = 0; i < count; ++i)
        if (ui_settings_target_equal(order[i], scene->focus))
            position = i;
    const u32 next = position == count ? (reverse ? count - 1 : 0)
                     : reverse         ? (position ? position - 1 : count - 1)
                                       : (position + 1 == count ? 0 : position + 1);
    return ui_settings_scene_focus(scene, order[next]);
}
bool ui_settings_scene_move_zone(UiSettingsScene* scene, bool reverse) noexcept {
    if (!scene || !current(*scene))
        return false;
    const UiSettingsFocusZone zones[]{UiSettingsFocusZone::search, UiSettingsFocusZone::sidebar,
                                      UiSettingsFocusZone::panel, UiSettingsFocusZone::close};
    const auto zone = zone_of(scene->focus);
    u32 position = 0;
    for (u32 i = 0; i < 4; ++i)
        if (zones[i] == zone)
            position = i;
    for (u32 step = 1; step <= 4; ++step) {
        const auto candidate = zones[reverse ? (position + 4 - step) % 4 : (position + step) % 4];
        UiSettingsTarget chosen{};
        if (candidate == UiSettingsFocusZone::sidebar) {
            const UiSettingsTarget selected{UiSettingsTargetKind::page, scene->selected_page, 0};
            if (target_exists(*scene, selected))
                chosen = selected;
        }
        if (chosen.kind == UiSettingsTargetKind::none)
            visit_targets(*scene, candidate, [&](UiSettingsTarget target) noexcept {
                if (!target_exists(*scene, target))
                    return false;
                // The panel resumes at the first row still in view.
                if (candidate == UiSettingsFocusZone::panel && target.kind == UiSettingsTargetKind::control) {
                    u32 row = 0;
                    if (find_control(*scene, target, &row) && row_start(*scene, row + 1) <= scene->content_offset)
                        return false;
                }
                chosen = target;
                return true;
            });
        if (chosen.kind != UiSettingsTargetKind::none)
            return ui_settings_scene_focus(scene, chosen);
    }
    return false;
}
bool ui_settings_scene_move_within(UiSettingsScene* scene, bool reverse) noexcept {
    if (!scene || !current(*scene))
        return false;
    const auto zone = zone_of(scene->focus);
    if (zone != UiSettingsFocusZone::sidebar && zone != UiSettingsFocusZone::panel)
        return false;
    UiSettingsTarget previous{}, chosen{};
    bool found = false;
    visit_targets(*scene, zone, [&](UiSettingsTarget target) noexcept {
        if (!target_exists(*scene, target))
            return false;
        if (found) {
            if (!reverse) {
                chosen = target;
                return true;
            }
            return false;
        }
        if (ui_settings_target_equal(target, scene->focus)) {
            found = true;
            if (reverse) {
                chosen = previous;
                return true;
            }
            return false;
        }
        previous = target;
        return false;
    });
    return chosen.kind != UiSettingsTargetKind::none && ui_settings_scene_focus(scene, chosen);
}
void ui_settings_scene_set_focus_visible(UiSettingsScene* scene, bool visible) noexcept {
    if (!scene || scene->focus_visible == visible)
        return;
    scene->focus_visible = visible;
    ++scene->revision;
}
bool ui_settings_scene_select(UiSettingsScene* scene, UiPageKey page) noexcept {
    if (!scene || !current(*scene) || !ui_page_registry_select(scene->registry, page).ok())
        return false;
    if (!ui_settings_scene_update(scene, scene->registry, scene->viewport))
        return false;
    return ui_settings_scene_focus(scene, {UiSettingsTargetKind::page, page, 0});
}
bool ui_settings_scene_scroll(UiSettingsScene* scene, bool sidebar, float delta) noexcept {
    if (!scene || !current(*scene) || !ui_coordinate_valid(delta))
        return false;
    const SceneTokens t(*scene);
    auto& offset = sidebar ? scene->sidebar_offset : scene->content_offset;
    const float previous = offset;
    offset = clamp_offset(*scene, sidebar, offset + delta, t.metric);
    return apply_scroll(scene, sidebar, previous);
}
UiScrollbarGeometry ui_settings_scene_scrollbar(const UiSettingsScene& scene, bool sidebar) noexcept {
    if (!current(scene))
        return {};
    const auto* node = ui_widget_manager_layout_node(scene.widgets, sidebar ? scene.sidebar : scene.body);
    if (!node)
        return {};
    const SceneTokens t(scene);
    auto viewport = ui_rect_overlap(node->rectangle, node->clip_rectangle);
    if (sidebar)
        viewport.right = minimum(scene.layout.sidebar.right, viewport.right + 12);
    return ui_scrollbar_geometry(viewport, sidebar ? scene.sidebar_offset : scene.content_offset,
                                 sidebar ? nav_extent(scene, t.metric) : content_extent(scene, t.metric));
}
bool ui_settings_scene_activation(const UiSettingsScene& scene, const UiSettingsTarget& target, bool reverse,
                                  UiSettingsActivation* activation) noexcept {
    if (!activation || !current(scene) || !target_exists(scene, target))
        return false;
    *activation = {target, UiSettingsControlKind::action, false, 0};
    if (target.kind != UiSettingsTargetKind::control)
        return true;
    const auto& c = *find_control(scene, target);
    activation->kind = c.kind;
    activation->checked = !c.checked;
    activation->selected_choice = c.selected_choice;
    const u32 count = c.choices.count;
    // Button groups and colour swatches press their first option.
    if (c.kind == UiSettingsControlKind::buttons || c.kind == UiSettingsControlKind::colors) {
        activation->selected_choice = c.selected_choice < count ? c.selected_choice : 0;
        return count != 0;
    }
    if (c.kind == UiSettingsControlKind::choice || c.kind == UiSettingsControlKind::radio ||
        c.kind == UiSettingsControlKind::palette || c.kind == UiSettingsControlKind::segments ||
        c.kind == UiSettingsControlKind::swatches || c.kind == UiSettingsControlKind::tabs) {
        const bool none = c.selected_choice >= count;
        activation->selected_choice = none        ? (reverse ? count - 1 : 0)
                                      : reverse   ? (c.selected_choice ? c.selected_choice - 1 : count - 1)
                                                  : (c.selected_choice + 1 == count ? 0 : c.selected_choice + 1);
    }
    if (c.kind == UiSettingsControlKind::slider)
        activation->selected_choice =
            reverse ? (c.selected_choice ? c.selected_choice - 1 : 0) : minimum(c.selected_choice + 1, count - 1);
    return true;
}
const UiSettingsControl* ui_settings_scene_control(const UiSettingsScene& scene, const UiSettingsTarget& target,
                                                   u32* index) noexcept {
    return current(scene) ? find_control(scene, target, index) : nullptr;
}
bool ui_settings_scene_control_anchor(const UiSettingsScene& scene, const UiSettingsTarget& target, UiRect* anchor) noexcept {
    const auto* control = ui_settings_scene_control(scene, target);
    if (!control || !anchor || !control->enabled)
        return false;
    UiSettingsControlGeometry geometry{};
    if (!ui_settings_scene_control_geometry(scene, target, &geometry))
        return false;
    *anchor = geometry.clip;
    return true;
}
bool ui_settings_scene_control_geometry(const UiSettingsScene& scene, const UiSettingsTarget& target,
                                        UiSettingsControlGeometry* output) noexcept {
    if (output)
        reset_value(output);
    u32 index = 0;
    const auto* control = ui_settings_scene_control(scene, target, &index);
    if (!output || !control)
        return false;
    const auto handle = target_handle(scene, target);
    const SceneTokens t(scene);
    RowLayout row{};
    UiRect clip{};
    if (!row_layout_for(scene, t, handle, index, &row, &clip))
        return false;
    UiRect field = ui_rect_has_area(row.control) ? row.control : row.row;
    UiRect text_rectangle = field;
    if (editor_kind(control->kind)) {
        const float pad = scene.style.field_padding;
        const float vertical = control->kind == UiSettingsControlKind::multiline
                                   ? pad
                                   : minimum(pad, maximum(0.f, (ui_rect_height(field) - 24.f) * .5f));
        text_rectangle = {field.left + pad, field.top + vertical, field.right - pad, field.bottom - vertical};
    } else if (control->kind == UiSettingsControlKind::choice)
        text_rectangle = ui_select_text_rect(t, field);
    const auto visible = ui_rect_overlap(field, clip);
    if (!ui_rect_has_area(visible) || !ui_rect_has_area(text_rectangle))
        return false;
    *output = {target,
               {&scene.widgets, handle},
               row.row,
               field,
               text_rectangle,
               visible,
               scene.revision,
               scene.content_revision,
               control->enabled,
               control->kind == UiSettingsControlKind::multiline};
    return true;
}
bool ui_settings_scene_editor_hit(const UiSettingsScene& scene, float x, float y,
                                  UiSettingsControlGeometry* output) noexcept {
    if (output)
        reset_value(output);
    const auto target = ui_settings_scene_hit_test(scene, x, y);
    const auto* control = ui_settings_scene_control(scene, target);
    return control && control->enabled && editor_kind(control->kind) &&
           ui_settings_scene_control_geometry(scene, target, output) && ui_rect_contains(output->clip, x, y);
}
bool ui_settings_scene_bind_palette(UiSettingsScene* scene, UiSettingsPaletteSource source) noexcept {
    if (!scene || (source.resolve && (!source.context || !source.revision)) ||
        (!source.resolve && (source.context || source.revision)))
        return false;
    if (scene->palette.context == source.context && scene->palette.resolve == source.resolve &&
        scene->palette.revision == source.revision)
        return true;
    scene->palette = source;
    scene->palette_hovered_index = ~u32{0};
    ++scene->revision;
    return true;
}
namespace {
// Fills the semantic request of choice `choice` on a control.
bool report(const UiSettingsTarget& target, const UiSettingsControl& c, u32 choice, UiSettingsActivation* output) noexcept {
    *output = {target, c.kind, c.checked, choice};
    return true;
}
} // namespace
bool ui_settings_scene_slider_geometry(const UiSettingsScene& scene, const UiSettingsTarget& target,
                                       UiSettingsSliderGeometry* output) noexcept {
    u32 index = 0;
    const auto* c = ui_settings_scene_control(scene, target, &index);
    UiSettingsControlGeometry geometry{};
    if (!output || !c || c->kind != UiSettingsControlKind::slider ||
        !ui_settings_scene_control_geometry(scene, target, &geometry))
        return false;
    const SceneTokens t(scene);
    const auto slider = ui_slider_geometry(t, geometry.field, c->choices.count, c->selected_choice,
                                           marks(scene.content, index));
    const UiRect track{slider.track_left, slider.bar.top, slider.track_right, slider.bar.bottom};
    if (!ui_rect_has_area(track) || !ui_rect_intersects(slider.bar, geometry.clip))
        return false;
    *output = {geometry, track, slider.grabber};
    return true;
}
bool ui_settings_scene_slider_pointer(const UiSettingsScene& scene, const UiSettingsTarget& target, float x,
                                      UiSettingsActivation* output) noexcept {
    UiSettingsSliderGeometry geometry{};
    if (!output || !ui_coordinate_valid(x) || !ui_settings_scene_slider_geometry(scene, target, &geometry) ||
        !geometry.control.enabled)
        return false;
    const auto* c = ui_settings_scene_control(scene, target);
    const float fraction = maximum(0.f, minimum(1.f, (x - geometry.track.left) / ui_rect_width(geometry.track)));
    return report(target, *c, static_cast<u32>(fraction * (c->choices.count - 1) + .5f), output);
}
bool ui_settings_scene_slider_hit(const UiSettingsScene& scene, const UiSettingsTarget& target, float x, float y) noexcept {
    UiSettingsSliderGeometry geometry{};
    if (!ui_coordinate_valid(x) || !ui_coordinate_valid(y) ||
        !ui_settings_scene_slider_geometry(scene, target, &geometry) || !geometry.control.enabled)
        return false;
    const float middle = (geometry.track.top + geometry.track.bottom) * .5f;
    return ui_rect_contains(ui_rect_intersection({geometry.track.left - 10, middle - 14, geometry.track.right + 10,
                                                  middle + 14},
                                                 geometry.control.clip),
                            x, y);
}
bool ui_settings_scene_slider_key(const UiSettingsScene& scene, const UiSettingsTarget& target, UiSettingsSliderKey key,
                                  UiSettingsActivation* output) noexcept {
    const auto* c = ui_settings_scene_control(scene, target);
    if (!output || !c || !c->enabled || !target_exists(scene, target) ||
        (c->kind != UiSettingsControlKind::slider && c->kind != UiSettingsControlKind::palette &&
         c->kind != UiSettingsControlKind::radio && c->kind != UiSettingsControlKind::segments &&
         c->kind != UiSettingsControlKind::swatches) ||
        key > UiSettingsSliderKey::page_increase)
        return false;
    const u32 last = c->choices.count - 1;
    const bool up = key == UiSettingsSliderKey::increase || key == UiSettingsSliderKey::page_increase;
    const bool page = key == UiSettingsSliderKey::page_decrease || key == UiSettingsSliderKey::page_increase;
    const u32 step = page && c->kind == UiSettingsControlKind::slider ? ui_slider_page_step(c->choices.count) : 1u;
    const u32 selected = c->selected_choice > last ? (up ? ~0u : last + 1) : c->selected_choice;
    const u32 value = key == UiSettingsSliderKey::home  ? 0
                      : key == UiSettingsSliderKey::end ? last
                      : !up ? (selected > step ? minimum(selected - step, last) : 0)
                            : (selected == ~0u ? 0 : minimum(selected + step, last));
    return report(target, *c, value, output);
}
bool ui_settings_scene_palette_pointer(const UiSettingsScene& scene, const UiSettingsTarget& target, float x, float y,
                                       UiSettingsActivation* output) noexcept {
    const auto* c = ui_settings_scene_control(scene, target);
    UiSettingsControlGeometry geometry{};
    if (!output || !ui_coordinate_valid(x) || !ui_coordinate_valid(y) || !c ||
        c->kind != UiSettingsControlKind::palette || !c->enabled ||
        !ui_settings_scene_control_geometry(scene, target, &geometry) || !ui_rect_contains(geometry.clip, x, y))
        return false;
    const SceneTokens t(scene);
    const u32 columns = ui_swatch_columns(t, ui_rect_width(geometry.field));
    for (u32 i = 0; i < c->choices.count; ++i) {
        auto swatch = ui_swatch_rect(t, geometry.field, i, columns);
        swatch.bottom += ui_swatch_label_height();
        if (ui_rect_contains(swatch, x, y)) {
            return report(target, *c, i, output);
        }
    }
    return false;
}
bool ui_settings_scene_radio_pointer(const UiSettingsScene& scene, const UiSettingsTarget& target, float x, float y,
                                     UiSettingsActivation* output) noexcept {
    const auto* c = ui_settings_scene_control(scene, target);
    UiSettingsControlGeometry geometry{};
    if (!output || !ui_coordinate_valid(x) || !ui_coordinate_valid(y) || !c ||
        c->kind != UiSettingsControlKind::radio || !c->enabled ||
        !ui_settings_scene_control_geometry(scene, target, &geometry) || !ui_rect_contains(geometry.clip, x, y))
        return false;
    const SceneTokens t(scene);
    const u32 choice = ui_radio_hit(t, geometry.field, c->choices.count, x, y);
    if (choice == ~0u)
        return false;
    return report(target, *c, choice, output);
}
bool ui_settings_scene_button_hit(const UiSettingsScene& scene, const UiSettingsTarget& target, float x, float y) noexcept {
    u32 index = 0;
    const auto* c = ui_settings_scene_control(scene, target, &index);
    if (c && c->kind == UiSettingsControlKind::edit && c->action_label.bytes && c->enabled) {
        RowLayout row{};
        UiRect clip{};
        return row_layout_for(scene, SceneTokens(scene), target_handle(scene, target), index, &row, &clip) &&
               ui_rect_contains(ui_rect_overlap(row.value, clip), x, y);
    }
    UiSettingsControlGeometry geometry{};
    return c && (c->kind == UiSettingsControlKind::notice || c->kind == UiSettingsControlKind::action) &&
           ui_settings_scene_control_geometry(scene, target, &geometry) && ui_rect_contains(geometry.clip, x, y);
}
namespace {
// One option of a tabs, buttons or colors row inside its control rectangle;
// colours report their trailing button as index choices.count.
UiRect option_rect(const UiSettingsLayoutTokens& t, const UiSettingsControl& c, const UiRect& field, u32 index) noexcept {
    float x = field.left;
    if (c.kind == UiSettingsControlKind::tabs) {
        for (u32 i = 0; i < c.choices.count; ++i) {
            const float w = ui_settings_text_width(c.choices[i], kUiSettingsLabelEm) * 1.04f;
            if (i == index)
                return {x, field.top, minimum(field.right, x + w), field.bottom};
            x += w + kTabGap;
        }
        return {};
    }
    if (c.kind == UiSettingsControlKind::buttons) {
        for (u32 i = 0; i < c.choices.count; ++i) {
            const float w = ui_settings_button_width(t, UiSettingsButtonSize::compact, button_label_width(c.choices[i]));
            if (i == index)
                return {x, field.top, minimum(field.right, x + w), field.bottom};
            x += w + kButtonGroupGap;
        }
        return {};
    }
    if (c.kind == UiSettingsControlKind::colors) {
        x += index * (kColorSwatchWidth + kColorSwatchGap);
        if (index < c.choices.count)
            return {x, field.top, x + kColorSwatchWidth, field.top + kColorSwatchHeight};
        if (index == c.choices.count && c.action_label.bytes) {
            const float w =
                ui_settings_button_width(t, UiSettingsButtonSize::compact, button_label_width(c.action_label));
            const float top = field.top + (kColorSwatchHeight - t.metric.button_small) * .5f;
            return {x - kColorSwatchGap + kButtonGroupGap, top, x - kColorSwatchGap + kButtonGroupGap + w,
                    top + t.metric.button_small};
        }
    }
    return {};
}
u32 option_hit(const UiSettingsLayoutTokens& t, const UiSettingsControl& c, const UiRect& field, float x, float y) noexcept {
    const u32 count = c.choices.count + (c.kind == UiSettingsControlKind::colors && c.action_label.bytes ? 1u : 0u);
    for (u32 i = 0; i < count; ++i)
        if (ui_rect_contains(option_rect(t, c, field, i), x, y))
            return i;
    return ~0u;
}
} // namespace
bool ui_settings_scene_option_pointer(const UiSettingsScene& scene, const UiSettingsTarget& target, float x, float y,
                                      UiSettingsActivation* output) noexcept {
    u32 index = 0;
    const auto* c = ui_settings_scene_control(scene, target, &index);
    UiSettingsControlGeometry geometry{};
    if (!output || !ui_coordinate_valid(x) || !ui_coordinate_valid(y) || !c || !c->enabled)
        return false;
    if ((list_item(scene.content, index) && c->kind == UiSettingsControlKind::action) ||
        (c->kind == UiSettingsControlKind::edit && c->action_label.bytes)) {
        // List rows report their part; an edit row its button as choice 0.
        const SceneTokens t(scene);
        RowLayout row{};
        UiRect clip{};
        if (!row_layout_for(scene, t, target_handle(scene, target), index, &row, &clip) ||
            !ui_rect_contains(clip, x, y))
            return false;
        const u32 part = c->kind == UiSettingsControlKind::edit
                             ? (ui_rect_contains(row.value, x, y) ? 0u : ~0u)
                             : list_part(t, scene.content, index, row.row, x, y);
        if (part == ~0u)
            return false;
        return report(target, *c, part, output);
    }
    if (!option_kind(c->kind) || !ui_settings_scene_control_geometry(scene, target, &geometry) ||
        !ui_rect_contains(geometry.clip, x, y))
        return false;
    const SceneTokens t(scene);
    const bool tristate = has_flag(scene.content, index, ui_settings_presentation_tristate);
    const u32 choice = c->kind == UiSettingsControlKind::segments
                           ? (tristate ? ui_tristate_hit(t, geometry.field, x, y)
                                       : ui_segments_hit(t, geometry.field, c->choices.count, x, y))
                       : c->kind == UiSettingsControlKind::swatches
                           ? ui_color_grid_hit(t, geometry.field, c->choices.count, x, y)
                           : option_hit(t, *c, geometry.field, x, y);
    if (choice == ~0u)
        return false;
    return report(target, *c, choice, output);
}
bool ui_settings_scene_choice_rectangle(const UiSettingsScene& scene, const UiSettingsTarget& target, u32 choice,
                                        UiRect* output) noexcept {
    u32 index = 0;
    const auto* c = ui_settings_scene_control(scene, target, &index);
    UiSettingsControlGeometry geometry{};
    if (output && c && list_style(scene.content, index, UiSettingsListStyle::row)) {
        const SceneTokens t(scene);
        RowLayout row{};
        if (!row_layout_for(scene, t, target_handle(scene, target), index, &row))
            return false;
        const auto parts = list_parts(t, scene.content, index, row.row);
        *output = choice == ui_settings_list_row     ? row.row
                  : choice == ui_settings_list_count ? parts.column
                  : choice - ui_settings_list_action < kUiSettingsListActions
                      ? parts.actions[choice - ui_settings_list_action]
                      : UiRect{};
        return ui_rect_has_area(*output);
    }
    if (!output || !c || choice >= c->choices.count ||
        (c->kind != UiSettingsControlKind::radio && c->kind != UiSettingsControlKind::segments &&
         c->kind != UiSettingsControlKind::tabs && c->kind != UiSettingsControlKind::buttons &&
         c->kind != UiSettingsControlKind::colors) ||
        !ui_settings_scene_control_geometry(scene, target, &geometry))
        return false;
    const SceneTokens t(scene);
    const bool tristate = has_flag(scene.content, index, ui_settings_presentation_tristate);
    *output = c->kind == UiSettingsControlKind::radio ? ui_radio_option(t, geometry.field, choice)
              : c->kind == UiSettingsControlKind::segments
                  ? (tristate ? ui_tristate_option(t, geometry.field, choice)
                              : ui_segments_option(t, geometry.field, c->choices.count, choice))
                  : option_rect(t, *c, geometry.field, choice);
    return ui_rect_has_area(*output);
}
bool ui_settings_scene_grip_hit(const UiSettingsScene& scene, const UiSettingsTarget& target, float x, float y) noexcept {
    u32 index = 0;
    const auto* c = ui_settings_scene_control(scene, target, &index);
    if (!c || !c->enabled || !has_flag(scene.content, index, ui_settings_presentation_grip))
        return false;
    const SceneTokens t(scene);
    RowLayout row{};
    UiRect clip{};
    if (!row_layout_for(scene, t, target_handle(scene, target), index, &row, &clip))
        return false;
    const UiRect grip{row.lead.left - 6, row.row.top, row.lead.left + t.metric.grip_width + 4, row.row.bottom};
    return ui_rect_contains(ui_rect_intersection(grip, clip), x, y);
}
bool ui_settings_scene_drop_target(const UiSettingsScene& scene, float y, u64* control_id, bool* after) noexcept {
    if (!control_id || !after || !current(scene) || !ui_coordinate_valid(y))
        return false;
    const SceneTokens t(scene);
    float best = kMaximumUiCoordinate;
    bool found = false;
    for (u32 i = 0; i < scene.widgets.count; ++i) {
        const auto& entry = entries(scene)[i];
        if (entry.role != EntryRole::row || entry.source_index >= scene.content.controls.count)
            continue;
        RowLayout row{};
        UiRect clip{};
        if (!row_layout_for(scene, t, {static_cast<u16>(i), scene.widgets.generation}, entry.source_index,
                            &row, &clip) ||
            !ui_rect_intersects(row.row, clip))
            continue;
        const float middle = (row.row.top + row.row.bottom) * .5f;
        const float distance = y < row.row.top ? row.row.top - y : y > row.row.bottom ? y - row.row.bottom : 0;
        if (distance < best) {
            best = distance;
            *control_id = scene.content.controls[entry.source_index].id;
            *after = y >= middle;
            found = true;
        }
    }
    return found;
}
bool ui_settings_scene_set_drag(UiSettingsScene* scene, u64 dragged, u64 drop_control, bool after) noexcept {
    if (!scene)
        return false;
    const auto row = [&](u64 id) noexcept -> u16 {
        for (u32 i = 0; id && i < scene->content.controls.count && i < 0xfffe; ++i)
            if (scene->content.controls[i].id == id)
                return static_cast<u16>(i + 1);
        return 0;
    };
    const u16 drag = row(dragged), drop = row(drop_control);
    after = after && drop;
    if (scene->drag_row == drag && scene->drop_row == drop && scene->drop_after == after)
        return false;
    scene->drag_row = drag;
    scene->drop_row = drop;
    scene->drop_after = after;
    ++scene->motion_revision;
    return true;
}
bool ui_settings_scene_set_footer_motion(UiSettingsScene* scene, float shift, float alert) noexcept {
    if (!scene || !ui_coordinate_valid(shift))
        return false;
    shift = maximum(-64.f, minimum(shift, 64.f));
    alert = alert > 0 ? minimum(alert, 1.f) : 0.f;
    const i16 shift_q = static_cast<i16>(shift * 16.f);
    const u8 alert_q = static_cast<u8>(alert * 255.f + .5f);
    if (scene->footer_shift_q == shift_q && scene->footer_alert_q == alert_q)
        return false;
    scene->footer_shift_q = shift_q;
    scene->footer_alert_q = alert_q;
    // Footer text moves with the bar, so text is rebuilt as well as paint.
    ++scene->motion_revision;
    ++scene->revision;
    return true;
}
bool ui_settings_scene_palette_hover(UiSettingsScene* scene, const UiSettingsTarget& target, float x, float y) noexcept {
    if (!scene)
        return false;
    UiSettingsActivation activation{};
    const u32 index = ui_settings_scene_palette_pointer(*scene, target, x, y, &activation) ||
                              ui_settings_scene_radio_pointer(*scene, target, x, y, &activation) ||
                              ui_settings_scene_option_pointer(*scene, target, x, y, &activation)
                          ? activation.selected_choice
                          : ~u32{0};
    if (scene->palette_hovered_index == index)
        return false;
    scene->palette_hovered_index = index;
    ++scene->revision;
    return true;
}
bool ui_settings_scene_aside(const UiSettingsScene& scene, UiRect* heading, UiRect* card, UiRect* clip,
                             bool* beside) noexcept {
    if (!current(scene))
        return false;
    const auto aside = aside_layout(scene);
    if (!aside.active)
        return false;
    const SceneTokens t(scene);
    const auto f = frame(scene, t.metric);
    const auto& content = scene.content;
    const float label = content.aside_label.bytes ? kUiSettingsLabelLine + kAsideHeadingGap : 0;
    float left = scene.layout.column.left, top = 0;
    if (aside.beside) {
        // Sticky 24 DIP below the scroller's top while its rows scroll by.
        left = scene.layout.column.left + body_width(scene) - aside.width;
        const float start = f.body.top + row_start(scene, content.aside_row) - scene.content_offset;
        const float end = f.body.top + content_extent(scene, t.metric) - t.metric.panel_padding_bottom -
                          scene.content_offset - aside.block;
        top = maximum(start, minimum(f.body.top + kSectionGap, end));
    } else
        top = f.body.top + row_start(scene, content.aside_row) - aside.block - kSectionGap - scene.content_offset;
    const UiRect head{left, top, left + aside.width, top + (label ? kUiSettingsLabelLine : 0)};
    const UiRect body{left, top + label, left + aside.width, top + label + content.aside_height};
    const UiRect visible = ui_rect_overlap(f.body, scene.layout.panel);
    if (!ui_rect_has_area(ui_rect_overlap({left, top, left + aside.width, top + aside.block}, visible)))
        return false;
    if (heading)
        *heading = head;
    if (card)
        *card = body;
    if (clip)
        *clip = visible;
    if (beside)
        *beside = aside.beside;
    return true;
}
bool ui_settings_scene_page_owner(const UiSettingsScene& scene, UiWidgetReference owner) noexcept {
    if (!current(scene) || owner.manager != &scene.widgets || !ui_widget_manager_resolve(scene.widgets, owner.handle))
        return false;
    if (ui_widget_handle_equal(owner.handle, scene.title) || ui_widget_handle_equal(owner.handle, scene.description))
        return true;
    const auto& entry = entries(scene)[owner.handle.slot];
    return entry.role == EntryRole::row || entry.role == EntryRole::footer_status ||
           entry.target.kind == UiSettingsTargetKind::save || entry.target.kind == UiSettingsTargetKind::reset;
}
bool ui_settings_scene_spinner_visible(const UiSettingsScene& scene) noexcept {
    if (!scene.active)
        return false;
    for (u32 i = 0; i < scene.content.controls.count; ++i) {
        const auto& c = scene.content.controls[i];
        if (c.kind == UiSettingsControlKind::notice && c.checked)
            return true;
    }
    return false;
}

bool ui_settings_scene_next_avatar(const UiSettingsScene& scene, u32* cursor,
                                   UiSettingsAvatarGeometry* output) noexcept {
    if (!cursor || !output || !current(scene) || !scene.content.avatar_media)
        return false;
    const SceneTokens t(scene);
    for (; *cursor < scene.widgets.count; ++*cursor) {
        const auto& entry = entries(scene)[*cursor];
        const u32 index = entry.source_index;
        if (entry.role != EntryRole::row || index >= scene.content.controls.count ||
            !avatar_backed(scene.content, index))
            continue;
        const auto& media = scene.content.avatar_media[index];
        const UiWidgetHandle handle{static_cast<u16>(*cursor), scene.widgets.generation};
        RowLayout r{};
        UiRect clip{};
        if (!row_layout_for(scene, t, handle, index, &r, &clip) || !ui_rect_has_area(r.lead))
            continue;
        // Same circle as paint_row's loading placeholder.
        UiRect lead = r.lead;
        if (has_flag(scene.content, index, ui_settings_presentation_grip))
            lead.left += t.metric.grip_width + 4;
        const float size = t.metric.avatar_size, cy = (lead.top + lead.bottom) * .5f;
        const UiRect avatar{lead.left, cy - size * .5f, lead.left + size, cy + size * .5f};
        const UiRect visible = ui_rect_overlap(avatar, clip);
        if (!ui_rect_has_area(visible))
            continue;
        *output = {&media, {&scene.widgets, handle}, avatar, visible};
        ++*cursor;
        return true;
    }
    return false;
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
    bool ok = true;
    if (item.style == UiSettingsListStyle::card) {
        const auto box = parts.content;
        const float radius = t.metric.control_radius;
        const float tile_radius = maximum(radius - t.metric.notice_padding, theme.scale.radius_sm);
        ok = paint_stream_push_rounded_border(paint, box,
                                              state.hovered && c.enabled ? tc.mod_normal : tc.mod_subtle,
                                              tc.border_subtle, radius, 1) &&
             paint_stream_push_rounded(paint, parts.icon,
                                       tc.mod_strong, tile_radius);
        if (ok && item.lead != UiIcon::count) {
            const float inset = (ui_rect_width(parts.icon) - t.metric.list_icon) * .5f;
            ok = ui_icon_paint(paint, item.lead,
                               {parts.icon.left + inset, parts.icon.top + inset, parts.icon.right - inset,
                                parts.icon.bottom - inset},
                               ui_settings_color_alpha(tc.text_strong, alpha));
        }
        if (ok)
            ok = ui_icon_paint(paint, UiIcon::chevron_right, parts.chevron,
                               ui_settings_color_alpha(state.hovered ? tc.text_strong : tc.text_subtle, alpha));
        const float ring = t.metric.focus_gap + t.metric.focus_width;
        if (ok && state.focused)
            ok = ui_settings_focus_paint(paint, t, {box.left + ring, box.top + ring, box.right - ring,
                                                    box.bottom - ring},
                                         maximum(0.f, radius - ring));
        return ok;
    }
    if (!highlighted && state.hovered && c.enabled)
        ok = paint_stream_push_rounded(paint, r.row.left, r.row.top + theme.scale.space_1, r.row.right,
                                       r.row.bottom - theme.scale.space_1, ui_settings_color_alpha(tc.mod_subtle, .7f),
                                       t.metric.control_radius);
    // Draggable rows always show their handle; it brightens on hover.
    if (ok && has_flag(scene.content, index, ui_settings_presentation_grip))
        ok = ui_admin_grip_paint(paint, t, {parts.content.left, r.row.top, parts.icon.left, r.row.bottom},
                                 state.hovered, c.enabled);
    if (ok && item.lead != UiIcon::count) {
        // The lead glyph wears the row accent (a role's colour) as ink and a
        // light underlay.
        Color ink = tc.text_subtle;
        if (accent(scene.content, index))
            resolve_color(scene, c.id, kUiSettingsAccentChoice, &ink);
        ok = ui_icon_paint_styled(paint, item.lead, parts.icon,
                                  {ui_settings_color_alpha(ink, alpha), ui_settings_color_alpha(ink, .35f * alpha), 0});
    }
    if (ok && item.locked)
        ok = ui_icon_paint(paint, UiIcon::lock, parts.lock, ui_settings_color_alpha(tc.text_subtle, alpha));
    if (ok && item.count_icon != UiIcon::count)
        ok = ui_icon_paint(paint, item.count_icon, list_count_icon(t, c, parts.column),
                           ui_settings_color_alpha(hovered_part == ui_settings_list_count ? tc.text_strong
                                                                                          : tc.text_subtle,
                                                   alpha));
    // Icon buttons appear with the pointer or keyboard focus on the row.
    for (u32 i = 0; ok && (state.hovered || state.focused) && i < kUiSettingsListActions; ++i) {
        if (item.actions[i] == UiIcon::count)
            continue;
        const auto b = parts.actions[i];
        const bool hot = hovered_part == ui_settings_list_action + i;
        ok = paint_stream_push_rounded(paint, b,
                                       hot ? (state.pressed ? tc.mod_strong : tc.mod_normal) : tc.mod_subtle,
                                       t.metric.control_radius);
        const float inset = (ui_rect_width(b) - t.metric.list_icon * .75f) * .5f;
        if (ok)
            ok = ui_icon_paint(paint, item.actions[i], {b.left + inset, b.top + inset, b.right - inset, b.bottom - inset},
                               ui_settings_color_alpha(hot ? tc.text_strong : tc.text_default, alpha));
    }
    // The ring stays inside the row so the list's clip keeps all of it.
    const float ring = t.metric.focus_gap + t.metric.focus_width;
    if (ok && state.focused)
        ok = ui_settings_focus_paint(paint, t, {r.row.left + ring, r.row.top + ring, r.row.right - ring,
                                                r.row.bottom - ring},
                                     t.metric.control_radius);
    // Rows of one list are divided by hairlines inside their content edges.
    if (ok && list_style(scene.content, index + 1, UiSettingsListStyle::row))
        ok = paint_stream_push(paint, parts.content.left, r.row.bottom - 1, parts.content.right, r.row.bottom,
                               tc.border_subtle);
    return ok;
}

bool paint_row(const UiSettingsScene& scene, const Theme& theme, const UiSettingsTokens& t, PaintStream* paint,
               UiWidgetHandle handle, const Entry& entry, u32 dpi) noexcept {
    const auto& c = scene.content.controls[entry.source_index];
    RowLayout r{};
    UiRect clip{};
    if (!row_layout_for(scene, t, handle, entry.source_index, &r, &clip) ||
        !ui_rect_has_area(ui_rect_overlap(r.row, clip)))
        return true;
    const auto& tc = t.color;
    auto state = widget_state(scene, handle, entry.target, c.enabled);
    bool ok = true;
    // The flash covers the same box as the selection fill.
    if (scene.flash_control == c.id && scene.flash > 0)
        ok = paint_stream_push_rounded(paint, r.row.left, r.row.top + theme.scale.space_1, r.row.right,
                                       r.row.bottom - theme.scale.space_1,
                                       ui_settings_color_alpha(tc.brand, .16f * scene.flash), t.metric.control_radius);
    const u32 index = entry.source_index;
    const bool dragged = scene.drag_row == index + 1;
    if (ok && (has_flag(scene.content, index, ui_settings_presentation_selected) || dragged))
        ok = paint_stream_push_rounded(paint, r.row.left, r.row.top + 2, r.row.right, r.row.bottom - 2,
                                       dragged ? ui_settings_color_alpha(tc.brand, .14f) : tc.mod_subtle,
                                       t.metric.control_radius);
    // Only list rows that are themselves the action get a hover fill; a row
    // with its own button highlights just that button.
    else if (ok && c.kind == UiSettingsControlKind::action && has_flag(scene.content, index, ui_settings_presentation_compact) &&
             !c.action_label.bytes && state.hovered && c.enabled && !list_item(scene.content, index))
        ok = paint_stream_push_rounded(paint, r.row.left, r.row.top + 2, r.row.right, r.row.bottom - 2,
                                       ui_settings_color_alpha(tc.mod_subtle, .7f), t.metric.control_radius);
    if (ok && scene.drop_row == index + 1 && !dragged) {
        const float y = scene.drop_after ? r.row.bottom - 1 : r.row.top + 1;
        ok = paint_stream_push_rounded(paint, r.row.left, y - 1.5f, r.row.right, y + 1.5f, tc.brand, 1.5f);
    }
    if (const auto* item = c.kind == UiSettingsControlKind::action ? list_item(scene.content, index) : nullptr)
        return ok && paint_list_row(scene, theme, t, paint, r, index, *item, state,
                                    dragged || has_flag(scene.content, index, ui_settings_presentation_selected));
    if (ok && c.kind == UiSettingsControlKind::action && ui_rect_has_area(r.lead)) {
        const float alpha = c.enabled ? 1.f : .5f;
        UiRect lead = r.lead;
        if (has_flag(scene.content, index, ui_settings_presentation_grip)) {
            ok = ui_admin_grip_paint(paint, t, lead, state.hovered, c.enabled);
            lead.left += t.metric.grip_width + 4;
        }
        Color fill = tc.brand;
        if (accent(scene.content, index))
            resolve_color(scene, c.id, kUiSettingsAccentChoice, &fill);
        if (ok && has_flag(scene.content, index, ui_settings_presentation_avatar)) {
            const float size = t.metric.avatar_size, cy = (lead.top + lead.bottom) * .5f;
            const UiRect avatar{lead.left, cy - size * .5f, lead.left + size, cy + size * .5f};
            // The renderer hides this placeholder under the loaded image.
            ok = avatar_backed(scene.content, index)
                     ? paint_stream_push_rounded(paint, avatar,
                                                 theme.avatar_placeholder, size * .5f)
                     : ui_admin_avatar_paint(paint, t, avatar, fill, alpha);
        } else if (ok && accent(scene.content, index)) {
            // The reserved lead always shows its dot: host colour, else brand.
            // Keep the dot on the label line when a description follows.
            const UiRect line{lead.left, r.label.top, lead.right, r.label.bottom};
            ok = ui_admin_dot_paint(paint, t, line, fill, alpha);
        }
    }
    if (ok && ui_rect_has_area(r.chips)) {
        // Accent rows (role lists) lead each chip with its colour dot, a
        // neutral one for colourless roles; plain chips (keybinds) have none.
        const bool dots = accent(scene.content, index);
        for (u32 i = 0; ok && i < c.choices.count; ++i) {
            const auto chip = ui_admin_chip_rect(t, c.choices, dots, r.chips, i);
            Color dot = tc.text_subtle;
            const bool has_dot = ui_admin_chip_dot(c.choices[i], dots);
            if (has_dot)
                (void)resolve_color(scene, c.id, i, &dot);
            if (ui_rect_has_area(chip))
                ok = ui_admin_chip_paint(paint, t, chip, dot, has_dot, c.enabled ? 1.f : .5f);
        }
    }
    if (ok && progress(scene.content, index) && ui_rect_has_area(r.value)) {
        const float level = static_cast<float>(minimum<u16>(scene.content.meters[index], 1000)) / 1000.f;
        const float radius = ui_rect_height(r.value) * .5f;
        ok = paint_stream_push_rounded(paint, r.value, tc.slider_track,
                                       radius);
        if (ok && level > 0)
            ok = paint_stream_push_rounded(paint, r.value.left, r.value.top,
                                           maximum(r.value.left + radius * 2, r.value.left + ui_rect_width(r.value) * level),
                                           r.value.bottom, tc.brand, radius);
    }
    // Rows inside one category are separated by hairline dividers; sections
    // end with a divider halfway through their trailing space.
    const u32 next = entry.source_index + 1;
    if (ok && !scene.content.sectioned && field_row(c.kind) && next < scene.content.controls.count &&
        field_row(scene.content.controls[next].kind))
        ok = paint_stream_push(paint, r.row.left, r.row.bottom - 1, r.row.right, r.row.bottom, tc.border_subtle);
    if (ok && scene.content.sectioned && c.kind != UiSettingsControlKind::tabs &&
        section_trailing(scene.content, entry.source_index) > 0) {
        const float y = r.row.bottom - kSectionGap - 1;
        ok = paint_stream_push(paint, r.row.left, y, r.row.right, y + 1, tc.border_subtle);
    }
    if (!ok)
        return false;
    switch (c.kind) {
    case UiSettingsControlKind::toggle: {
        const float motion = toggle_motion(scene, entry.target, t, c.checked);
        return ui_switch_paint(paint, t, r.control, c.checked, motion, scene.reduced_motion, state);
    }
    case UiSettingsControlKind::segments:
        if (has_flag(scene.content, index, ui_settings_presentation_tristate))
            return ui_tristate_paint(paint, t, r.control, c.selected_choice,
                                     state.hovered ? scene.palette_hovered_index : ~0u, state);
        return ui_segments_paint(paint, t, r.control, c.choices.count, c.selected_choice,
                                 state.hovered ? scene.palette_hovered_index : ~0u, state);
    case UiSettingsControlKind::swatches:
        for (u32 i = 0; i < c.choices.count; ++i) {
            UiControlState option = hover_option(scene, state, i);
            option.focused = state.focused && (c.selected_choice == i || (c.selected_choice >= c.choices.count && !i));
            Color fill = tc.mod_strong;
            resolve_color(scene, c.id, i, &fill);
            if (!ui_color_grid_paint(paint, t, ui_color_grid_cell(t, r.control, i), fill, c.selected_choice == i,
                                     option))
                return false;
        }
        return true;
    case UiSettingsControlKind::tabs: {
        // The brand tab bar: a hairline under every tab and a 2 DIP
        // brand underline under the active one.
        ok = paint_stream_push(paint, r.control.left, r.control.bottom - 1, r.control.right, r.control.bottom,
                               tc.border_subtle);
        const auto tab = option_rect(t, c, r.control, c.selected_choice);
        if (ok && c.selected_choice < c.choices.count && ui_rect_has_area(tab))
            ok = paint_stream_push_rounded(paint, tab.left, r.control.bottom - 2, tab.right, r.control.bottom,
                                           tc.brand, theme.scale.radius_xs);
        if (ok && state.focused && scene.focus_visible && ui_rect_has_area(tab))
            ok = ui_settings_focus_paint(paint, t,
                                         {tab.left - theme.scale.space_2, tab.top, tab.right + theme.scale.space_2,
                                          tab.top + kUiSettingsLabelLine},
                                         theme.scale.radius_sm);
        return ok;
    }
    case UiSettingsControlKind::buttons:
        for (u32 i = 0; i < c.choices.count; ++i) {
            UiControlState option = hover_option(scene, state, i);
            option.focused = state.focused && i == (c.selected_choice < c.choices.count ? c.selected_choice : 0);
            if (!ui_settings_button_paint(paint, t, option_rect(t, c, r.control, i),
                                          i ? UiSettingsButtonVariant::secondary : UiSettingsButtonVariant::primary,
                                          option))
                return false;
        }
        return true;
    case UiSettingsControlKind::colors: {
        // Profile theme swatches: the colour with a matching
        // border and a pencil at the top end, its caption below.
        for (u32 i = 0; ok && i < c.choices.count; ++i) {
            const auto swatch = option_rect(t, c, r.control, i);
            Color fill = tc.mod_strong;
            resolve_color(scene, c.id, i, &fill);
            if (!c.enabled)
                fill.alpha *= .3f;
            const bool focused = state.focused && scene.focus_visible &&
                                 i == (c.selected_choice < c.choices.count ? c.selected_choice : 0);
            ok = paint_stream_push_rounded_border(paint, swatch, fill,
                                                  focused ? tc.focus_ring : ui_settings_color_alpha(fill, 1),
                                                  theme.scale.radius_sm, focused ? 2.f : 1.f);
            const float icon = 14;
            ok = ok && ui_icon_paint(paint, UiIcon::edit,
                                     {swatch.right - 4 - icon, swatch.top + 4, swatch.right - 4, swatch.top + 4 + icon},
                                     ui_settings_color_alpha(ui_surface_foreground(theme, ui_settings_color_alpha(fill, 1)),
                                                             c.enabled ? 1.f : .3f));
        }
        if (ok && c.action_label.bytes) {
            const UiControlState option = hover_option(scene, state, c.choices.count);
            ok = ui_settings_button_paint(paint, t, option_rect(t, c, r.control, c.choices.count),
                                          UiSettingsButtonVariant::secondary, option);
        }
        return ok;
    }
    case UiSettingsControlKind::action:
        if (!c.action_label.bytes)
            return true;
        return ui_settings_button_paint(paint, t, r.control,
                                        variant(scene.content, entry.source_index) == UiSettingsVariant::critical
                                            ? UiSettingsButtonVariant::critical
                                        : variant(scene.content, entry.source_index) == UiSettingsVariant::primary
                                            ? UiSettingsButtonVariant::primary
                                            : UiSettingsButtonVariant::secondary,
                                        state);
    case UiSettingsControlKind::choice:
        return ui_select_paint(paint, t, r.control, false, state, field_error(scene.content, entry.source_index));
    case UiSettingsControlKind::edit:
    case UiSettingsControlKind::multiline: {
        auto view = c.editor->view;
        view.active = true;
        view.focused = c.enabled && view.focused && ui_settings_target_equal(scene.focus, entry.target);
        view.caret_visible = view.caret_visible && view.focused;
        UiSettingsControlGeometry geometry{};
        if (!ui_settings_scene_control_geometry(scene, entry.target, &geometry))
            return true;
        if (ui_rect_has_area(r.value)) {
            UiControlState button = hover_option(scene, state, 0);
            button.focused = false;
            if (!ui_settings_button_paint(paint, t, r.value, UiSettingsButtonVariant::primary, button))
                return false;
        }
        return ui_input_shell_paint(paint, t, r.control, {c.enabled, state.hovered, false, view.focused},
                                    field_error(scene.content, entry.source_index)) &&
               ui_edit_field_paint_content(paint, ui_rect_overlap(geometry.text, geometry.clip), view,
                                           c.editor->geometry, theme, dpi);
    }
    case UiSettingsControlKind::slider: {
        const auto g = ui_slider_geometry(t, r.control, c.choices.count, c.selected_choice,
                                          marks(scene.content, entry.source_index));
        if (scene.content.meters && scene.content.meters[entry.source_index] != kUiSettingsNoMeter)
            return ui_slider_meter_paint(paint, t, g, c.choices.count, scene.content.meters[entry.source_index],
                                         state);
        return ui_slider_paint(paint, t, g, c.choices.count, default_choice(scene.content, entry.source_index), state);
    }
    case UiSettingsControlKind::radio:
        for (u32 i = 0; i < c.choices.count; ++i) {
            const auto option = ui_radio_option(t, r.control, i);
            UiControlState option_state = hover_option(scene, state, i);
            option_state.focused = state.focused && (c.selected_choice == i ||
                                                     (c.selected_choice >= c.choices.count && i == 0));
            if (!ui_radio_paint(paint, t, ui_radio_indicator(t, option), c.selected_choice == i, option_state))
                return false;
        }
        return true;
    case UiSettingsControlKind::palette: {
        const u32 columns = ui_swatch_columns(t, ui_rect_width(r.control));
        const auto palette = scene.palette;
        for (u32 i = 0; i < c.choices.count; ++i) {
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
            Color fill = theme.base_lowest;
            if (resolved && background_theme_valid(colors) && colors.color_count >= 2 &&
                colors.colors[0] == colors.colors[1])
                fill = color_from_rgb24(colors.colors[0]);
            const bool gradient = resolved && background_theme_valid(colors) && colors.colors[0] != colors.colors[1];
            if (!ui_swatch_paint(paint, t, swatch, fill, gradient ? &colors : nullptr, c.selected_choice == i,
                                 option_state))
                return false;
        }
        return true;
    }
    case UiSettingsControlKind::notice: {
        const auto tone = variant(scene.content, entry.source_index);
        const Color fill = tone == UiSettingsVariant::critical   ? tc.critical_tint
                           : tone == UiSettingsVariant::positive ? ui_settings_color_alpha(tc.positive, .12f)
                                                                 : tc.mod_subtle;
        const UiRect box{r.row.left, r.row.top + t.metric.row_padding * .5f, r.row.right,
                         r.row.bottom - t.metric.row_padding * .5f};
        // Critical notices outline the tinted box, sharing its corners.
        ok = tone == UiSettingsVariant::critical
                 ? paint_stream_push_rounded_border(paint, box, fill,
                                                    ui_settings_color_alpha(tc.critical_text, .5f),
                                                    t.metric.control_radius, 1)
                 : paint_stream_push_rounded(paint, box, fill,
                                             t.metric.control_radius);
        if (ok && c.action_label.bytes)
            ok = ui_settings_button_paint(paint, t, r.control, UiSettingsButtonVariant::secondary, state);
        if (ok && c.checked) {
            const float size = ui_rect_height(box) - theme.scale.space_4 * 2;
            const float side = minimum(size, 20.f), cy = (box.top + box.bottom) * .5f;
            const float right = box.right - theme.scale.space_4;
            ok = ui_spinner_paint(paint, {right - side, cy - side * .5f, right, cy + side * .5f}, tc.text_subtle,
                                  scene.spinner_phase);
        }
        return ok;
    }
    case UiSettingsControlKind::category:
        if (link(scene.content, entry.source_index)) {
            const float icon = 18, cy = (r.label.top + r.label.bottom) * .5f;
            ok = ui_icon_paint(paint, UiIcon::chevron_right,
                               {r.row.right - icon, cy - icon * .5f, r.row.right, cy + icon * .5f},
                               state.hovered ? tc.text_strong : tc.text_subtle);
            const float ring = theme.scale.space_2;
            if (ok && state.focused)
                ok = paint_stream_push_rounded_border(paint, r.row.left - ring, r.label.top - ring, r.row.right + ring,
                                                      r.label.bottom + ring, {}, tc.focus_ring, theme.scale.radius_sm,
                                                      2);
        }
        return ok;
    case UiSettingsControlKind::error:
        break;
    }
    return true;
}
bool paint_nav(const UiSettingsScene& scene, const UiSettingsTokens& t, PaintStream* paint) noexcept {
    const auto* nav = ui_widget_manager_layout_node(scene.widgets, scene.sidebar);
    if (!nav)
        return false;
    const auto clip = ui_rect_overlap(nav->rectangle, nav->clip_rectangle);
    if (!paint_stream_push_clip(paint, clip.left, clip.top, clip.right, clip.bottom))
        return false;
    bool ok = true;
    for (u32 i = 0; ok && i < scene.widgets.count; ++i) {
        const auto& entry = entries(scene)[i];
        if (entry.role != EntryRole::nav)
            continue;
        const UiWidgetHandle handle{static_cast<u16>(i), scene.widgets.generation};
        const auto* node = ui_widget_manager_layout_node(scene.widgets, handle);
        NavRow row{};
        if (!node || !nav_row(scene, entry.source_index, &row) ||
            !ui_rect_has_area(ui_rect_overlap(node->rectangle, node->clip_rectangle)))
            continue;
        if (row.kind == UiSettingsNavKind::separator) {
            ok = ui_settings_separator_paint(paint, t, node->rectangle);
            continue;
        }
        if (row.kind == UiSettingsNavKind::header)
            continue;
        const bool selected = row.kind == UiSettingsNavKind::page && ui_page_key_equal(row.target.page, scene.selected_page);
        const auto state = widget_state(scene, handle, row.target, true);
        ok = ui_settings_nav_item_paint(paint, t, node->rectangle, selected, row.destructive, state);
        if (ok && row.icon < UiIcon::count)
            ok = ui_icon_paint(paint, row.icon, ui_settings_nav_icon_rect(t, node->rectangle),
                               ui_settings_nav_item_text(t, selected, row.destructive, state));
    }
    return paint_stream_pop_clip(paint) && ok;
}
} // namespace

bool ui_settings_scene_paint(const UiSettingsScene& scene, const Theme& theme, PaintStream* paint, u32 dpi,
                             UiSettingsPaintRanges* ranges) noexcept {
    if (ranges)
        *ranges = {paint ? paint->count : 0, paint ? paint->count : 0, paint ? paint->count : 0,
                   paint ? paint->count : 0};
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
    if (!paint_stream_push_clip(paint, viewport.left, viewport.top, viewport.right, viewport.bottom))
        return false;
    bool ok = true;
    if (modal && !l.compact)
        ok = paint_stream_push(paint, viewport.left, viewport.top, viewport.right, viewport.bottom, tc.scrim);
    if (ranges)
        ranges->modal_first_quad = paint->count;
    const float radius = modal && !l.compact ? t.metric.modal_radius : 0;
    if (modal) {
        if (ok && !l.compact)
            ok = ui_settings_shadow_paint(paint, l.modal, radius, 12, .3f);
        ok = ok && ui_background_paint_surface(paint, l.modal, theme, tc.modal_background,
                                               !l.compact ? tc.border_subtle : Color{}, radius, !l.compact ? 1.f : 0.f,
                                               .9f, UiBackgroundSurfaceMode::gradient, UiBackgroundTier::higher,
                                               UiBackgroundBorderTone::palette);
        // The sidebar keeps the modal's rounded left corners and a square inner edge.
        ok = ok && (dialog ||
             (ui_background_paint_surface(paint, l.sidebar, theme, tc.sidebar_background, {}, radius, 0, .9f,
                                         UiBackgroundSurfaceMode::gradient, UiBackgroundTier::high) &&
             (radius <= 0 ||
              ui_background_paint_surface(paint, {l.sidebar.right - radius, l.sidebar.top, l.sidebar.right,
                                                  l.sidebar.bottom},
                                          theme, tc.sidebar_background, {}, 0, 0, .9f, UiBackgroundSurfaceMode::gradient,
                                          UiBackgroundTier::high))));
    } else {
        // Full-window layers sit on the app canvas with a lifted sidebar region.
        ok = ok &&
             ui_background_paint_surface(paint, viewport, theme, theme.base_lowest, {}, 0, 0, 1,
                                         UiBackgroundSurfaceMode::gradient, UiBackgroundTier::chat) &&
             ui_background_paint_surface(paint, l.sidebar, theme, theme.base_lower, {}, 0, 0, 1,
                                         UiBackgroundSurfaceMode::gradient, UiBackgroundTier::app_frame);
    }
    if (ok && scene.search_field.generation) {
        const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.search_field);
        if (node) {
            const UiSettingsTarget target{UiSettingsTargetKind::search, {}, 0};
            auto state = widget_state(scene, scene.search_field, target, true);
            state.focused = scene.search.view.focused || state.focused;
            ok = ui_settings_search_paint(paint, t, node->rectangle, scene.search.view.text.bytes != 0, state);
            if (ok && scene.search.view.focused) {
                UiSettingsControlGeometry geometry{};
                if (ui_settings_scene_search_geometry(scene, &geometry)) {
                    auto view = scene.search.view;
                    view.active = true;
                    ok = ui_edit_field_paint(paint, node->rectangle, geometry.text, view, scene.search.geometry,
                                             {0, 0, 0, 0}, theme, dpi, t.metric.control_radius);
                }
            }
        }
    }
    ok = ok && paint_nav(scene, t, paint);
    if (ok) {
        const UiSettingsTarget target{UiSettingsTargetKind::close, {}, 0};
        const auto state = widget_state(scene, scene.close, target, true);
        ok = dialog ? ui_settings_dialog_close_paint(paint, t, l.close, state)
                    : ui_settings_close_paint(paint, t, l.close, state);
    }
    if (ranges)
        ranges->page_first_quad = paint->count;
    const auto* body = ui_widget_manager_layout_node(scene.widgets, scene.body);
    if (ok && body) {
        const auto clip = ui_rect_overlap(body->rectangle, body->clip_rectangle);
        ok = paint_stream_push_clip(paint, clip.left, clip.top, clip.right, clip.bottom);
        for (u32 i = 0; ok && i < scene.widgets.count; ++i) {
            const auto& entry = entries(scene)[i];
            if (entry.role != EntryRole::row || entry.source_index >= scene.content.controls.count)
                continue;
            ok = paint_row(scene, theme, t, paint, {static_cast<u16>(i), scene.widgets.generation}, entry,
                           dpi);
        }
        ok = paint_stream_pop_clip(paint) && ok;
    }
    if (ok && scene.footer.visible) {
        const auto* status = ui_widget_manager_layout_node(scene.widgets, scene.footer_status);
        const auto* footer =
            status ? ui_widget_manager_layout_node(scene.widgets, scene.widgets.storage[scene.footer_status.slot].parent)
                   : nullptr;
        if (footer) {
            const float dx = scene.footer_shift_q / 16.f, alert = scene.footer_alert_q / 255.f;
            const UiRect f{footer->rectangle.left + dx, footer->rectangle.top, footer->rectangle.right + dx,
                           footer->rectangle.bottom};
            if (dialog) {
                // The band takes the modal's lower corners; its top edge stays square.
                const UiRect band = footer->rectangle;
                const Color fill = ui_settings_color_mix(tc.sidebar_background, tc.critical, alert * .5f);
                ok = ui_background_paint_surface(paint, band, theme, fill, {}, radius, 0, .9f,
                                                 UiBackgroundSurfaceMode::gradient, UiBackgroundTier::high) &&
                     (radius <= 0 ||
                      ui_background_paint_surface(paint, {band.left, band.top, band.right, band.top + radius}, theme,
                                                  fill, {}, 0, 0, .9f, UiBackgroundSurfaceMode::gradient,
                                                  UiBackgroundTier::high));
            } else {
                ok = ui_settings_shadow_paint(paint, f, t.metric.control_radius, 6, .25f) &&
                     paint_stream_push_rounded_border(
                         paint, f.left, f.top, f.right, f.bottom,
                         ui_settings_color_mix(theme.base_lowest, tc.critical, alert),
                         scene.footer.failed || alert > 0 ? tc.critical_text : tc.border_subtle,
                         t.metric.control_radius, 1);
            }
            const UiSettingsTarget reset{UiSettingsTargetKind::reset, scene.selected_page, 0};
            const UiSettingsTarget save{UiSettingsTargetKind::save, scene.selected_page, 0};
            const auto* reset_node = ui_widget_manager_layout_node(scene.widgets, scene.footer_reset);
            const auto* save_node = ui_widget_manager_layout_node(scene.widgets, scene.footer_save);
            const auto moved = [dx](UiRect r) noexcept { return UiRect{r.left + dx, r.top, r.right + dx, r.bottom}; };
            if (ok && reset_node) {
                auto state = widget_state(scene, scene.footer_reset, reset, target_exists(scene, reset));
                if (state.hovered || state.focused)
                    ok = ui_settings_button_paint(paint, t, moved(reset_node->rectangle),
                                                  UiSettingsButtonVariant::secondary, state);
            }
            if (ok && save_node)
                ok = ui_settings_button_paint(paint, t, moved(save_node->rectangle), UiSettingsButtonVariant::primary,
                                              widget_state(scene, scene.footer_save, save, target_exists(scene, save)));
        }
    }
    if (ranges)
        ranges->page_end_quad = paint->count;
    if (ok)
        ok = ui_scrollbar_paint(paint, ui_settings_scene_scrollbar(scene, true), theme) &&
             ui_scrollbar_paint(paint, ui_settings_scene_scrollbar(scene, false), theme);
    if (ranges)
        ranges->modal_end_quad = paint->count;
    return paint_stream_pop_clip(paint) && ok;
}

namespace {
bool row_text(const UiSettingsScene& scene, const UiSettingsTokens& t, UiTextOutput* stream, UiWidgetHandle handle,
              const Entry& entry) noexcept {
    const auto& c = scene.content.controls[entry.source_index];
    RowLayout r{};
    UiRect clip{};
    if (!row_layout_for(scene, t, handle, entry.source_index, &r, &clip) || !ui_rect_has_area(ui_rect_overlap(r.row, clip)))
        return true;
    const auto& tc = t.color;
    const auto state = widget_state(scene, handle, entry.target, c.enabled);
    const float dim = c.enabled ? 1.f : .5f;
    if (c.kind == UiSettingsControlKind::category) {
        const bool is_link = link(scene.content, entry.source_index);
        if (ui_rect_has_area(r.value) && c.choices.count &&
            !text(stream, scene, handle, c.choices[0], r.value, tc.text_subtle, text_eyebrow))
            return false;
        return text(stream, scene, handle, c.label, r.label,
                    is_link && state.hovered ? tc.text_strong : ui_settings_color_alpha(tc.text_strong, dim),
                    text_heading) &&
               describe(stream, scene, handle, c.description, r.description, tc.text_subtle);
    }
    if (c.kind == UiSettingsControlKind::error)
        return describe(stream, scene, handle, c.label, r.label, tc.critical_text);
    if (c.kind == UiSettingsControlKind::tabs) {
        for (u32 i = 0; i < c.choices.count; ++i) {
            const auto tab = option_rect(t, c, r.control, i);
            const bool hovered = state.hovered && scene.palette_hovered_index == i;
            if (!text(stream, scene, handle, c.choices[i], {tab.left, tab.top, tab.right, tab.top + kUiSettingsLabelLine},
                      ui_settings_color_alpha(c.selected_choice == i ? tc.text_strong
                                              : hovered               ? tc.text_default
                                                                      : tc.text_subtle,
                                              dim),
                      text_label, UiTextAlignment::center))
                return false;
        }
        return true;
    }
    if (list_style(scene.content, entry.source_index, UiSettingsListStyle::card))
        return text(stream, scene, handle, c.label, r.label, ui_settings_color_alpha(tc.text_strong, dim), text_heading) &&
               describe(stream, scene, handle, c.description, r.description, ui_settings_color_alpha(tc.text_subtle, dim));
    if (list_style(scene.content, entry.source_index, UiSettingsListStyle::row)) {
        const bool count_hovered = state.hovered && scene.palette_hovered_index == ui_settings_list_count;
        return text(stream, scene, handle, c.label, r.label, ui_settings_color_alpha(tc.text_strong, dim), text_label) &&
               (!c.choices.count ||
                text(stream, scene, handle, c.choices[0], r.value,
                     ui_settings_color_alpha(count_hovered ? tc.text_strong : tc.text_default, dim), text_label));
    }
    const Color label_color = c.kind == UiSettingsControlKind::notice &&
                                      variant(scene.content, entry.source_index) == UiSettingsVariant::critical
                                  ? tc.critical_text
                                  : ui_settings_color_alpha(tc.text_strong, dim);
    if (!text(stream, scene, handle, c.label, r.label, label_color, text_label) ||
        !describe(stream, scene, handle, c.description, r.description, ui_settings_color_alpha(tc.text_subtle, dim)))
        return false;
    const u32 index = entry.source_index;
    // Initials only for avatar rows without an image to draw.
    if (c.kind == UiSettingsControlKind::action && has_flag(scene.content, index, ui_settings_presentation_avatar) &&
        !avatar_backed(scene.content, index)) {
        char storage[16];
        UiRect lead = r.lead;
        if (has_flag(scene.content, index, ui_settings_presentation_grip))
            lead.left += t.metric.grip_width + 4;
        const float size = t.metric.avatar_size, cy = (lead.top + lead.bottom) * .5f;
        if (!text(stream, scene, handle, ui_admin_initials(c.label, {storage, sizeof(storage)}),
                  {lead.left, cy - size * .5f, lead.left + size, cy + size * .5f}, tc.on_brand, text_heading,
                  UiTextAlignment::center))
            return false;
    }
    if (ui_rect_has_area(r.chips)) {
        const bool dots = accent(scene.content, index);
        for (u32 i = 0; i < c.choices.count; ++i) {
            const auto chip = ui_admin_chip_rect(t, c.choices, dots, r.chips, i);
            if (ui_rect_has_area(chip) &&
                !text(stream, scene, handle, c.choices[i],
                      ui_admin_chip_label_rect(t, chip, ui_admin_chip_dot(c.choices[i], dots)),
                      ui_settings_color_alpha(tc.text_default, dim), text_description))
                return false;
        }
    }
    switch (c.kind) {
    case UiSettingsControlKind::segments:
        if (has_flag(scene.content, index, ui_settings_presentation_tristate))
            return true;
        for (u32 i = 0; i < c.choices.count; ++i)
            if (!text(stream, scene, handle, c.choices[i], ui_segments_option(t, r.control, c.choices.count, i),
                      ui_settings_color_alpha(c.selected_choice == i ? tc.on_brand : tc.text_default, dim),
                      text_button, UiTextAlignment::center))
                return false;
        return true;
    case UiSettingsControlKind::buttons:
        for (u32 i = 0; i < c.choices.count; ++i) {
            const UiControlState option = hover_option(scene, state, i);
            if (!text(stream, scene, handle, c.choices[i], option_rect(t, c, r.control, i),
                      ui_settings_button_text(t, i ? UiSettingsButtonVariant::secondary : UiSettingsButtonVariant::primary,
                                              option),
                      text_button, UiTextAlignment::center))
                return false;
        }
        return true;
    case UiSettingsControlKind::colors:
        for (u32 i = 0; i < c.choices.count; ++i) {
            const auto swatch = option_rect(t, c, r.control, i);
            if (!text(stream, scene, handle, c.choices[i],
                      {swatch.left - 8, swatch.bottom + 4, swatch.right + 8, swatch.bottom + kColorCaption},
                      ui_settings_color_alpha(tc.text_default, dim), text_description, UiTextAlignment::center))
                return false;
        }
        if (c.action_label.bytes) {
            const UiControlState option = hover_option(scene, state, c.choices.count);
            if (!text(stream, scene, handle, c.action_label, option_rect(t, c, r.control, c.choices.count),
                      ui_settings_button_text(t, UiSettingsButtonVariant::secondary, option), text_button,
                      UiTextAlignment::center))
                return false;
        }
        return true;
    case UiSettingsControlKind::action:
    case UiSettingsControlKind::notice: {
        if (!c.action_label.bytes || !ui_rect_has_area(r.control))
            return true;
        const auto v = variant(scene.content, entry.source_index);
        const auto button = c.kind == UiSettingsControlKind::notice ? UiSettingsButtonVariant::secondary
                            : v == UiSettingsVariant::critical      ? UiSettingsButtonVariant::critical
                            : v == UiSettingsVariant::primary       ? UiSettingsButtonVariant::primary
                                                                    : UiSettingsButtonVariant::secondary;
        return text(stream, scene, handle, c.action_label, r.control, ui_settings_button_text(t, button, state),
                    text_button, UiTextAlignment::center);
    }
    case UiSettingsControlKind::choice:
        // An unselected dropdown shows the "Select…" placeholder.
        return c.selected_choice >= c.choices.count
                   ? text(stream, scene, handle, utf8_literal("Select\xE2\x80\xA6"), ui_select_text_rect(t, r.control),
                          ui_settings_color_alpha(tc.text_subtle, dim), text_label)
                   : text(stream, scene, handle, c.choices[c.selected_choice], ui_select_text_rect(t, r.control),
                          ui_settings_color_alpha(tc.text_strong, dim), text_label);
    case UiSettingsControlKind::radio:
        for (u32 i = 0; i < c.choices.count; ++i)
            if (!text(stream, scene, handle, c.choices[i], ui_radio_label(t, ui_radio_option(t, r.control, i)),
                      ui_settings_color_alpha(c.selected_choice == i ? tc.text_strong : tc.text_default, dim),
                      text_label))
                return false;
        return true;
    case UiSettingsControlKind::slider: {
        const bool with_marks = marks(scene.content, entry.source_index);
        if (with_marks) {
            const auto g = ui_slider_geometry(t, r.control, c.choices.count, c.selected_choice, true);
            const u32 fallback = default_choice(scene.content, entry.source_index);
            for (u32 i = 0; i < c.choices.count; ++i)
                if (!text(stream, scene, handle, c.choices[i], ui_slider_mark_label(t, g, c.choices.count, i),
                          ui_settings_color_alpha(i == fallback ? tc.positive
                                                  : i == c.selected_choice ? tc.text_strong
                                                                           : tc.text_subtle,
                                                  dim),
                          text_description, UiTextAlignment::center))
                    return false;
            return true;
        }
        return c.selected_choice >= c.choices.count ||
               text(stream, scene, handle, c.choices[c.selected_choice], r.value,
                    ui_settings_color_alpha(tc.text_default, dim), text_description, UiTextAlignment::end);
    }
    case UiSettingsControlKind::palette: {
        const u32 columns = ui_swatch_columns(t, ui_rect_width(r.control));
        for (u32 i = 0; i < c.choices.count; ++i) {
            const auto swatch = ui_swatch_rect(t, r.control, i, columns);
            const UiRect label{swatch.left - 8, swatch.bottom + 6, swatch.right + 8,
                               swatch.bottom + ui_swatch_label_height()};
            if (!text(stream, scene, handle, c.choices[i], label,
                      ui_settings_color_alpha(c.selected_choice == i ? tc.text_strong : tc.text_subtle, dim),
                      text_description, UiTextAlignment::center, TextWrapping::wrap, UiTextAlignment::start))
                return false;
        }
        return true;
    }
    case UiSettingsControlKind::edit:
    case UiSettingsControlKind::multiline: {
        if (ui_rect_has_area(r.value) && c.action_label.bytes) {
            const UiControlState button = hover_option(scene, state, 0);
            if (!text(stream, scene, handle, c.action_label, r.value,
                      ui_settings_button_text(t, UiSettingsButtonVariant::primary, button), text_button,
                      UiTextAlignment::center))
                return false;
        }
        const auto& editor = *c.editor;
        if (!geometry_valid(editor.geometry))
            return false;
        UiSettingsControlGeometry geometry{};
        if (!ui_settings_scene_control_geometry(scene, entry.target, &geometry))
            return true;
        const bool entered = editor.view.text.bytes != 0;
        const bool measured = entered && editor.geometry && ui_rect_has_area(editor.geometry->text);
        const auto rectangle = measured ? editor.geometry->text : geometry.text;
        const auto text_clip = ui_rect_overlap(geometry.text, geometry.clip);
        if (c.kind == UiSettingsControlKind::multiline && scene.content.sectioned && c.action_label.bytes) {
            const UiRect counter{geometry.field.right - 14 - 64, geometry.field.bottom - 10 - kUiSettingsLabelLine,
                                 geometry.field.right - 14, geometry.field.bottom - 10};
            if (!text(stream, scene, handle, c.action_label, counter,
                      variant(scene.content, entry.source_index) == UiSettingsVariant::critical ? tc.critical_text
                                                                                                : tc.text_default,
                      text_button, UiTextAlignment::end))
                return false;
        }
        if (!ui_rect_has_area(text_clip))
            return true;
        return ui_text_output_push(
            stream, {entered ? editor.view.text : editor.placeholder,
                     {&scene.widgets, handle},
                     rectangle,
                     text_clip,
                     entered ? ui_settings_color_alpha(tc.text_strong, dim) : tc.text_subtle,
                     0,
                     UiTextAlignment::start,
                     c.kind == UiSettingsControlKind::multiline || measured ? UiTextAlignment::start
                                                                            : UiTextAlignment::center,
                     c.kind == UiSettingsControlKind::multiline ? TextWrapping::wrap : TextWrapping::no_wrap,
                     TextOverflow::clip});
    }
    default:
        return true;
    }
}
} // namespace

bool ui_settings_scene_text_output(const UiSettingsScene& scene, const Theme& theme, UiTextOutput* stream) noexcept {
    if (!scene.active)
        return true;
    if (!stream || !current(scene))
        return false;
    const UiSettingsThemeTokens t(theme);
    const auto& tc = t.color;
    // Chrome text first; page-owned text stays contiguous for the page layer.
    for (u32 i = 0; i < scene.widgets.count; ++i) {
        const auto& entry = entries(scene)[i];
        if (entry.role != EntryRole::nav)
            continue;
        const UiWidgetHandle handle{static_cast<u16>(i), scene.widgets.generation};
        const auto* node = ui_widget_manager_layout_node(scene.widgets, handle);
        NavRow row{};
        if (!node || !nav_row(scene, entry.source_index, &row) || row.kind == UiSettingsNavKind::separator ||
            !ui_rect_has_area(ui_rect_overlap(node->rectangle, node->clip_rectangle)))
            continue;
        if (row.kind == UiSettingsNavKind::header) {
            const float inset = t.metric.nav_item_padding;
            // Bottom-aligned: the group gap stays above the label.
            if (!text(stream, scene, handle, row.label,
                      {node->rectangle.left + inset, node->rectangle.bottom - t.metric.nav_header_height + 4,
                       node->rectangle.right - inset, node->rectangle.bottom - 4},
                      tc.nav_header, text_eyebrow))
                return false;
            continue;
        }
        const bool selected = row.kind == UiSettingsNavKind::page && ui_page_key_equal(row.target.page, scene.selected_page);
        const auto state = widget_state(scene, handle, row.target, true);
        if (!text(stream, scene, handle, row.label,
                  ui_settings_nav_label_rect(t, node->rectangle, row.icon < UiIcon::count),
                  ui_settings_nav_item_text(t, selected, row.destructive, state), text_label))
            return false;
    }
    if (scene.search_field.generation) {
        UiSettingsControlGeometry geometry{};
        if (ui_settings_scene_search_geometry(scene, &geometry)) {
            const auto& search = scene.search;
            const bool entered = search.view.text.bytes != 0;
            const bool measured = entered && search.view.focused && search.geometry &&
                                  ui_rect_has_area(search.geometry->text);
            const auto clip = ui_rect_overlap(geometry.text, geometry.clip);
            if (ui_rect_has_area(clip) &&
                !ui_text_output_push(stream, {entered ? search.view.text
                                                      : (search.placeholder.bytes ? search.placeholder
                                                                                  : utf8_literal("Search")),
                                              {&scene.widgets, scene.search_field},
                                              measured ? search.geometry->text : geometry.text,
                                              clip,
                                              entered ? tc.text_strong : tc.text_subtle,
                                              text_label,
                                              UiTextAlignment::start,
                                              measured ? UiTextAlignment::start : UiTextAlignment::center,
                                              TextWrapping::no_wrap,
                                              TextOverflow::clip}))
                return false;
        }
    }
    if (scene.presentation != UiSettingsPresentation::dialog) {
        const UiSettingsTarget target{UiSettingsTargetKind::close, {}, 0};
        const auto state = widget_state(scene, scene.close, target, true);
        if (!text(stream, scene, scene.close, utf8_literal("ESC"),
                  ui_settings_close_caption_rect(t, scene.layout.close), ui_settings_close_color(t, state),
                  text_eyebrow, UiTextAlignment::center))
            return false;
    }
    const auto* title = ui_widget_manager_layout_node(scene.widgets, scene.title);
    const auto* description = ui_widget_manager_layout_node(scene.widgets, scene.description);
    if (!title || !description ||
        !text(stream, scene, scene.title, scene.content.title, title->rectangle, tc.text_strong,
              text_style_heading_2 | text_style_bold) ||
        !describe(stream, scene, scene.description, scene.content.description, description->rectangle, tc.text_subtle))
        return false;
    for (u32 i = 0; i < scene.widgets.count; ++i) {
        const auto& entry = entries(scene)[i];
        if (entry.role != EntryRole::row || entry.source_index >= scene.content.controls.count)
            continue;
        if (!row_text(scene, t, stream, {static_cast<u16>(i), scene.widgets.generation}, entry))
            return false;
    }
    UiRect aside_heading{}, aside_clip{};
    if (scene.content.aside_label.bytes && ui_settings_scene_aside(scene, &aside_heading, nullptr, &aside_clip)) {
        const auto clip = ui_rect_overlap(aside_heading, aside_clip);
        if (ui_rect_has_area(clip) &&
            !ui_text_output_push(stream, {scene.content.aside_label,
                                          {&scene.widgets, scene.body},
                                          aside_heading,
                                          clip,
                                          tc.text_strong,
                                          text_label,
                                          UiTextAlignment::start,
                                          UiTextAlignment::center,
                                          TextWrapping::no_wrap,
                                          TextOverflow::ellipsis}))
            return false;
    }
    if (scene.footer.visible) {
        const float dx = scene.footer_shift_q / 16.f, alert = scene.footer_alert_q / 255.f;
        const auto moved = [dx](UiRect r) noexcept { return UiRect{r.left + dx, r.top, r.right + dx, r.bottom}; };
        const Color on_alert = ui_settings_color_mix(tc.text_strong, tc.on_brand, alert);
        const auto* status = ui_widget_manager_layout_node(scene.widgets, scene.footer_status);
        if (status) {
            // Dialog footers carry actions, not a draft: no unsaved-changes prompt.
            const auto label = scene.footer.status.bytes ? scene.footer.status
                               : scene.footer.saving     ? utf8_literal("Saving…")
                               : scene.footer.failed     ? utf8_literal("Couldn't save. Try again.")
                               : scene.presentation == UiSettingsPresentation::dialog
                                   ? Utf8View{}
                                   : utf8_literal("Careful — you have unsaved changes!");
            if (!text(stream, scene, scene.footer_status, label, moved(status->rectangle),
                      scene.footer.failed && !scene.footer.saving && !scene.footer_alert_q ? tc.critical_text
                                                                                              : on_alert,
                      text_label))
                return false;
        }
        const UiSettingsTarget reset{UiSettingsTargetKind::reset, scene.selected_page, 0};
        const UiSettingsTarget save{UiSettingsTargetKind::save, scene.selected_page, 0};
        const auto* reset_node = ui_widget_manager_layout_node(scene.widgets, scene.footer_reset);
        const auto* save_node = ui_widget_manager_layout_node(scene.widgets, scene.footer_save);
        if (reset_node &&
            !text(stream, scene, scene.footer_reset,
                  scene.footer.reset_label.bytes ? scene.footer.reset_label : utf8_literal("Reset"),
                  moved(reset_node->rectangle),
                  target_exists(scene, reset) ? on_alert : ui_settings_color_alpha(on_alert, .5f),
                  text_button, UiTextAlignment::center))
            return false;
        if (save_node &&
            !text(stream, scene, scene.footer_save,
                  scene.footer.saving               ? utf8_literal("Saving…")
                  : scene.footer.save_label.bytes ? scene.footer.save_label
                                                  : utf8_literal("Save changes"),
                  moved(save_node->rectangle),
                  ui_settings_button_text(t, UiSettingsButtonVariant::primary,
                                          {target_exists(scene, save), false, false, false}),
                  text_button, UiTextAlignment::center))
            return false;
    }
    return true;
}
} // namespace maicord
