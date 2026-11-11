#include "maicord/ui/voice/go_live_scene.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/ui/layout/rect_edges.hpp"
#include "maicord/ui/theme/foreground_color.hpp"

namespace maicord {
namespace {
constexpr float kInset = 24, kHeaderHeight = 64, kFooterHeight = 72, kUpsellHeight = 40, kPadding = 16, kGap = 16,
                kTabWidth = 136, kTabHeight = 36, kButton = 40, kSelectWidth = 148, kLabelHeight = 32,
                kTileMinimum = 200, kCardWidth = 208, kCardHeight = 132, kRadius = 8;
constexpr UiLook kClose{.fill = UI_TOKEN(surface_high), .border = UI_TOKEN(interactive), .focus = UI_TOKEN(interactive),
                        .radius = 6, .flags = ui_look_hot_only | ui_look_dialog};

bool initialize(UiGoLiveScene& s) noexcept {
    if (s.storage.arena.base)
        return true;
    if (!ui_scene_open(&s.storage, &s.widgets, kUiGoLiveWidgetCapacity))
        return false;
    s.root = ui_scene_part_root(&s.widgets, utf8_literal("Screen Share"));
    if (!s.root.generation)
        return false;
    const auto add = [&](Utf8View name) noexcept { return ui_scene_part(&s.widgets, s.root, UiWidgetKind::button, name); };
    s.close = add(utf8_literal("Close"));
    s.tabs[0] = add(utf8_literal("Applications"));
    s.tabs[1] = add(utf8_literal("Entire Screen"));
    s.kinds[0] = add(utf8_literal("Share entire screen"));
    s.kinds[1] = add(utf8_literal("Share a window"));
    s.grid = ui_scene_part(&s.widgets, s.root, UiWidgetKind::container, utf8_literal("Sources"));
    bool ok = s.close.generation && s.tabs[0].generation && s.tabs[1].generation && s.kinds[0].generation &&
              s.kinds[1].generation && s.grid.generation;
    for (u32 i = 0; ok && i < kUiGoLiveMaximumSources; ++i)
        ok = (s.tiles[i] = ui_scene_part(&s.widgets, s.grid, UiWidgetKind::button, utf8_literal("Source"))).generation;
    s.select = add(utf8_literal("Make Selection"));
    s.options = add(utf8_literal("Options"));
    s.upsell_button = add(utf8_literal("Stream in HD resolution with Nitro"));
    return ok && s.select.generation && s.options.generation && s.upsell_button.generation;
}
float scroll_limit(const UiGoLiveScene& s) noexcept {
    return maximum(0.f, s.content_height - ui_rect_height(s.body));
}
UiRect tile_absolute(const UiGoLiveScene& s, u32 index) noexcept {
    const auto r = s.tile_rectangles[index];
    return {s.body.left + r.left, s.body.top + r.top - s.scroll, s.body.left + r.right, s.body.top + r.bottom - s.scroll};
}
UiRect thumbnail_box(const UiRect& tile) noexcept {
    return {tile.left, tile.top, tile.right, tile.bottom - kLabelHeight};
}
// Parts sit at their rectangle: the root's children against the viewport, the tiles against the grid (the body).
bool place(UiGoLiveScene& s, UiWidgetHandle handle, const UiRect& r, bool shown, const UiRect* origin = nullptr) noexcept {
    return ui_widget_manager_place_part(&s.widgets, handle, shown ? r : UiRect{}, origin ? *origin : s.viewport, shown,
                                        shown);
}
// Places every control for the current mode and scroll; lays out when something moved.
bool layout(UiGoLiveScene& s, bool force) noexcept {
    bool dirty = force;
    const bool picker = s.mode == UiGoLiveMode::picker, system = s.mode == UiGoLiveMode::system_picker;
    dirty |= place(s, s.close, s.close_rectangle, true);
    for (u32 i = 0; i < 2; ++i) {
        dirty |= place(s, s.tabs[i], s.tab_rectangles[i], picker);
        dirty |= place(s, s.kinds[i], s.kind_rectangles[i], system);
    }
    // The grid only groups the tiles (a list for screen readers): it takes no input itself.
    dirty |= ui_widget_manager_place_part(&s.widgets, s.grid, picker ? s.body : UiRect{}, s.viewport, picker, false);
    for (u32 i = 0; i < kUiGoLiveMaximumSources; ++i) {
        const auto r = i < s.source_count ? tile_absolute(s, i) : UiRect{};
        dirty |= place(s, s.tiles[i], ui_rect_intersection(r, s.body),
                       picker && i < s.source_count && ui_rect_intersects(r, s.body), &s.body);
    }
    dirty |= place(s, s.select, s.select_rectangle, system && !s.busy);
    dirty |= place(s, s.options, s.options_rectangle, s.mode != UiGoLiveMode::unavailable);
    dirty |= place(s, s.upsell_button, s.upsell, s.nitro && s.mode != UiGoLiveMode::unavailable);
    if (dirty && !ui_widget_manager_layout_rectangle(&s.widgets, s.viewport).ok())
        return false;
    ui_widget_manager_reconcile_input(&s.widgets);
    return true;
}
void measure(UiGoLiveScene& s) noexcept {
    const auto v = s.viewport;
    const float width = minimum(860.f, ui_rect_width(v) - 2 * kInset);
    const float height = minimum(620.f, ui_rect_height(v) - 2 * kInset);
    const float left = v.left + (ui_rect_width(v) - width) * .5f, top = v.top + (ui_rect_height(v) - height) * .5f;
    s.panel = {left, top, left + width, top + height};
    const auto p = s.panel;
    s.header = {p.left, p.top, p.right, p.top + kHeaderHeight};
    const bool upsell = s.nitro && s.mode != UiGoLiveMode::unavailable;
    s.upsell = upsell ? UiRect{p.left, p.bottom - kUpsellHeight, p.right, p.bottom} : UiRect{};
    const float footer_bottom = upsell ? s.upsell.top : p.bottom;
    s.footer = {p.left + kPadding, footer_bottom - kFooterHeight, p.right - kPadding, footer_bottom};
    s.body = {p.left + kPadding, s.header.bottom, p.right - kPadding, s.footer.top - 8};
    s.close_rectangle = ui_rect_edges(p, ui_end(-48), ui_start(16), ui_end(-16), ui_start(48));
    const float mid_x = (p.left + p.right) * .5f, tab_top = p.top + (kHeaderHeight - kTabHeight) * .5f;
    s.tab_rectangles[0] = {mid_x - kTabWidth - 2, tab_top, mid_x - 2, tab_top + kTabHeight};
    s.tab_rectangles[1] = {mid_x + 2, tab_top, mid_x + kTabWidth + 2, tab_top + kTabHeight};
    const float middle = (s.footer.top + s.footer.bottom) * .5f;
    s.options_rectangle = {s.footer.right - kButton, middle - kButton * .5f, s.footer.right, middle + kButton * .5f};
    s.select_rectangle = {s.options_rectangle.left - 8 - kSelectWidth, s.options_rectangle.top,
                          s.options_rectangle.left - 8, s.options_rectangle.bottom};
    const float cards = 2 * kCardWidth + kGap, card_top = (s.body.top + s.body.bottom - kCardHeight) * .5f + 12;
    const float card_left = mid_x - cards * .5f;
    s.kind_rectangles[0] = {card_left, card_top, card_left + kCardWidth, card_top + kCardHeight};
    s.kind_rectangles[1] = {card_left + kCardWidth + kGap, card_top, card_left + cards, card_top + kCardHeight};
    // The grid: as many columns as fit tiles of at least kTileMinimum, at most four.
    const float body_width = ui_rect_width(s.body);
    s.columns = static_cast<u32>(maximum(1.f, minimum(4.f, (body_width + kGap) / (kTileMinimum + kGap))));
    const float tile_width = (body_width - static_cast<float>(s.columns - 1) * kGap) / static_cast<float>(s.columns);
    const float tile_height = tile_width / kUiGoLiveTileAspect + kLabelHeight;
    for (u32 i = 0; i < s.source_count; ++i) {
        const float x = static_cast<float>(i % s.columns) * (tile_width + kGap);
        const float y = static_cast<float>(i / s.columns) * (tile_height + kGap) + 8;
        s.tile_rectangles[i] = {x, y, x + tile_width, y + tile_height};
    }
    const u32 rows = (s.source_count + s.columns - 1) / s.columns;
    s.content_height = rows ? static_cast<float>(rows) * (tile_height + kGap) + 8 - kGap : 0;
    s.scroll = minimum(s.scroll, scroll_limit(s));
}
u32 focused_tile(const UiGoLiveScene& s) noexcept {
    for (u32 i = 0; i < s.source_count; ++i)
        if (ui_focused(s, s.tiles[i]))
            return i;
    return kUiGoLiveMaximumSources;
}
// Scrolls tile `index` fully into the body, then focuses it.
bool focus_tile(UiGoLiveScene& s, u32 index) noexcept {
    const auto r = s.tile_rectangles[index];
    const float view = ui_rect_height(s.body);
    if (r.top < s.scroll)
        s.scroll = maximum(0.f, r.top - 8);
    else if (r.bottom > s.scroll + view)
        s.scroll = minimum(scroll_limit(s), r.bottom - view + 8);
    if (!layout(s, false))
        return false;
    ui_widget_manager_focus(&s.widgets, s.tiles[index]);
    ++s.revision;
    return true;
}
} // namespace

bool ui_go_live_scene_update(UiGoLiveScene* s, const UiGoLiveContent& c, const UiRect& viewport) noexcept {
    if (!s || !ui_rect_valid(viewport) || ui_rect_width(viewport) < 360 || ui_rect_height(viewport) < 320 ||
        c.mode > UiGoLiveMode::unavailable || c.tab > UiGoLiveTab::screens || c.kind > UiGoLiveSystemKind::window ||
        c.sources.count > kUiGoLiveMaximumSources || !initialize(*s))
        return false;
    if (s->active && s->content_revision == c.revision && ui_rect_equal(s->viewport, viewport))
        return true;
    const bool reset = !s->active || s->mode != c.mode || s->tab != c.tab;
    if (!s->active)
        ++s->opens;
    const UiRect old_panel = s->panel;
    s->viewport = viewport;
    s->mode = c.mode;
    s->tab = c.tab;
    s->kind = c.kind;
    s->busy = c.busy;
    s->nitro = c.nitro;
    s->change_source = c.change_source;
    s->content_revision = c.revision;
    utf8_assign(s->title, s->title_bytes, c.title);
    utf8_assign(s->summary, s->summary_bytes, c.summary);
    utf8_assign(s->detail, s->detail_bytes, c.detail);
    utf8_assign(s->notice, s->notice_bytes, c.notice);
    utf8_assign(s->body_text, s->body_bytes, c.body);
    // A tile keeps its focus across refreshes while its source is still listed.
    const u32 focus = focused_tile(*s);
    const u64 focus_id = focus < s->source_count ? s->sources[focus].id : 0;
    s->source_count = c.mode == UiGoLiveMode::picker ? c.sources.count : 0;
    for (u32 i = 0; i < s->source_count; ++i) {
        const auto& source = c.sources[i];
        const u32 bytes = utf8_copy_truncated(s->names[i], source.name);
        s->sources[i] = source;
        s->sources[i].name = {s->names[i], bytes};
    }
    if (reset)
        s->scroll = 0;
    measure(*s);
    s->active = true;
    if (!ui_rect_equal(old_panel, s->panel))
        ui_widget_manager_release_pointer(&s->widgets);
    if (!layout(*s, true)) {
        s->active = false;
        return false;
    }
    u32 refocus = kUiGoLiveMaximumSources;
    for (u32 i = 0; focus_id && i < s->source_count; ++i)
        if (s->sources[i].id == focus_id)
            refocus = i;
    if (refocus < s->source_count)
        ui_widget_manager_focus(&s->widgets, s->tiles[refocus]);
    else if (reset || !s->widgets.focused.generation)
        ui_widget_manager_focus(&s->widgets, c.mode == UiGoLiveMode::picker      ? s->tabs[static_cast<u32>(c.tab)]
                                             : c.mode == UiGoLiveMode::system_picker ? s->kinds[0]
                                                                                     : s->close);
    ++s->revision;
    return true;
}

void ui_go_live_scene_release(UiGoLiveScene* s) noexcept {
    ui_scene_release(s);
}

UiGoLiveTarget ui_go_live_scene_target(const UiGoLiveScene& s, UiWidgetHandle handle) noexcept {
    if (!s.active || !ui_widget_manager_input_enabled(s.widgets, handle))
        return {};
    if (ui_widget_handle_matches(handle, s.close))
        return {UiGoLiveAction::close};
    for (u32 i = 0; i < 2; ++i) {
        if (ui_widget_handle_matches(handle, s.tabs[i]))
            return {UiGoLiveAction::tab, i};
        if (ui_widget_handle_matches(handle, s.kinds[i]))
            return {UiGoLiveAction::kind, i + 1};
    }
    for (u32 i = 0; i < s.source_count; ++i)
        if (ui_widget_handle_matches(handle, s.tiles[i]))
            return {UiGoLiveAction::source, i, s.sources[i].id};
    if (ui_widget_handle_matches(handle, s.select))
        return {UiGoLiveAction::select};
    if (ui_widget_handle_matches(handle, s.options))
        return {UiGoLiveAction::options};
    if (ui_widget_handle_matches(handle, s.upsell_button))
        return {UiGoLiveAction::nitro};
    return {};
}

UiGoLiveTarget ui_go_live_scene_hit_test(const UiGoLiveScene& s, float x, float y) noexcept {
    if (!s.active || !ui_rect_contains(s.viewport, x, y))
        return {};
    if (!ui_rect_contains(s.panel, x, y))
        return {UiGoLiveAction::close};
    return ui_go_live_scene_target(s, ui_widget_manager_hit_test(s.widgets, x, y));
}

bool ui_go_live_scene_set_hover(UiGoLiveScene* s, float x, float y, bool* changed) noexcept {
    if (changed)
        *changed = false;
    return s && s->active && ui_scene_hover(&s->widgets, &s->revision, x, y, changed);
}

bool ui_go_live_scene_scroll(UiGoLiveScene* s, float delta) noexcept {
    if (!s || !s->active || s->mode != UiGoLiveMode::picker)
        return false;
    const float next = minimum(scroll_limit(*s), maximum(0.f, s->scroll + delta));
    if (next == s->scroll)
        return false;
    s->scroll = next;
    if (!layout(*s, false))
        return false;
    ++s->revision;
    return true;
}

bool ui_go_live_scene_focus_next(UiGoLiveScene* s, bool reverse) noexcept {
    if (!s || !s->active)
        return false;
    const auto next = ui_widget_manager_next_focus(s->widgets, s->widgets.focused, reverse, true);
    if (!next.generation)
        return false;
    // Tabbing onto a tile outside the body scrolls it in (a hidden tile takes no focus, so start at the first).
    for (u32 i = 0; i < s->source_count; ++i)
        if (ui_widget_handle_matches(next, s->tiles[i]))
            return focus_tile(*s, i);
    ui_widget_manager_focus(&s->widgets, next);
    ++s->revision;
    return true;
}

bool ui_go_live_scene_focus_arrow(UiGoLiveScene* s, i32 dx, i32 dy) noexcept {
    if (!s || !s->active)
        return false;
    const auto focused = s->widgets.focused;
    for (u32 i = 0; i < 2; ++i) {
        const bool tab = ui_widget_handle_matches(focused, s->tabs[i]), kind = ui_widget_handle_matches(focused, s->kinds[i]);
        if ((tab || kind) && dx) {
            const u32 other = i ^ 1u;
            ui_widget_manager_focus(&s->widgets, tab ? s->tabs[other] : s->kinds[other]);
            ++s->revision;
            return true;
        }
        if (tab && dy > 0 && s->source_count)
            return focus_tile(*s, 0);
    }
    const u32 tile = focused_tile(*s);
    if (tile >= s->source_count)
        return false;
    const i32 columns = static_cast<i32>(s->columns);
    const i32 next = static_cast<i32>(tile) + dx + dy * columns;
    if (dy < 0 && next < 0) {
        ui_widget_manager_focus(&s->widgets, s->tabs[static_cast<u32>(s->tab)]);
        ++s->revision;
        return true;
    }
    if (next < 0 || next >= static_cast<i32>(s->source_count))
        return false;
    return focus_tile(*s, static_cast<u32>(next));
}

bool ui_go_live_scene_thumbnail(const UiGoLiveScene& s, u32 index, UiRect* picture, UiRect* clip, bool* dim) noexcept {
    if (!s.active || s.mode != UiGoLiveMode::picker || index >= s.source_count)
        return false;
    const auto& source = s.sources[index];
    const auto box = thumbnail_box(tile_absolute(s, index));
    const auto visible = ui_rect_intersection(box, s.body);
    if (!ui_rect_has_area(visible) || !source.width || !source.height || !media_asset_key_valid(source.thumbnail))
        return false;
    // Letterboxed: the whole picture, centred in the 16:9 box.
    const float scale = minimum(ui_rect_width(box) / source.width, ui_rect_height(box) / source.height);
    const float w = source.width * scale, h = source.height * scale;
    const float x = (box.left + box.right - w) * .5f, y = (box.top + box.bottom - h) * .5f;
    *picture = {x, y, x + w, y + h};
    *clip = visible;
    // Hovered or focused: dimmed under the "Share Screen" call to action.
    if (dim)
        *dim = ui_widget_state(s.widgets, s.tiles[index]) != 0;
    return true;
}

bool ui_go_live_scene_paint(const UiGoLiveScene& s, const Theme& t, PaintStream* out) noexcept {
    if (!s.active)
        return true;
    UiPaint p{out, t};
    p.dialog(s.viewport, s.panel)
        .look<kClose>(s.close_rectangle, ui_widget_state(s.widgets, s.close))
        .icon(UiIcon::close, ui_rect_shrink(s.close_rectangle, 7), t.text_secondary);
    const Color muted = ui_foreground_color(t, t.text_secondary);
    if (s.mode == UiGoLiveMode::picker) {
        // Pill tabs: the selected one filled, the others a plate while hot.
        for (u32 i = 0; i < 2; ++i)
            p.look<kUiTab>(s.tab_rectangles[i], static_cast<u8>(ui_widget_state(s.widgets, s.tabs[i]) |
                                                                (static_cast<u32>(s.tab) == i ? ui_state_selected : 0)));
        p.clip(s.body);
        for (u32 i = 0; p && i < s.source_count; ++i) {
            const auto tile = tile_absolute(s, i);
            if (!ui_rect_intersects(tile, s.body))
                continue;
            const auto box = thumbnail_box(tile);
            const u8 state = ui_widget_state(s.widgets, s.tiles[i]);
            p.rounded(box, t.base_lowest, kRadius);
            if (!s.sources[i].width)
                p.icon(s.sources[i].screen ? UiIcon::monitor : UiIcon::screen_share,
                       {(box.left + box.right) * .5f - 16, (box.top + box.bottom) * .5f - 16,
                        (box.left + box.right) * .5f + 16, (box.top + box.bottom) * .5f + 16},
                       muted);
            // The focus ring sits just outside the picture, which the host draws over the plate.
            if (state & ui_state_focused)
                p.rounded_border({box.left - 3, box.top - 3, box.right + 3, box.bottom + 3}, Color{},
                                 ui_foreground_color(t, t.tokens.border.focus), kRadius + 3, 2);
            const UiRect label{tile.left, box.bottom, tile.right, tile.bottom};
            p.icon(s.sources[i].screen ? UiIcon::monitor : UiIcon::screen_share,
                   {label.left + 2, label.top + 8, label.left + 18, label.top + 24}, state ? t.text_primary : muted);
        }
        p.unclip();
    } else if (s.mode == UiGoLiveMode::system_picker) {
        for (u32 i = 0; i < 2; ++i) {
            const auto r = s.kind_rectangles[i];
            const bool chosen = static_cast<u32>(s.kind) == i + 1;
            const u8 state = static_cast<u8>(ui_widget_state(s.widgets, s.kinds[i]) | (chosen ? ui_state_selected : 0));
            p.surface(r, (state & (ui_state_hovered | ui_state_selected)) ? t.surface_high : t.base_lowest,
                      (state & ui_state_focused) ? ui_foreground_color(t, t.tokens.border.focus)
                      : chosen                  ? ui_foreground_color(t, t.tokens.emphasis.outline)
                                                : t.interactive,
                      kRadius, true)
                .icon(i == 0 ? UiIcon::monitor : UiIcon::screen_share,
                      {(r.left + r.right) * .5f - 18, r.top + 28, (r.left + r.right) * .5f + 18, r.top + 64},
                      chosen ? ui_foreground_color(t, t.tokens.emphasis.outline) : t.text_primary);
        }
        if (!s.busy) {
            const u8 state = ui_widget_state(s.widgets, s.select);
            const bool enabled = s.kind != UiGoLiveSystemKind::none;
            Color fill = t.tokens.emphasis.fill;
            if (!enabled)
                fill.alpha *= .5f;
            p.surface(s.select_rectangle, fill,
                      (state & ui_state_focused) ? ui_foreground_color(t, t.tokens.border.focus) : fill, 6, true);
            if (enabled && (state & ui_state_hovered))
                p.rounded(s.select_rectangle, t.tokens.state.hover, 6);
        }
    } else {
        const float mid = (s.body.left + s.body.right) * .5f, top = s.body.top + ui_rect_height(s.body) * .28f;
        p.icon(UiIcon::screen_share_off, {mid - 24, top - 48, mid + 24, top}, muted);
    }
    if (s.mode != UiGoLiveMode::unavailable) {
        p.rule_above(s.footer);
        const u8 state = ui_widget_state(s.widgets, s.options);
        p.surface(s.options_rectangle, (state & ui_state_hovered) ? t.surface_high : t.base_low,
                  (state & ui_state_focused) ? ui_foreground_color(t, t.tokens.border.focus) : t.interactive, 6, true)
            .icon(UiIcon::settings, ui_rect_shrink(s.options_rectangle, 10), t.text_primary);
    }
    if (ui_rect_has_area(s.upsell)) {
        const u8 state = ui_widget_state(s.widgets, s.upsell_button);
        p.fill(s.upsell, (state & ui_state_hovered) ? t.surface_high : t.base_lowest);
        if (state & ui_state_focused)
            p.rounded_border(ui_rect_shrink(s.upsell, 2), Color{}, ui_foreground_color(t, t.tokens.border.focus), 4, 2);
        const float y = (s.upsell.top + s.upsell.bottom) * .5f;
        p.icon(UiIcon::lock, {s.upsell.left + kPadding, y - 7, s.upsell.left + kPadding + 14, y + 7}, muted);
    }
    return p.end();
}

bool ui_go_live_scene_text_output(const UiGoLiveScene& s, const Theme& t, UiTextOutput* out) noexcept {
    if (!s.active)
        return true;
    UiTextPen<UiTextClip::region> text{out, s.widgets, s.panel};
    const Color muted = t.text_secondary;
    if (s.mode == UiGoLiveMode::picker) {
        for (u32 i = 0; i < 2; ++i)
            text(s.tabs[i], i ? utf8_literal("Entire Screen") : utf8_literal("Applications"), s.tab_rectangles[i],
                 static_cast<u32>(s.tab) == i ? t.text_primary : muted, text_style_bold | text_style_subtext,
                 ui_text_center | ui_text_middle);
        for (u32 i = 0; text.ok() && i < s.source_count; ++i) {
            const auto tile = tile_absolute(s, i);
            if (!ui_rect_intersects(tile, s.body))
                continue;
            const auto box = thumbnail_box(tile);
            const UiRect label{tile.left + 24, box.bottom, tile.right, tile.bottom};
            const bool hot = ui_widget_state(s.widgets, s.tiles[i]) != 0;
            text.in(s.body)(s.tiles[i], s.sources[i].name, label, hot ? t.text_primary : muted, text_style_subtext,
                            ui_text_middle);
            // The official client's hover call to action over the thumbnail.
            if (hot && s.sources[i].width)
                text.in(ui_rect_intersection(box, s.body))(s.tiles[i],
                                                          s.change_source ? utf8_literal("Change Stream")
                                                                          : utf8_literal("Share Screen"),
                                                          box, t.text_primary, text_style_bold,
                                                          ui_text_center | ui_text_middle);
        }
        if (!s.source_count)
            text(s.root,
                 s.tab == UiGoLiveTab::applications ? utf8_literal("No applications to share")
                                                    : utf8_literal("No screens to share"),
                 s.body, muted, 0, ui_text_center | ui_text_middle);
    } else {
        text(s.root, {s.title, s.title_bytes},
             s.mode == UiGoLiveMode::system_picker
                 ? UiRect{s.panel.left + 56, s.panel.top, s.panel.right - 56, s.header.bottom}
                 : UiRect{s.body.left, s.body.top + ui_rect_height(s.body) * .28f + 12, s.body.right,
                          s.body.top + ui_rect_height(s.body) * .28f + 44},
             t.text_primary, text_style_bold, ui_text_center | ui_text_middle);
    }
    if (s.mode == UiGoLiveMode::system_picker) {
        for (u32 i = 0; i < 2; ++i) {
            const auto r = s.kind_rectangles[i];
            text(s.kinds[i], i ? utf8_literal("Share a window") : utf8_literal("Share entire screen"),
                 {r.left + 8, r.top + 76, r.right - 8, r.bottom - 16}, t.text_primary,
                 text_style_subtext | text_style_bold, ui_text_center | ui_text_middle);
        }
        text(s.root,
             utf8_literal("Click the Make Selection button, then choose a window, application, or screen to Go Live "
                          "using your system menu."),
             {s.body.left + 48, s.body.top + 8, s.body.right - 48, s.kind_rectangles[0].top - 8}, muted,
             text_style_subtext, ui_text_center | ui_text_wrap | ui_text_middle);
        if (!s.busy) {
            const bool enabled = s.kind != UiGoLiveSystemKind::none;
            Color ink = ui_surface_foreground(t, t.tokens.emphasis.fill);
            if (!enabled)
                ink.alpha *= .6f;
            text(s.select, utf8_literal("Make Selection"), s.select_rectangle, ink, text_style_bold | text_style_subtext,
                 ui_text_center | ui_text_middle);
        } else
            text(s.root, utf8_literal("Waiting for your selection\xE2\x80\xA6"),
                 {s.footer.left, s.footer.top, s.options_rectangle.left - 12, s.footer.bottom}, muted,
                 text_style_subtext, ui_text_end | ui_text_middle);
    }
    if (s.mode == UiGoLiveMode::unavailable) {
        const float top = s.body.top + ui_rect_height(s.body) * .28f + 48;
        return text(s.root, {s.body_text, s.body_bytes}, {s.body.left + 48, top, s.body.right - 48, s.body.bottom},
                    muted, text_style_subtext, ui_text_center | ui_text_wrap)
            .ok();
    }
    const float middle = (s.footer.top + s.footer.bottom) * .5f;
    const float right = (s.mode == UiGoLiveMode::system_picker ? s.select_rectangle.left : s.options_rectangle.left) - 12;
    if (s.notice_bytes)
        text(s.root, {s.notice, s.notice_bytes}, {s.footer.left, s.footer.top + 8, right, middle}, t.tokens.text.danger,
             text_style_subtext | text_style_bold, ui_text_middle);
    else
        text(s.root, {s.summary, s.summary_bytes}, {s.footer.left, s.footer.top + 8, right, middle}, t.text_primary,
             text_style_bold, ui_text_middle);
    text(s.root, {s.detail, s.detail_bytes}, {s.footer.left, middle, right, s.footer.bottom - 8}, muted,
         text_style_subtext, ui_text_middle);
    if (ui_rect_has_area(s.upsell))
        text(s.upsell_button, utf8_literal("Stream in HD resolution with Nitro"),
             {s.upsell.left + kPadding + 22, s.upsell.top, s.upsell.right - kPadding, s.upsell.bottom}, muted,
             text_style_subtext, ui_text_middle);
    return text.ok();
}
} // namespace maicord
