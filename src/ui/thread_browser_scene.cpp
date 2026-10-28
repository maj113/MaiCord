#include "maicord/ui/thread_browser_scene.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/rect_edges.hpp"
#include "maicord/ui/scene_text.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/spinner.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool current(const UiThreadBrowserScene& s) noexcept {
    return s.active && s.backing && s.view && s.view->source_publication == s.source_publication &&
        s.view->revision == s.content_revision && s.view->rows.data == s.observed_rows &&
        s.view->rows.count == s.count && s.view->tab == s.tab && s.view->page_index == s.page_index;
}
bool eligible(const UiThreadBrowserScene& s, UiThreadBrowserTarget value) noexcept {
    if (!current(s) || value.source_publication != s.source_publication || value.action == UiThreadBrowserAction::none)
        return false;
    const auto& view = *s.view;
    switch (value.action) {
    case UiThreadBrowserAction::close:
    case UiThreadBrowserAction::active:
    case UiThreadBrowserAction::archived: return !value.thread_id;
    case UiThreadBrowserAction::create_post: return !value.thread_id && view.can_create_post;
    case UiThreadBrowserAction::previous: return !value.thread_id && view.has_previous && !view.loading && !view.error;
    case UiThreadBrowserAction::next: return !value.thread_id && view.has_next && !view.loading && !view.error;
    case UiThreadBrowserAction::retry: return !value.thread_id && view.error && !view.loading;
    case UiThreadBrowserAction::open_thread:
        if (!value.thread_id || view.loading || view.error) return false;
        for (u32 i = 0; i < view.rows.count; ++i) if (view.rows[i].thread_id == value.thread_id) return true;
        return false;
    case UiThreadBrowserAction::forum_sort:
    case UiThreadBrowserAction::forum_list:
    case UiThreadBrowserAction::forum_gallery: return !value.thread_id && view.forum;
    case UiThreadBrowserAction::forum_tag:
        if (!value.thread_id || !view.forum) return false;
        for (u32 i = 0; i < view.forum->tags.count; ++i) if (view.forum->tags[i].id == value.thread_id) return true;
        return false;
    default: return false;
    }
}
UiThreadBrowserTarget target(const UiThreadBrowserScene& s, UiThreadBrowserAction action, u64 id = 0) noexcept {
    return {action, id, s.source_publication};
}
bool semantic_equal(UiThreadBrowserTarget a, UiThreadBrowserTarget b) noexcept {
    return a.action == b.action && a.thread_id == b.thread_id;
}
UiRect bounds(const UiThreadBrowserScene& s, UiWidgetHandle handle) noexcept {
    const auto* node = ui_widget_manager_layout_node(s.widgets, handle);
    return node ? node->rectangle : UiRect{};
}
bool selected(const UiThreadBrowserScene& s, UiWidgetHandle handle) noexcept {
    return ui_widget_handle_matches(handle, s.widgets.focused);
}
bool hovered(const UiThreadBrowserScene& s, UiWidgetHandle handle) noexcept {
    return ui_widget_handle_matches(handle, s.widgets.hovered);
}
UiWidgetHandle add(UiThreadBrowserScene& s, const UiRect& r, UiThreadBrowserAction action,
    const Utf8View& label, UiWidgetHandle parent, u64 id = 0) noexcept {
    if (!ui_rect_has_area(r)) return {};
    const auto origin = ui_widget_handle_matches(s.body_owner, parent) ? s.body : s.panel;
    const auto value = target(s, action, id);
    const bool enabled = eligible(s, value);
    const auto result = ui_widget_manager_add_placed(&s.widgets, parent,
        action == UiThreadBrowserAction::none ? UiWidgetKind::container : UiWidgetKind::button,
        static_cast<u16>(enabled ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable : 0), label, r,
        origin);
    if (!result.ok()) return {};
    s.backing->targets[result.handle.slot] = value;
    s.backing->targets[result.handle.slot].widget = result.handle;
    return result.handle;
}
// Forum cards share the plain rows' horizontal extent (inset, scrollbar gutter).
UiRect card_area(const UiThreadBrowserScene& s) noexcept {
    return {s.panel.left + s.style.inset, s.body.top, s.panel.right - s.style.inset - 10, s.body.bottom};
}
UiForumGrid forum_grid(const UiThreadBrowserScene& s) noexcept {
    return ui_forum_grid(ui_rect_width(card_area(s)), s.view->forum->layout, s.forum_style);
}
float content_height(const UiThreadBrowserScene& s) noexcept {
    return s.forum && s.view && s.view->forum ? ui_forum_content_height(forum_grid(s), s.count)
        : s.count * s.style.row_height;
}
UiRect control(const UiThreadBrowserScene& s, UiRect band, float left, float right) noexcept {
    const float top = band.top + (ui_rect_height(band) - s.style.button_height) * .5f;
    return {left, top, right, top + s.style.button_height};
}
void append(UiThreadBrowserScene& s, Utf8View text) noexcept {
    if (s.page_bytes + text.bytes > sizeof(s.page_text)) return;
    copy_bytes(s.page_text + s.page_bytes, text.data, text.bytes);
    s.page_bytes = static_cast<u8>(s.page_bytes + text.bytes);
}
void number(UiThreadBrowserScene& s, u64 value) noexcept {
    // Page labels stay far below the 64-byte buffer (u32 page and total, fixed text).
    s.page_bytes = static_cast<u8>(s.page_bytes + decimal_write(s.page_text + s.page_bytes, value));
}
bool rebuild(UiThreadBrowserScene& s) noexcept {
    ui_widget_manager_reset(&s.widgets);
    clear_bytes(s.backing, sizeof(*s.backing));
    reset_value(&s.body_owner);
    reset_value(&s.retry);
    reset_value(&s.create_post);
    s.forum_sort = s.forum_list = s.forum_gallery = {};
    auto layout = ui_layout_fill_style(ui_layout_node_visible | ui_layout_node_allow_overlap |
                                       ui_layout_node_clip_children);
    const auto root = ui_widget_manager_add_prevalidated(&s.widgets, ui_widget_invalid_handle(),
        {1, UiWidgetKind::container, ui_widget_none, {utf8_literal("Thread browser"), layout}});
    if (!root.ok()) return false;
    s.root = root.handle;
    const float left = s.panel.left + s.style.inset, right = s.panel.right - s.style.inset;
    s.close = add(s, control(s, s.header, right - s.style.button_height, right),
        UiThreadBrowserAction::close, utf8_literal("Close thread browser"), s.root);
    if (s.view->can_create_post) {
        const float end = right - s.style.button_height - s.style.gap;
        s.create_post = add(s, control(s, s.header, maximum(left, end - s.style.create_button_width), end),
            UiThreadBrowserAction::create_post, s.forum ? utf8_literal("New post") : utf8_literal("Create post"), s.root);
        if (!s.create_post.generation) return false;
    }
    const float tab_width = (right - left - s.style.gap) * .5f;
    s.active_tab = add(s, control(s, s.tabs, left, left + tab_width),
        UiThreadBrowserAction::active, utf8_literal("Active threads"), s.root);
    s.archived_tab = add(s, control(s, s.tabs, left + tab_width + s.style.gap, right),
        UiThreadBrowserAction::archived, utf8_literal("Archived threads"), s.root);
    if (s.forum) {
        const auto& forum = *s.view->forum;
        auto& toolbar = s.backing->toolbar;
        ui_forum_toolbar_layout({left, s.toolbar.top, right, s.toolbar.bottom}, forum, s.forum_style, &toolbar);
        s.backing->grid = forum_grid(s);
        s.forum_sort = add(s, toolbar.sort, UiThreadBrowserAction::forum_sort, utf8_literal("Sort by"), s.root);
        s.forum_list = add(s, toolbar.list, UiThreadBrowserAction::forum_list, utf8_literal("List"), s.root);
        s.forum_gallery = add(s, toolbar.gallery, UiThreadBrowserAction::forum_gallery, utf8_literal("Gallery"), s.root);
        if (!s.forum_sort.generation || !s.forum_list.generation || !s.forum_gallery.generation) return false;
        for (u32 i = 0; i < toolbar.chip_count; ++i) {
            const auto chip = ui_rect_intersection(toolbar.chips[i], s.toolbar);
            if (!ui_rect_has_area(chip)) continue;
            s.backing->chips[i] = add(s, chip, UiThreadBrowserAction::forum_tag, forum.tags[i].name, s.root,
                forum.tags[i].id);
            if (!s.backing->chips[i].generation) return false;
        }
        for (u32 i = 0; i < s.count; ++i) {
            const u8 bytes = static_cast<u8>(decimal_write(s.backing->replies[i], forum.posts[i].reply_count));
            s.backing->reply_bytes[i] = bytes;
        }
    }
    s.body_owner = add(s, s.body, UiThreadBrowserAction::none,
        s.forum ? utf8_literal("Post list viewport") : utf8_literal("Thread list viewport"), s.root);
    for (u32 i = 0; i < s.count; ++i) {
        const float top = s.body.top + i * s.style.row_height - s.scroll_dips;
        const UiRect card = s.forum ? ui_forum_card_rectangle(card_area(s), s.backing->grid, i, s.scroll_dips)
            : UiRect{left, top, right - 10, top + s.style.row_height};
        s.backing->cards[i] = card;
        const auto clip = ui_rect_intersection(card, s.body);
        if (!ui_rect_has_area(clip)) continue;
        const auto& row = s.view->rows[i];
        s.backing->rows[i] = add(s, clip, UiThreadBrowserAction::open_thread,
            row.name.bytes ? row.name : s.forum ? utf8_literal("Untitled post") : utf8_literal("Unnamed thread"),
            s.body_owner, row.thread_id);
        if (!s.backing->rows[i].generation) return false;
    }
    const float button_width = minimum(84.f, (right - left) * .32f);
    s.previous = add(s, control(s, s.footer, left, left + button_width),
        UiThreadBrowserAction::previous, utf8_literal("Previous page"), s.root);
    s.next = add(s, control(s, s.footer, right - button_width, right),
        UiThreadBrowserAction::next, utf8_literal("Next page"), s.root);
    if (s.view->error) {
        const float retry_width = minimum(72.f, ui_rect_width(s.status) * .4f);
        s.retry = add(s, control(s, s.status, right - retry_width, right),
            UiThreadBrowserAction::retry, utf8_literal("Retry loading threads"), s.root);
    }
    if (!s.close.generation || !s.active_tab.generation || !s.archived_tab.generation ||
        !s.body_owner.generation || !s.previous.generation || !s.next.generation ||
        (s.view->error && !s.retry.generation)) return false;
    if (!ui_widget_manager_layout_rectangle(&s.widgets, s.panel).ok()) return false;
    for (u32 i = 1; i < s.widgets.count; ++i) {
        const UiWidgetHandle handle{static_cast<u16>(i), s.widgets.generation};
        if (semantic_equal(s.backing->targets[i], s.focused) && ui_widget_manager_focus(&s.widgets, handle)) {
            s.focused = s.backing->targets[i];
            break;
        }
    }
    ++s.revision;
    ++s.rebuilds;
    ++s.layout_passes;
    return true;
}
bool text(UiTextOutput* out, const UiThreadBrowserScene& s, UiWidgetHandle owner, const Utf8View& value,
    const UiRect& r, const Color& color, u32 flags = 0, u32 mode = 0) noexcept {
    return ui_scene_text(out, s.widgets, owner, value, r, (mode & ui_text_in_body) ? s.body : s.panel, color, flags, mode);
}
bool surface(PaintStream* out, const UiRect& r, const Theme& t, const Color& fill, const Color& border, float radius) noexcept {
    return ui_background_paint_surface(out, r, t, fill, border, radius, 1, .9f,
        UiBackgroundSurfaceMode::gradient, UiBackgroundTier::high);
}
bool button(PaintStream* out, const UiThreadBrowserScene& s, const Theme& t, UiWidgetHandle handle,
    bool tab = false) noexcept {
    if (!handle.generation) return true;
    // The current tab carries the accent selection tint (as selected pills
    // and checked menu rows do), so it reads as chosen rather than as a well.
    return surface(out, bounds(s, handle), t,
        tab ? ui_emphasis_selected(t, UiElevation::popover) : hovered(s, handle) ? t.surface_high : t.surface,
        selected(s, handle) ? t.tokens.border.focus : tab ? t.tokens.emphasis.outline : t.surface_high, s.style.radius);
}
Utf8View state_text(const UiThreadBrowserScene& s) noexcept {
    if (s.view->loading) return utf8_literal("Loading threads…");
    if (s.view->error) return utf8_literal("Couldn't load threads");
    if (s.tab == UiThreadBrowserTab::active && !s.view->inventory_synced)
        return utf8_literal("Loading threads…");
    return s.tab == UiThreadBrowserTab::search ? utf8_literal("No matching threads")
        : s.tab == UiThreadBrowserTab::archived ? utf8_literal("No archived threads on this page")
        : utf8_literal("No active threads");
}
} // namespace

bool ui_thread_browser_style_valid(const UiThreadBrowserStyle& s) noexcept {
    const float dimensions[]{s.header_height, s.tabs_height, s.status_height,
        s.footer_height, s.row_height, s.inset, s.gap, s.button_height, s.radius, s.create_button_width};
    for (float value : dimensions) if (!ui_extent_valid(value)) return false;
    return s.header_height >= 44 && s.header_height <= 80 && s.tabs_height >= 36 && s.tabs_height <= 64 &&
        s.status_height >= 32 && s.status_height <= 64 &&
        s.footer_height >= 36 && s.footer_height <= 64 && s.row_height >= 54 && s.row_height <= 96 &&
        s.inset >= 8 && s.inset <= 24 && s.gap >= 4 && s.gap <= 16 && s.button_height >= 28 &&
        s.button_height <= minimum(s.header_height, minimum(s.tabs_height,
        minimum(s.status_height, s.footer_height))) && s.radius <= 16 &&
        s.create_button_width >= 96 && s.create_button_width <= 160;
}
bool ui_thread_browser_scene_current(const UiThreadBrowserScene& s) noexcept { return current(s); }
bool ui_thread_browser_scene_spinner_visible(const UiThreadBrowserScene& s) noexcept {
    return current(s) && !s.count && s.view->loading;
}
bool ui_thread_browser_scene_update(UiThreadBrowserScene* s, const UiThreadBrowserView* view,
    UiRect viewport, const UiThreadBrowserStyle* style) noexcept {
    const auto appearance = style ? *style : kDefaultUiThreadBrowserStyle;
    if (!s || !view || !view->source_publication || view->rows.count > kUiThreadBrowserPageCapacity ||
        (view->rows.count && !view->rows.data) || view->tab > UiThreadBrowserTab::search ||
        !utf8_validate(view->title).valid || !utf8_validate(view->status).valid ||
        !ui_thread_browser_style_valid(appearance) || !ui_rect_valid(viewport) ||
        ui_rect_width(viewport) < 180 || ui_rect_height(viewport) < appearance.header_height +
            appearance.tabs_height + appearance.status_height + appearance.footer_height + 24) return false;
    if (current(*s) && s->view == view && ui_rect_equal(s->viewport, viewport) &&
        bytes_equal(&s->style, &appearance, sizeof(appearance))) return true;
    for (u32 i = 0; i < view->rows.count; ++i) {
        const auto& row = view->rows[i];
        if (!row.thread_id || !utf8_validate(row.name).valid || (row.type != 10 && row.type != 11 && row.type != 12))
            return false;
        for (u32 j = 0; j < i; ++j) if (view->rows[j].thread_id == row.thread_id) return false;
        if (view->forum && (view->forum->posts.count != view->rows.count ||
            view->forum->posts[i].thread_id != row.thread_id)) return false;
    }
    if (view->forum && (!ui_forum_view_valid(*view->forum) || view->forum->posts.count != view->rows.count))
        return false;
    if (!s->storage.arena.base) {
        if (!ui_fixed_widget_storage_initialize(&s->storage, kUiThreadBrowserWidgetCapacity,
            sizeof(UiThreadBrowserBacking), alignof(UiThreadBrowserBacking))) return false;
        if (!ui_widget_manager_initialize(&s->widgets, s->storage.widgets, s->storage.layout_nodes)) {
            ui_fixed_widget_storage_release(&s->storage);
            return false;
        }
        s->backing = ui_fixed_widget_storage_sidecar<UiThreadBrowserBacking>(s->storage);
    }
    const bool same_page = s->active && s->source_publication == view->source_publication &&
        s->tab == view->tab && s->page_index == view->page_index;
    if (!same_page) {
        s->scroll_dips = 0;
        // Borrowed results never transfer row focus to a new account/parent/page.
        if (s->focused.action == UiThreadBrowserAction::open_thread) reset_value(&s->focused);
    }
    s->view = view;
    s->observed_rows = view->rows.data;
    s->source_publication = view->source_publication;
    s->content_revision = view->revision;
    s->page_index = view->page_index;
    s->count = static_cast<u32>(view->rows.count);
    s->tab = view->tab;
    s->style = appearance;
    s->viewport = s->panel = viewport;
    s->header = {viewport.left, viewport.top, viewport.right, viewport.top + appearance.header_height};
    s->tabs = {viewport.left, s->header.bottom, viewport.right, s->header.bottom + appearance.tabs_height};
    s->status = {viewport.left, s->tabs.bottom, viewport.right, s->tabs.bottom + appearance.status_height};
    s->footer = {viewport.left, viewport.bottom - appearance.footer_height, viewport.right, viewport.bottom};
    s->forum = view->forum != nullptr;
    s->forum_style = kDefaultUiForumStyle;
    float toolbar_height = 0;
    if (s->forum) {
        UiForumToolbar toolbar{};
        ui_forum_toolbar_layout({viewport.left + appearance.inset, s->status.bottom, viewport.right - appearance.inset,
            s->status.bottom}, *view->forum, s->forum_style, &toolbar);
        // The band never takes the last 24 DIPs of the body; overflowing chip rows clip.
        toolbar_height = minimum(toolbar.height, maximum(0.f, s->footer.top - s->status.bottom - 24));
    }
    s->toolbar = {viewport.left, s->status.bottom, viewport.right, s->status.bottom + toolbar_height};
    s->body = {viewport.left, s->toolbar.bottom, viewport.right, s->footer.top};
    s->scroll_dips = minimum(maximum(0.f, s->scroll_dips), ui_thread_browser_scene_scroll_maximum(*s));
    s->page_bytes = 0;
    append(*s, utf8_literal("Page "));
    number(*s, static_cast<u64>(s->page_index) + 1);
    if (view->tab != UiThreadBrowserTab::archived) {
        append(*s, utf8_literal(" · "));
        number(*s, view->total);
        append(*s, view->total == 1 ? utf8_literal(" thread") : utf8_literal(" threads"));
    }
    s->active = true;
    if (!rebuild(*s)) { s->active = false; return false; }
    return true;
}
void ui_thread_browser_scene_release(UiThreadBrowserScene* s) noexcept {
    if (!s) return;
    ui_fixed_widget_storage_release(&s->storage);
    reset_value(s);
}
bool ui_thread_browser_scene_contains(const UiThreadBrowserScene& s, float x, float y) noexcept {
    return current(s) && ui_rect_contains(s.panel, x, y);
}
bool ui_thread_browser_scene_target_current(const UiThreadBrowserScene& s, UiThreadBrowserTarget value) noexcept {
    return eligible(s, value) && value.widget.generation && value.widget.slot < kUiThreadBrowserWidgetCapacity &&
        ui_widget_manager_input_enabled(s.widgets, value.widget) &&
        ui_thread_browser_target_equal(s.backing->targets[value.widget.slot], value);
}
UiThreadBrowserTarget ui_thread_browser_scene_target(const UiThreadBrowserScene& s, UiWidgetHandle handle) noexcept {
    if (!current(s) || handle.slot >= kUiThreadBrowserWidgetCapacity ||
        !ui_widget_manager_input_enabled(s.widgets, handle)) return {};
    const auto value = s.backing->targets[handle.slot];
    return ui_thread_browser_scene_target_current(s, value) ? value : UiThreadBrowserTarget{};
}
UiThreadBrowserTarget ui_thread_browser_scene_hit_test(const UiThreadBrowserScene& s, float x, float y) noexcept {
    if (!current(s)) return {};
    return ui_thread_browser_scene_target(s, ui_widget_manager_hit_test(s.widgets, x, y));
}
UiThreadBrowserTarget ui_thread_browser_scene_focused_target(const UiThreadBrowserScene& s) noexcept {
    return ui_thread_browser_scene_target(s, s.widgets.focused);
}
bool ui_thread_browser_scene_set_hover(UiThreadBrowserScene* s, float x, float y, bool* changed) noexcept {
    if (!s || !changed || !current(*s)) return false;
    const auto handle = ui_widget_manager_hit_test(s->widgets, x, y);
    *changed = handle.slot != s->widgets.hovered.slot || handle.generation != s->widgets.hovered.generation;
    if (!ui_widget_manager_hover(&s->widgets, handle)) return false;
    if (*changed) ++s->revision;
    return true;
}
float ui_thread_browser_scene_scroll_maximum(const UiThreadBrowserScene& s) noexcept {
    return maximum(0.f, content_height(s) - ui_rect_height(s.body));
}
bool ui_thread_browser_scene_scroll(UiThreadBrowserScene* s, float offset) noexcept {
    if (!s || !current(*s) || !ui_coordinate_valid(offset)) return false;
    const float next = minimum(maximum(0.f, offset), ui_thread_browser_scene_scroll_maximum(*s));
    if (next == s->scroll_dips) return true;
    s->scroll_dips = next;
    return rebuild(*s);
}
bool ui_thread_browser_scene_focus(UiThreadBrowserScene* s, UiThreadBrowserTarget value) noexcept {
    if (!s || !eligible(*s, value) ||
        (value.widget.generation && !ui_thread_browser_scene_target_current(*s, value))) return false;
    if (value.action == UiThreadBrowserAction::open_thread) {
        for (u32 i = 0; i < s->count; ++i) {
            if (s->view->rows[i].thread_id != value.thread_id) continue;
            const auto grid = s->forum ? forum_grid(*s) : UiForumGrid{1, 0, s->style.row_height, 0};
            const float extent = grid.card_height;
            const float first = static_cast<float>(i / grid.columns) * (grid.card_height + grid.gap);
            const float last = first + extent;
            const float height = ui_rect_height(s->body);
            const float offset = first < s->scroll_dips ? first
                : last > s->scroll_dips + height ? (extent > height ? first : last - height) : s->scroll_dips;
            if (!ui_thread_browser_scene_scroll(s, offset)) return false;
            break;
        }
    }
    for (u32 i = 1; i < s->widgets.count; ++i) {
        const UiWidgetHandle handle{static_cast<u16>(i), s->widgets.generation};
        if (semantic_equal(s->backing->targets[i], value) && ui_widget_manager_focus(&s->widgets, handle)) {
            s->focused = s->backing->targets[i];
            ++s->revision;
            return true;
        }
    }
    return false;
}
bool ui_thread_browser_scene_move_focus(UiThreadBrowserScene* s, bool reverse) noexcept {
    if (!s || !current(*s)) return false;
    // Close, Create post, both tabs, Retry and both page buttons, plus the
    // forum controls (sort, List, Gallery, tag chips) and the page rows.
    UiThreadBrowserTarget candidates[kUiThreadBrowserPageCapacity + 7 + kUiThreadBrowserForumControlCapacity];
    u32 count = 0;
    const UiThreadBrowserAction header[]{UiThreadBrowserAction::close, UiThreadBrowserAction::create_post,
        UiThreadBrowserAction::active, UiThreadBrowserAction::archived, UiThreadBrowserAction::forum_sort,
        UiThreadBrowserAction::forum_list, UiThreadBrowserAction::forum_gallery};
    for (const auto action : header) {
        const auto value = target(*s, action);
        if (eligible(*s, value)) candidates[count++] = value;
    }
    if (s->forum && s->view->forum)
        for (u32 i = 0; i < s->view->forum->tags.count && i < s->backing->toolbar.chip_count; ++i) {
            if (!s->backing->chips[i].generation) continue;
            const auto value = target(*s, UiThreadBrowserAction::forum_tag, s->view->forum->tags[i].id);
            if (eligible(*s, value)) candidates[count++] = value;
        }
    for (u32 i = 0; i < s->view->rows.count; ++i) {
        const auto value = target(*s, UiThreadBrowserAction::open_thread, s->view->rows[i].thread_id);
        if (eligible(*s, value)) candidates[count++] = value;
    }
    const UiThreadBrowserAction footer[]{UiThreadBrowserAction::retry, UiThreadBrowserAction::previous, UiThreadBrowserAction::next};
    for (const auto action : footer) {
        const auto value = target(*s, action);
        if (eligible(*s, value)) candidates[count++] = value;
    }
    const auto focused = ui_thread_browser_scene_focused_target(*s);
    u32 index = count;
    for (u32 i = 0; i < count; ++i) if (semantic_equal(candidates[i], focused)) { index = i; break; }
    index = reverse ? (index >= count || !index ? count - 1 : index - 1)
        : (index + 1 >= count ? 0 : index + 1);
    return count && ui_thread_browser_scene_focus(s, candidates[index]);
}
bool ui_thread_browser_scene_paint(const UiThreadBrowserScene& s, const Theme& t, PaintStream* out) noexcept {
    if (!current(s)) return true;
    if (!out || !ui_surface_shadow_paint_isolated(out, s.panel, t, UiElevation::popover, 0) ||
        !paint_stream_push_clip(out, s.panel)) return false;
    bool ok = ui_surface_paint_plate(out, s.panel, t, UiElevation::popover, 0, nullptr, &t.surface_high) &&
        paint_stream_push(out, ui_rect_edges(s.header, ui_start(0), ui_end(-1), ui_end(0), ui_end(0)), t.surface_high) &&
        ((!s.create_post.generation || bounds(s, s.create_post).left - s.style.gap >= s.header.left + s.style.inset + 32) ?
            ui_icon_paint(out, UiIcon::thread, {s.header.left + s.style.inset, s.header.top + 10,
                s.header.left + s.style.inset + 24, s.header.top + 34}, t.text_secondary) : true) &&
        button(out, s, t, s.close) && button(out, s, t, s.create_post) && button(out, s, t, s.active_tab, s.tab == UiThreadBrowserTab::active) &&
        button(out, s, t, s.archived_tab, s.tab == UiThreadBrowserTab::archived) &&
        button(out, s, t, s.previous) && button(out, s, t, s.next) && button(out, s, t, s.retry);
    const auto close = bounds(s, s.close);
    ok = ok && ui_icon_paint(out, UiIcon::close, ui_rect_shrink(close, 6), t.text_secondary);
    if (ok && paint_stream_push_clip(out, s.body)) {
        for (u32 i = 0; ok && !s.forum && i < s.count; ++i) {
            const auto handle = s.backing->rows[i];
            if (!handle.generation) continue;
            const auto r = s.backing->cards[i];
            ok = surface(out, r, t, hovered(s, handle) ? t.surface_high : t.surface,
                selected(s, handle) ? t.tokens.border.focus : t.surface_high, s.style.radius) &&
                ui_icon_paint(out, s.view->rows[i].type == 12 ? UiIcon::lock : UiIcon::thread,
                    ui_rect_edges(r, ui_start(8), ui_start(16), ui_start(30), ui_start(38)), t.text_secondary);
        }
        if (!s.count) {
            const float x = (s.body.left + s.body.right) * .5f, y = s.body.top + minimum(30.f, ui_rect_height(s.body) * .2f);
            ok = ok && (s.view->loading
                ? ui_spinner_paint(out, {x - 10, y - 10, x + 10, y + 10}, t.tokens.emphasis.outline, s.spinner_phase)
                : ui_icon_paint(out, s.view->error ? UiIcon::warning : UiIcon::thread,
                    {x - 12, y - 12, x + 12, y + 12}, t.text_secondary));
        }
        ok = paint_stream_pop_clip(out) && ok;
    } else if (ok) ok = false;
    if (ok && s.forum) ok = ui_forum_channel_paint(s, t, out);
    if (ok) ok = ui_scrollbar_paint(out, ui_scrollbar_geometry(s.body, s.scroll_dips,
        content_height(s)), t) && paint_stream_push(out, ui_rect_edges(s.footer, ui_start(0), ui_start(0), ui_end(0), ui_start(1)),
            t.surface_high);
    return paint_stream_pop_clip(out) && ok;
}
bool ui_thread_browser_scene_text_output(const UiThreadBrowserScene& s, const Theme& t, UiTextOutput* out) noexcept {
    if (!current(s)) return true;
    if (!ui_text_output_valid(out)) return false;
    const float left = s.panel.left + s.style.inset, right = s.panel.right - s.style.inset;
    const float title_right = bounds(s, s.create_post.generation ? s.create_post : s.close).left - s.style.gap;
    if (!text(out, s, s.root, s.forum ? (s.view->forum->media_channel ? utf8_literal("Media") : utf8_literal("Posts"))
            : utf8_literal("Threads"), {left + 32, s.header.top + 2, title_right, s.header.top + 24},
            t.text_primary, s.style.title_flags) ||
        !text(out, s, s.root, s.view->title, {left + 32, s.header.top + 24, title_right, s.header.bottom - 2},
            t.text_secondary, s.style.detail_flags)) return false;
    const UiWidgetHandle buttons[]{s.active_tab, s.archived_tab, s.previous, s.next, s.retry, s.create_post};
    const Utf8View labels[]{utf8_literal("Active"), utf8_literal("Archived"), utf8_literal("Previous"),
        utf8_literal("Next"), utf8_literal("Retry"), s.forum ? utf8_literal("New post") : utf8_literal("Create post")};
    for (u32 i = 0; i < 6; ++i) {
        if (!buttons[i].generation) continue;
        const bool enabled = ui_thread_browser_scene_target(s, buttons[i]).action != UiThreadBrowserAction::none;
        if (!text(out, s, buttons[i], labels[i], bounds(s, buttons[i]), enabled ? t.text_primary : t.text_secondary,
            s.style.title_flags, ui_text_center)) return false;
    }
    const auto status = s.view->status.bytes ? s.view->status
        : s.view->loading ? utf8_literal("Loading threads…") : s.view->error ? utf8_literal("Couldn't load threads")
        : s.tab == UiThreadBrowserTab::search ? utf8_literal("Search results")
        : s.tab == UiThreadBrowserTab::archived ? utf8_literal("Archived threads")
        : s.view->inventory_synced ? utf8_literal("Active threads in this channel") : utf8_literal("Loading threads…");
    if (!text(out, s, s.root, status, {left, s.status.top, s.retry.generation ? bounds(s, s.retry).left - s.style.gap : right,
        s.status.bottom}, s.view->error ? t.notification : t.text_secondary, s.style.detail_flags)) return false;
    if (s.forum && !ui_forum_channel_text_output(s, t, out)) return false;
    for (u32 i = 0; !s.forum && i < s.count; ++i) {
        if (!s.backing->rows[i].generation) continue;
        const auto r = s.backing->cards[i];
        const auto& row = s.view->rows[i];
        const auto color = s.view->loading || s.view->error ? t.text_secondary : t.text_primary;
        if (!text(out, s, s.backing->rows[i], row.name.bytes ? row.name : utf8_literal("Unnamed thread"),
            ui_rect_edges(r, ui_start(38), ui_start(4), ui_end(-8), ui_start(28)), color, s.style.name_flags, ui_text_in_body)) return false;
        // Prepared fixed labels expose archive and membership without inventing server state.
        const auto detail = row.type == 12
            ? (row.archived
                ? (row.membership_known ? (row.joined ? utf8_literal("Private · Archived · Joined") : utf8_literal("Private · Archived · Not joined"))
                    : utf8_literal("Private · Archived"))
                : (row.membership_known ? (row.joined ? utf8_literal("Private · Active · Joined") : utf8_literal("Private · Active · Not joined"))
                    : utf8_literal("Private · Active")))
            : (row.archived
                ? (row.membership_known ? (row.joined ? utf8_literal("Public · Archived · Joined") : utf8_literal("Public · Archived · Not joined"))
                    : utf8_literal("Public · Archived"))
                : (row.membership_known ? (row.joined ? utf8_literal("Public · Active · Joined") : utf8_literal("Public · Active · Not joined"))
                    : utf8_literal("Public · Active")));
        if (!text(out, s, s.backing->rows[i], detail, ui_rect_edges(r, ui_start(38), ui_start(28), ui_end(-8), ui_end(-4)),
            t.text_secondary, s.style.detail_flags, ui_text_in_body)) return false;
    }
    if (!s.count && !s.forum) {
        const float top = s.body.top + minimum(52.f, ui_rect_height(s.body) * .35f);
        if (!text(out, s, s.root, state_text(s), {left, top, right, minimum(s.body.bottom, top + 60)},
            t.text_secondary, s.style.title_flags, ui_text_in_body | ui_text_center | ui_text_wrap)) return false;
    }
    return text(out, s, s.root, {s.page_text, s.page_bytes},
        {bounds(s, s.previous).right + s.style.gap, s.footer.top,
         bounds(s, s.next).left - s.style.gap, s.footer.bottom}, t.text_secondary,
        s.style.detail_flags, ui_text_center);
}
} // namespace maicord
