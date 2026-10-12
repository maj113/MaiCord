#include "maicord/ui/shortcuts_scene.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/controls/settings_controls.hpp"

namespace maicord {
namespace {
constexpr float kHeaderHeight = 34, kRowHeight = 40, kTallRowHeight = 60, kColumnGap = 32, kCapHeight = 24,
                kCapGap = 4, kChordGap = 14, kKeyEm = 12, kLabelEm = 15, kScrollGutter = 12;

bool same(UiWidgetHandle a, UiWidgetHandle b) noexcept {
    return a.generation && a.slot == b.slot && a.generation == b.generation;
}
bool current(const UiShortcutsScene& s) noexcept {
    return s.active && s.content && s.content->revision == s.content_revision &&
           s.content->rows.count == s.row_count;
}
bool valid_view(Utf8View text, u32 maximum) noexcept {
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
    if (!ui_fixed_widget_storage_initialize(&s.storage, 4, 0, 1) ||
        !ui_widget_manager_initialize(&s.widgets, s.storage.widgets, s.storage.layout_nodes))
        return false;
    const auto add = [&](UiWidgetHandle parent, UiWidgetKind kind, Utf8View name) noexcept {
        auto style = ui_layout_style();
        style.flags |= ui_layout_node_clip_children | ui_layout_node_allow_overlap | ui_layout_node_allow_zero_size;
        return ui_widget_manager_add_prevalidated(&s.widgets, parent,
                                                  {s.widgets.count + 1, kind, ui_widget_none, {name, style}});
    };
    const auto root = add(ui_widget_invalid_handle(), UiWidgetKind::container, utf8_literal("Keyboard shortcuts"));
    if (!root.ok())
        return false;
    s.root = root.handle;
    auto* node = ui_layout_engine_resolve(&s.widgets.layout, s.widgets.storage[s.root.slot].layout);
    node->spec.style.width = node->spec.style.height = ui_layout_fill();
    const auto close = add(s.root, UiWidgetKind::button, utf8_literal("Close keyboard shortcuts"));
    if (!close.ok())
        return false;
    s.close = close.handle;
    return true;
}
bool place(UiShortcutsScene& s, UiWidgetHandle handle, UiRect r) noexcept {
    auto* widget = ui_widget_manager_resolve(&s.widgets, handle);
    auto* node = widget ? ui_layout_engine_resolve(&s.widgets.layout, widget->layout) : nullptr;
    if (!node)
        return false;
    auto& style = node->spec.style;
    style.width = ui_layout_fixed(ui_rect_width(r));
    style.height = ui_layout_fixed(ui_rect_height(r));
    style.margin = {r.left - s.panel.left, r.top - s.panel.top, 0, 0};
    style.flags = static_cast<u16>(ui_layout_node_clip_children | ui_layout_node_allow_overlap |
                                   ui_layout_node_allow_zero_size | ui_layout_node_visible);
    widget->flags = static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable);
    return true;
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
        float group = 0;
        for (u32 j = i; j < end; ++j)
            group += row_height(rows[j], column_width) + (rows[j].header && j ? 8.f : 0.f);
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
bool emit(UiTextOutput* out, const UiShortcutsScene& s, UiWidgetHandle owner, Utf8View value, UiRect r, UiRect clip,
          Color color, u32 flags = 0, UiTextAlignment alignment = UiTextAlignment::start,
          UiTextWrapping wrapping = UiTextWrapping::no_wrap) noexcept {
    if (!value.bytes || !ui_rect_has_area(r))
        return true;
    return ui_text_output_push(out, {value, {&s.widgets, owner}, r, clip, color, flags, alignment,
                                     UiTextAlignment::center, wrapping, UiTextOverflow::ellipsis,
                                     UiTextFace::normal});
}
// Key caps right-aligned in a row: chord 0, a separator, chord 1.
template <typename Visit> bool each_cap(const UiShortcutsRow& row, UiRect r, Visit&& visit) noexcept {
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

float ui_shortcuts_key_width(Utf8View key) noexcept {
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
    if (!place(*s, s->close, s->close_rectangle) || !ui_widget_manager_layout_rectangle(&s->widgets, s->panel).ok()) {
        s->active = false;
        return false;
    }
    ui_widget_manager_reconcile_input(&s->widgets);
    ++s->revision;
    return true;
}
void ui_shortcuts_scene_release(UiShortcutsScene* s) noexcept {
    if (!s)
        return;
    ui_fixed_widget_storage_release(&s->storage);
    *s = {};
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
    if (!s || !current(*s))
        return false;
    const auto hit = ui_widget_manager_hit_test(s->widgets, x, y), before = s->widgets.hovered;
    if (!ui_widget_manager_hover(&s->widgets, hit))
        return false;
    const bool delta = !same(before, hit) && (before.generation || hit.generation);
    if (delta)
        ++s->revision;
    if (changed)
        *changed = delta;
    return true;
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
    if (!out || !paint_stream_push_clip(out, s.viewport.left, s.viewport.top, s.viewport.right, s.viewport.bottom))
        return false;
    const auto p = s.panel;
    const Color cap_fill = t.base_lowest, cap_border = t.surface_high;
    bool ok = paint_stream_push(out, s.viewport.left, s.viewport.top, s.viewport.right, s.viewport.bottom,
                                t.tokens.overlay.scrim) &&
              ui_surface_paint(out, p, t, UiElevation::dialog, t.scale.radius_lg + t.scale.space_1, nullptr,
                               &t.interactive_hover) &&
              ui_icon_paint(out, UiIcon::keyboard, {p.left + 24, p.top + 20, p.left + 44, p.top + 40},
                            ui_foreground_color(t, t.tokens.emphasis.outline));
    if (ok && (same(s.widgets.hovered, s.close) || same(s.widgets.focused, s.close))) {
        const auto c = s.close_rectangle;
        ok = ui_background_paint_surface(out, c, t, t.surface_high, t.interactive, t.scale.radius_md, 1, 1,
                                         UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest);
    }
    const auto c = s.close_rectangle;
    ok = ok && ui_icon_paint(out, UiIcon::close, {c.left + 6, c.top + 6, c.right - 6, c.bottom - 6}, t.text_secondary);
    // Query line: the search field look, without a caret (typing filters).
    const auto q = s.search;
    ok = ok && ui_background_paint_surface(out, q, t, t.base_lowest, t.surface_high, t.scale.radius_md, 1, 1,
                                           UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest) &&
         ui_icon_paint(out, UiIcon::search, {q.right - 30, q.top + 9, q.right - 12, q.bottom - 9}, t.text_secondary);
    if (ok && s.content->query.bytes) {
        // A thin caret after the query hints that typing continues it.
        const float x = minimum(q.right - 40, q.left + 12 + ui_settings_text_width(s.content->query, 14) * 1.04f + 5);
        ok = paint_stream_push(out, x, q.top + 10, x + 1.5f, q.bottom - 10, t.text_primary);
    }
    ok = ok && paint_stream_push_clip(out, s.body.left, s.body.top, s.body.right, s.body.bottom);
    for (u32 i = 0; ok && i < s.row_count; ++i) {
        UiRect r{};
        if (!ui_shortcuts_scene_row(s, i, &r))
            continue;
        const auto& row = s.content->rows[i];
        if (row.header)
            continue;
        // Hairline between rows of a group.
        if (i + 1 < s.row_count && !s.content->rows[i + 1].header)
            ok = paint_stream_push(out, r.left, r.bottom - 1, r.right, r.bottom, ui_theme_color_alpha(t.surface_high, .6f));
        ok = ok && each_cap(row, r, [&](Utf8View key, UiRect cap) noexcept {
                 if (!key.bytes)
                     return true;
                 return ui_background_paint_surface(out, cap, t, cap_fill, cap_border, t.scale.radius_sm, 1, 1,
                                                    UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest) &&
                        paint_stream_push_rounded(out, cap.left + 1, cap.bottom - 3, cap.right - 1, cap.bottom - 1,
                                                  ui_theme_color_alpha(cap_border, .9f), t.scale.radius_xs);
             });
    }
    if (ok && s.content_height > ui_rect_height(s.body)) {
        // Overlay scrollbar thumb.
        const float track = ui_rect_height(s.body);
        const float thumb = maximum(32.f, track * track / s.content_height);
        const float top = s.body.top + (track - thumb) * (s.scroll / scroll_limit(s));
        ok = paint_stream_push_rounded(out, s.body.right - 4, top, s.body.right, top + thumb,
                                       ui_theme_color_alpha(t.text_secondary, .45f), t.scale.radius_xs);
    }
    return paint_stream_pop_clip(out) && paint_stream_pop_clip(out) && ok;
}
bool ui_shortcuts_scene_text_output(const UiShortcutsScene& s, const Theme& t, UiTextOutput* out) noexcept {
    if (!current(s))
        return true;
    if (!ui_text_output_valid(out))
        return false;
    const auto p = s.panel;
    const auto q = s.search;
    if (!emit(out, s, s.root, s.content->title.bytes ? s.content->title : utf8_literal("Keyboard shortcuts"),
              {p.left + 54, p.top + 14, p.right - 56, p.top + 46}, p, t.text_primary, text_style_bold) ||
        !emit(out, s, s.root, s.content->query.bytes ? s.content->query : s.content->placeholder,
              {q.left + 12, q.top, q.right - 40, q.bottom}, q,
              s.content->query.bytes ? t.text_primary : t.text_secondary))
        return false;
    if (!s.row_count)
        return emit(out, s, s.root, s.content->empty, {s.body.left, s.body.top + 24, s.body.right, s.body.top + 52},
                    s.body, t.text_secondary, 0, UiTextAlignment::center);
    for (u32 i = 0; i < s.row_count; ++i) {
        UiRect r{};
        if (!ui_shortcuts_scene_row(s, i, &r))
            continue;
        const auto& row = s.content->rows[i];
        if (row.header) {
            if (!emit(out, s, s.root, row.label, {r.left, r.top + 8, r.right, r.bottom}, s.body,
                      ui_foreground_color(t, t.tokens.emphasis.outline), text_style_bold | text_style_subtext))
                return false;
            continue;
        }
        const float label_right = maximum(r.left + 40, r.right - caps_width(row) - 12);
        const bool tall = ui_rect_height(r) > kRowHeight + .5f;
        if (!emit(out, s, s.root, row.label, {r.left, r.top + (tall ? 4.f : 0.f), label_right, r.bottom - (tall ? 4.f : 0.f)},
                  s.body, t.text_primary, 0, UiTextAlignment::start,
                  tall ? UiTextWrapping::wrap : UiTextWrapping::no_wrap) ||
            !each_cap(row, r, [&](Utf8View key, UiRect cap) noexcept {
                return key.bytes ? emit(out, s, s.root, key, {cap.left, cap.top, cap.right, cap.bottom - 2}, s.body,
                                        t.text_primary, text_style_bold | text_style_subtext, UiTextAlignment::center)
                                 : emit(out, s, s.root, utf8_literal("/"), cap, s.body, t.text_secondary,
                                        text_style_subtext, UiTextAlignment::center);
            }))
            return false;
    }
    return true;
}
} // namespace maicord
