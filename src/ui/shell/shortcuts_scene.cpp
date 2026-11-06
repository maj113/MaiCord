#include "maicord/ui/shell/shortcuts_scene.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/layout/rect_edges.hpp"
#include "maicord/ui/theme/foreground_color.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/core/visit_ref.hpp"

namespace maicord {
namespace {
constexpr float kHeaderHeight = 34, kRowHeight = 40, kTallRowHeight = 60, kColumnGap = 32, kCapHeight = 24,
                kCapGap = 4, kChordGap = 14, kKeyEm = 12, kLabelEm = 15, kScrollGutter = 12;
constexpr UiLook kClose{.fill = UI_TOKEN(surface_high), .border = UI_TOKEN(interactive), .focus = UI_TOKEN(interactive),
                        .flags = ui_look_hot_only | ui_look_dialog | ui_look_radius_md};

bool current(const UiShortcutsScene& s) noexcept {
    return s.active && s.content && s.content->revision == s.content_revision &&
           s.content->rows.count == s.row_count;
}
bool valid_view(const Utf8View& text, u32 maximum) noexcept {
    return text.bytes <= maximum && utf8_validate(text).valid;
}
bool valid_row(const UiShortcutsRow& row) noexcept {
    if (!row.label.bytes || !valid_view(row.label, 256) || row.key_counts[0] > kUiShortcutsChordKeys ||
        row.key_counts[1] > kUiShortcutsChordKeys || (row.header && (row.key_counts[0] || row.key_counts[1])))
        return false;
    for (u32 chord = 0; chord < 2; ++chord)
        for (u32 key = 0; key < row.key_counts[chord]; ++key)
            if (!row.keys[chord][key].bytes || !valid_view(row.keys[chord][key], 32))
                return false;
    return true;
}
bool initialize(UiShortcutsScene& s) noexcept {
    if (s.storage.arena.base)
        return true;
    if (!ui_scene_open(&s.storage, &s.widgets, 4))
        return false;
    s.root = ui_scene_part_root(&s.widgets, utf8_literal("Keyboard shortcuts"));
    if (!s.root.generation)
        return false;
    s.close = ui_scene_part(&s.widgets, s.root, UiWidgetKind::button, utf8_literal("Close keyboard shortcuts"));
    return s.close.generation;
}
float chord_width(const UiShortcutsRow& row, u32 chord) noexcept {
    float width = 0;
    for (u32 key = 0; key < row.key_counts[chord]; ++key)
        width += ui_shortcuts_key_width(row.keys[chord][key]) + (key ? kCapGap : 0);
    return width;
}
float caps_width(const UiShortcutsRow& row) noexcept;
// A label that would not fit beside its key caps wraps onto a second line.
float row_height(const UiShortcutsRow& row, float column_width) noexcept {
    if (row.header)
        return kHeaderHeight;
    const float available = column_width - caps_width(row) - 12;
    return ui_settings_text_width(row.label, kLabelEm) * 1.04f > available ? kTallRowHeight : kRowHeight;
}
// Groups (a heading and its rows) fill the shorter column in order.
void layout_rows(UiShortcutsScene& s) noexcept {
    const auto rows = s.content->rows;
    // The overlay scrollbar keeps a gutter clear of the key caps.
    const float width = ui_rect_width(s.body) - kScrollGutter;
    s.columns = width >= 600 ? 2 : 1;
    const float column_width = (width - (s.columns - 1) * kColumnGap) / s.columns;
    float heights[2]{};
    for (u32 i = 0; i < rows.count;) {
        u32 end = i + 1;
        while (end < rows.count && !rows[end].header)
            ++end;
        const u32 column = s.columns == 2 && heights[1] < heights[0] ? 1 : 0;
        float y = heights[column] + (heights[column] > 0 && rows[i].header ? 12.f : 0.f);
        const float x = column * (column_width + kColumnGap);
        for (u32 j = i; j < end; ++j) {
            const float height = row_height(rows[j], column_width);
            s.rows[j] = {x, y, x + column_width, y + height};
            y += height;
        }
        heights[column] = y;
        i = end;
    }
    s.content_height = maximum(heights[0], heights[1]);
}
float scroll_limit(const UiShortcutsScene& s) noexcept {
    return maximum(0.f, s.content_height - ui_rect_height(s.body));
}
UiRect absolute(const UiShortcutsScene& s, u32 index) noexcept {
    const auto r = s.rows[index];
    return {s.body.left + r.left, s.body.top + r.top - s.scroll, s.body.left + r.right,
            s.body.top + r.bottom - s.scroll};
}
// Key caps right-aligned in a row: chord 0, a separator, chord 1.
bool each_cap(const UiShortcutsRow& row, const UiRect& r, UiVisitRef<const Utf8View&, const UiRect&> visit) noexcept {
    float right = r.right;
    for (i32 chord = 1; chord >= 0; --chord) {
        if (!row.key_counts[chord])
            continue;
        float x = right - chord_width(row, static_cast<u32>(chord));
        right = x - kChordGap;
        const float top = (r.top + r.bottom - kCapHeight) * .5f;
        for (u32 key = 0; key < row.key_counts[chord]; ++key) {
            const float width = ui_shortcuts_key_width(row.keys[chord][key]);
            if (!visit(row.keys[chord][key], UiRect{x, top, x + width, top + kCapHeight}))
                return false;
            x += width + kCapGap;
        }
        if (chord == 1 && row.key_counts[0] && !visit(Utf8View{}, UiRect{right, top, right + kChordGap, top + kCapHeight}))
            return false;
    }
    return true;
}
float caps_width(const UiShortcutsRow& row) noexcept {
    float width = chord_width(row, 0) + chord_width(row, 1);
    if (row.key_counts[0] && row.key_counts[1])
        width += kChordGap;
    return width;
}
} // namespace

float ui_shortcuts_key_width(const Utf8View& key) noexcept {
    return maximum(kCapHeight, ui_settings_text_width(key, kKeyEm) * 1.1f + 14);
}
bool ui_shortcuts_scene_current(const UiShortcutsScene& s) noexcept {
    return current(s);
}
bool ui_shortcuts_scene_update(UiShortcutsScene* s, const UiShortcutsContent* content, UiRect viewport) noexcept {
    if (!s || !content || !ui_rect_valid(viewport) || ui_rect_width(viewport) < 240 ||
        ui_rect_height(viewport) < 240 || content->rows.count > kUiShortcutsMaximumRows ||
        (content->rows.count && !content->rows.data) || !valid_view(content->title, 128) ||
        !valid_view(content->query, 256) || !valid_view(content->placeholder, 128) || !valid_view(content->empty, 256))
        return false;
    if (current(*s) && s->content == content && ui_rect_equal(s->viewport, viewport))
        return true;
    for (u32 i = 0; i < content->rows.count; ++i)
        if (!valid_row(content->rows[i]))
            return false;
    if (!initialize(*s))
        return false;
    const bool rebuilt = !s->active || s->content_revision != content->revision || s->content != content;
    s->content = content;
    s->content_revision = content->revision;
    s->row_count = content->rows.count;
    s->viewport = viewport;
    const float inset = ui_rect_width(viewport) < 480 ? 12.f : 32.f;
    const float width = minimum(820.f, ui_rect_width(viewport) - inset * 2);
    const float height = minimum(640.f, ui_rect_height(viewport) - 48);
    const float left = viewport.left + (ui_rect_width(viewport) - width) * .5f;
    const float top = viewport.top + (ui_rect_height(viewport) - height) * .5f;
    s->panel = {left, top, left + width, top + height};
    const float padding = width < 420 ? 16.f : 24.f;
    s->close_rectangle = {s->panel.right - 48, top + 14, s->panel.right - 16, top + 46};
    s->search = {left + padding, top + 62, s->panel.right - padding, top + 98};
    s->body = {left + padding, top + 114, s->panel.right - padding, s->panel.bottom - padding};
    layout_rows(*s);
    if (rebuilt)
        s->scroll = 0;
    s->scroll = minimum(s->scroll, scroll_limit(*s));
    s->active = true;
    ui_widget_manager_place_part(&s->widgets, s->close, s->close_rectangle, s->panel, true, true);
    if (!ui_widget_manager_layout_rectangle(&s->widgets, s->panel).ok()) {
        s->active = false;
        return false;
    }
    ++s->revision;
    return true;
}
void ui_shortcuts_scene_release(UiShortcutsScene* s) noexcept {
    ui_scene_release(s);
}
UiShortcutsTarget ui_shortcuts_scene_hit_test(const UiShortcutsScene& s, float x, float y) noexcept {
    if (!current(s) || !ui_rect_contains(s.viewport, x, y))
        return UiShortcutsTarget::none;
    if (!ui_rect_contains(s.panel, x, y) || ui_rect_contains(s.close_rectangle, x, y))
        return UiShortcutsTarget::close;
    if (ui_rect_contains(s.search, x, y))
        return UiShortcutsTarget::search;
    return ui_rect_contains(s.body, x, y) ? UiShortcutsTarget::body : UiShortcutsTarget::none;
}
bool ui_shortcuts_scene_scroll(UiShortcutsScene* s, float delta) noexcept {
    if (!s || !current(*s))
        return false;
    const float next = minimum(maximum(0.f, s->scroll + delta), scroll_limit(*s));
    if (next == s->scroll)
        return false;
    s->scroll = next;
    ++s->revision;
    return true;
}
bool ui_shortcuts_scene_set_hover(UiShortcutsScene* s, float x, float y, bool* changed) noexcept {
    if (changed)
        *changed = false;
    return s && current(*s) && ui_scene_hover(&s->widgets, &s->revision, x, y, changed);
}
bool ui_shortcuts_scene_row(const UiShortcutsScene& s, u32 index, UiRect* rectangle) noexcept {
    if (!current(s) || index >= s.row_count)
        return false;
    const auto r = absolute(s, index);
    if (r.bottom <= s.body.top || r.top >= s.body.bottom)
        return false;
    if (rectangle)
        *rectangle = r;
    return true;
}
bool ui_shortcuts_scene_paint(const UiShortcutsScene& s, const Theme& t, PaintStream* out) noexcept {
    if (!current(s))
        return true;
    const auto q = s.search;
    UiPaint p{out, t};
    p.dialog(s.viewport, s.panel)
        .icon(UiIcon::keyboard, ui_rect_edges(s.panel, ui_start(24), ui_start(20), ui_start(44), ui_start(40)),
              ui_foreground_color(t, t.tokens.emphasis.outline))
        .look<kClose>(s.close_rectangle, ui_widget_state(s.widgets, s.close))
        .icon(UiIcon::close, ui_rect_shrink(s.close_rectangle, 6), t.text_secondary)
        // Query line: the search field look, without a caret (typing filters).
        .surface(q, t.base_lowest, t.surface_high, t.scale.radius_md, true)
        .icon(UiIcon::search, ui_rect_edges(q, ui_end(-30), ui_start(9), ui_end(-12), ui_end(-9)), t.text_secondary);
    if (s.content->query.bytes) {
        // A thin caret after the query hints that typing continues it.
        const float x = minimum(q.right - 40, q.left + 12 + ui_settings_text_width(s.content->query, 14) * 1.04f + 5);
        p.fill({x, q.top + 10, x + 1.5f, q.bottom - 10}, t.text_primary);
    }
    p.clip(s.body);
    for (u32 i = 0; p && i < s.row_count; ++i) {
        UiRect r{};
        if (!ui_shortcuts_scene_row(s, i, &r) || s.content->rows[i].header)
            continue;
        // Hairline between rows of a group.
        if (i + 1 < s.row_count && !s.content->rows[i + 1].header)
            p.separator({r.left, r.bottom - 1, r.right, r.bottom});
        each_cap(s.content->rows[i], r, [&](Utf8View key, UiRect cap) noexcept {
            return !key.bytes || p.surface(cap, t.base_lowest, t.surface_high, t.scale.radius_sm, true)
                                     .rounded({cap.left + 1, cap.bottom - 3, cap.right - 1, cap.bottom - 1},
                                              ui_theme_color_alpha(t.surface_high, .9f), t.scale.radius_xs)
                                     .ok();
        });
    }
    if (s.content_height > ui_rect_height(s.body)) {
        // Overlay scrollbar thumb.
        const float track = ui_rect_height(s.body);
        const float thumb = maximum(32.f, track * track / s.content_height);
        const float top = s.body.top + (track - thumb) * (s.scroll / scroll_limit(s));
        p.rounded({s.body.right - 4, top, s.body.right, top + thumb}, ui_theme_color_alpha(t.text_secondary, .45f),
                  t.scale.radius_xs);
    }
    return p.end();
}
bool ui_shortcuts_scene_text_output(const UiShortcutsScene& s, const Theme& t, UiTextOutput* out) noexcept {
    if (!current(s))
        return true;
    UiTextPen<UiTextClip::region> text{out, s.widgets, s.panel, s.body};
    const auto& c = *s.content;
    const auto q = s.search;
    text(s.root, c.title.bytes ? c.title : utf8_literal("Keyboard shortcuts"),
         ui_rect_edges(s.panel, ui_start(54), ui_start(14), ui_end(-56), ui_start(46)), t.text_primary, text_style_bold);
    text.in(q)(s.root, c.query.bytes ? c.query : c.placeholder, ui_rect_edges(q, ui_start(12), ui_start(0), ui_end(-40), ui_end(0)),
               c.query.bytes ? t.text_primary : t.text_secondary);
    if (!s.row_count)
        return text(s.root, c.empty, {s.body.left, s.body.top + 24, s.body.right, s.body.top + 52}, t.text_secondary, 0,
                    ui_text_in_body | ui_text_center).ok();
    for (u32 i = 0; text.ok() && i < s.row_count; ++i) {
        UiRect r{};
        if (!ui_shortcuts_scene_row(s, i, &r))
            continue;
        const auto& row = c.rows[i];
        if (row.header) {
            text(s.root, row.label, {r.left, r.top + 8, r.right, r.bottom}, ui_foreground_color(t, t.tokens.emphasis.outline),
                 text_style_bold | text_style_subtext, ui_text_in_body);
            continue;
        }
        const float label_right = maximum(r.left + 40, r.right - caps_width(row) - 12);
        const bool tall = ui_rect_height(r) > kRowHeight + .5f;
        text(s.root, row.label, {r.left, r.top + (tall ? 4.f : 0.f), label_right, r.bottom - (tall ? 4.f : 0.f)},
             t.text_primary, 0, ui_text_in_body | (tall ? ui_text_wrap | ui_text_middle : 0));
        each_cap(row, r, [&](Utf8View key, UiRect cap) noexcept {
            return (key.bytes ? text(s.root, key, {cap.left, cap.top, cap.right, cap.bottom - 2}, t.text_primary,
                                     text_style_bold | text_style_subtext, ui_text_in_body | ui_text_center)
                              : text(s.root, utf8_literal("/"), cap, t.text_secondary, text_style_subtext,
                                     ui_text_in_body | ui_text_center))
                .ok();
        });
    }
    return text.ok();
}
} // namespace maicord
