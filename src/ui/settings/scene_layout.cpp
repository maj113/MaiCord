#include "scene_internal.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/kit/scene_kit.hpp"

namespace maicord {
using namespace settings_scene_detail;

namespace settings_scene_detail {
namespace {
constexpr u16 interactive = ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable;
// Title line of the 20 DIP page heading; the official scroll-into-view paddings.
constexpr float kTitleLine = 26, kRevealPadding = 16, kSidebarRevealPadding = 12;
} // namespace
Entry* entries(UiSettingsScene& scene) noexcept {
    return static_cast<Entry*>(scene.storage.sidecar);
}
const Entry* entries(const UiSettingsScene& scene) noexcept {
    return static_cast<const Entry*>(scene.storage.sidecar);
}
namespace {
float* prefixes(UiSettingsScene& scene) noexcept {
    return reinterpret_cast<float*>(entries(scene) + scene.capacity);
}
const float* prefixes(const UiSettingsScene& scene) noexcept {
    return reinterpret_cast<const float*>(entries(scene) + scene.capacity);
}
} // namespace
bool editor_kind(UiSettingsControlKind kind) noexcept {
    return kind == UiSettingsControlKind::edit || kind == UiSettingsControlKind::multiline;
}
namespace {
bool side_control(UiSettingsControlKind kind) noexcept {
    return kind == UiSettingsControlKind::toggle || kind == UiSettingsControlKind::action ||
           kind == UiSettingsControlKind::segments;
}
} // namespace
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
bool has_flag(const UiSettingsPageContent& content, u32 index, u8 flag) noexcept {
    const auto* p = presentation(content, index);
    return p && (p->flags & flag);
}
bool accent(const UiSettingsPageContent& content, u32 index) noexcept {
    return has_flag(content, index, ui_settings_presentation_accent);
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
namespace {
// Action rows may carry list decorations: chips are their choices (list
// rows use choices[0] as their count instead).
bool chips(const UiSettingsPageContent& content, u32 index) noexcept {
    const auto& c = content.controls[index];
    return c.kind == UiSettingsControlKind::action && c.choices.data && c.choices.count && c.choices.count <= 16 &&
           !list_item(content, index);
}
} // namespace
// Action rows with a meter level show it as a progress bar (thousandths).
bool progress(const UiSettingsPageContent& content, u32 index) noexcept {
    return content.controls[index].kind == UiSettingsControlKind::action && content.meters &&
           content.meters[index] != kUiSettingsNoMeter && !list_item(content, index);
}
float button_label_width(const Utf8View& label) noexcept {
    return ui_settings_text_width(label, kUiSettingsDescriptionEm) * 1.08f;
}
namespace {
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

} // namespace
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
namespace {
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
} // namespace
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

namespace {
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
} // namespace
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
namespace {
// Group headers after the first open their group with extra space (the official
// client separates sidebar sections), so a header never sits flush under a row.
constexpr float kNavGroupGap = 8;
float nav_height(const UiSettingsMetrics& m, UiSettingsNavKind kind, u32 index) noexcept {
    return kind == UiSettingsNavKind::header      ? m.nav_header_height + (index ? kNavGroupGap : 0)
           : kind == UiSettingsNavKind::separator ? 1 + m.separator_margin * 2
                                                  : m.nav_item_height + m.nav_item_gap;
}
} // namespace
float nav_extent(const UiSettingsScene& scene, const UiSettingsMetrics& m) noexcept {
    float extent = 0;
    u32 index = 0;
    visit_nav(scene, [&](const NavRow& row) noexcept {
        extent += nav_height(m, row.kind, index++);
        return true;
    });
    return extent;
}

// Dialog footer band: buttons between the shared notice padding.
float dialog_footer_height(const UiSettingsMetrics& m) noexcept {
    return m.button_small + m.notice_padding * 2 + 8;
}
namespace {
float description_height(const UiSettingsScene& scene, const UiSettingsMetrics& m) noexcept {
    return scene.content.description.bytes
               ? text_block(m, {}, scene.content.description, ui_rect_width(scene.layout.column)) + m.label_gap
               : 0;
}
} // namespace
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
namespace {
float row_width(const UiSettingsScene& scene, const AsideLayout& aside, u32 index) noexcept {
    return aside.active && aside.beside && index >= scene.content.aside_row ? aside.form : body_width(scene);
}
} // namespace
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
namespace {
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
} // namespace
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
namespace {
// Choice-count limits per kind; a kind marked optional may start unselected
// (a dropdown's "pick one to add"), the rest always have a selection.
struct ChoiceRule {
    UiSettingsControlKind kind;
    u16 minimum, maximum;
    bool optional;
};
constexpr ChoiceRule kChoiceRules[]{
    {UiSettingsControlKind::choice, 1, 1024, true},   {UiSettingsControlKind::palette, 1, 12, true},
    {UiSettingsControlKind::slider, 2, 1024, false},  {UiSettingsControlKind::radio, 1, 16, true},
    {UiSettingsControlKind::segments, 2, 4, false},   {UiSettingsControlKind::swatches, 1, 32, true},
    {UiSettingsControlKind::tabs, 2, 8, false},       {UiSettingsControlKind::buttons, 1, 4, true},
    {UiSettingsControlKind::colors, 1, 4, true},
};
const ChoiceRule* choice_rule(UiSettingsControlKind kind) noexcept {
    for (const auto& rule : kChoiceRules)
        if (rule.kind == kind)
            return &rule;
    return nullptr;
}
} // namespace
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
        const auto* rule = choice_rule(c.kind);
        if (rule && (!c.choices.data || c.choices.count < rule->minimum || c.choices.count > rule->maximum ||
                     (c.selected_choice >= c.choices.count &&
                      !(rule->optional && c.selected_choice == kUiSettingsNoChoice))))
            return false;
        if (rule || chips(content, i))
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
                                      u32* index) noexcept {
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
namespace {
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
} // namespace
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
namespace {
UiSettingsTarget fallback_focus(const UiSettingsScene& scene) noexcept {
    if (scene.presentation == UiSettingsPresentation::modal)
        return {UiSettingsTargetKind::search, {}, 0};
    return {UiSettingsTargetKind::close, {}, 0};
}
} // namespace
UiWidgetHandle target_handle(const UiSettingsScene& scene, const UiSettingsTarget& target) noexcept {
    for (u32 i = 0; i < scene.widgets.count; ++i)
        if (ui_settings_target_equal(entries(scene)[i].target, target))
            return {static_cast<u16>(i), scene.widgets.generation};
    return ui_widget_invalid_handle();
}
namespace {
UiWidgetHandle add(UiSettingsScene* scene, UiWidgetHandle parent, UiWidgetKind kind, const Utf8View& name,
                   const UiLayoutStyle& style, const UiSettingsTarget& target, u32 source_index, bool enabled,
                   EntryRole role) noexcept {
    const u32 id = scene->widgets.count + 1;
    const auto result = ui_widget_manager_add_prevalidated(
        &scene->widgets, parent,
        {id,
         kind,
         static_cast<u16>(target.kind != UiSettingsTargetKind::none && enabled ? interactive : ui_widget_none),
         {name, style, {}, 0, id}});
    if (!result.ok()) {
        scene->build_failed = true;
        return ui_widget_invalid_handle();
    }
    entries(*scene)[result.handle.slot] = {target, source_index, role};
    return result.handle;
}
// A layout-only node: no target, never interactive.
UiWidgetHandle add(UiSettingsScene* scene, UiWidgetHandle parent, UiWidgetKind kind, const Utf8View& name,
                   const UiLayoutStyle& style) noexcept {
    return add(scene, parent, kind, name, style, {}, 0, true, EntryRole::normal);
}
// A node answering to `target`.
UiWidgetHandle add(UiSettingsScene* scene, UiWidgetHandle parent, UiWidgetKind kind, const Utf8View& name,
                   const UiLayoutStyle& style, const UiSettingsTarget& target) noexcept {
    return add(scene, parent, kind, name, style, target, 0, true, EntryRole::normal);
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
} // namespace
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
    scene->build_failed = false;
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
    }
    style = placed(f.nav, viewport);
    style.flow = UiLayoutFlow::vertical;
    style.flags |= ui_layout_node_clip_children | ui_layout_node_scroll_vertical;
    scene->sidebar = add(scene, root, UiWidgetKind::scroll_view, utf8_literal("Pages"), style);
    u32 nav_index = 0;
    visit_nav(*scene, [&](const NavRow& row) noexcept {
        const u32 i = nav_index++;
        const bool button = row.kind == UiSettingsNavKind::page || row.kind == UiSettingsNavKind::action;
        auto item = sized(-1, button ? m.nav_item_height : nav_height(m, row.kind, i));
        if (button)
            item.margin.bottom = m.nav_item_gap;
        add(scene, scene->sidebar,
            row.target.kind != UiSettingsTargetKind::none ? UiWidgetKind::button : UiWidgetKind::container,
            utf8_literal("Navigation"), item, row.target, i, true, EntryRole::nav);
        return true;
    });
    scene->title = add(scene, root, UiWidgetKind::text, utf8_literal("Page title"), placed(f.title, viewport));
    scene->description =
        add(scene, root, UiWidgetKind::text, utf8_literal("Page description"), placed(f.description, viewport));
    style = placed(f.body, viewport);
    style.flow = UiLayoutFlow::vertical;
    style.flags |= ui_layout_node_clip_children | ui_layout_node_scroll_vertical;
    scene->body = add(scene, root, UiWidgetKind::scroll_view, utf8_literal("Controls"), style);
    const u32 count = scene->content.controls.count;
    const u32 first = first_row(*scene, scene->content_offset);
    u32 end = first;
    const float bottom = scene->content_offset + ui_rect_height(f.body);
    while (end < count && row_start(*scene, end) < bottom)
        ++end;
    end = minimum(count, end + 1);
    add(scene, scene->body, UiWidgetKind::container, utf8_literal("Before"), sized(width, row_start(*scene, first)));
    for (u32 i = first; i < end; ++i) {
        const auto& c = scene->content.controls[i];
        const UiSettingsTarget target{UiSettingsTargetKind::control, scene->selected_page, c.id};
        const bool interactive_row = focusable_control(*scene, i);
        const auto kind = editor_kind(c.kind) ? UiWidgetKind::text_field : UiWidgetKind::button;
        add(scene, scene->body, interactive_row ? kind : UiWidgetKind::container, utf8_literal("Setting"),
            sized(row_width(*scene, aside, i), row_start(*scene, i + 1) - row_start(*scene, i)), target, i,
            interactive_row, EntryRole::row);
    }
    add(scene, scene->body, UiWidgetKind::container, utf8_literal("After"),
        sized(width, content_extent(*scene, m) - row_start(*scene, end)));
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
                                  {UiSettingsTargetKind::reset, scene->selected_page, 0}, 0, !scene->footer.saving,
                                  EntryRole::normal);
        scene->footer_save =
            add(scene, footer, UiWidgetKind::button, utf8_literal("Save draft"), sized(save_width, m.button_small),
                {UiSettingsTargetKind::save, scene->selected_page, 0}, 0,
                scene->footer.can_save && !scene->footer.saving, EntryRole::normal);
    }
    scene->close = add(scene, root, UiWidgetKind::button, utf8_literal("Close settings"),
                       placed(scene->layout.close, viewport), {UiSettingsTargetKind::close, {}, 0});
    // The scroll offsets index the sidebar and body slots, so a failed add stops here.
    if (scene->build_failed ||
        !ui_widget_manager_set_scroll(&scene->widgets, scene->sidebar, {0, scene->sidebar_offset}) ||
        !ui_widget_manager_set_scroll(&scene->widgets, scene->body, {0, scene->content_offset}) ||
        !ui_widget_manager_layout_rectangle(&scene->widgets, scene->viewport).ok())
        return false;
    if (!target_exists(*scene, scene->focus))
        scene->focus = fallback_focus(*scene);
    ui_widget_manager_focus(&scene->widgets, target_handle(*scene, scene->focus));
    ++scene->revision;
    return true;
}
bool row_layout_for(const UiSettingsScene& scene, const UiSettingsLayoutTokens& t, UiWidgetHandle handle, u32 source,
                    RowLayout* layout, UiRect* clip) noexcept {
    const auto* node = ui_widget_manager_layout_node(scene.widgets, handle);
    if (!node || source >= scene.content.controls.count)
        return false;
    *layout = row_layout(t, scene.style, scene.content, source, node->rectangle.left, node->rectangle.top,
                         ui_rect_width(node->rectangle));
    if (clip)
        *clip = node->clip_rectangle;
    return true;
}
namespace {
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
} // namespace
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
    if (!ui_widget_manager_set_scroll(&scene->widgets, scroller, {0, offset}) ||
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
} // namespace settings_scene_detail
} // namespace maicord
