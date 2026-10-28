#include "maicord/ui/forum_channel_scene.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/thread_browser_scene.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/rect_edges.hpp"
#include "maicord/ui/scene_text.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/visit_ref.hpp"

namespace maicord {
namespace {
float chip_width(Utf8View name, const UiForumStyle& s) noexcept {
    const auto valid = utf8_validate(name);
    const float width = static_cast<float>(valid.valid ? valid.codepoints : 0) * s.chip_scalar_width + s.chip_padding;
    return minimum(s.chip_max_width, maximum(s.chip_min_width, width));
}
// a sorts before b.
bool before(const UiForumSortKey& a, const UiForumSortKey& b, UiForumSort sort) noexcept {
    if (a.pinned != b.pinned) return a.pinned;
    if (sort == UiForumSort::latest_activity) {
        const u64 x = a.last_message_id ? a.last_message_id : a.thread_id;
        const u64 y = b.last_message_id ? b.last_message_id : b.thread_id;
        if (x != y) return x > y;
    }
    return a.thread_id > b.thread_id;
}
UiRect bounds(const UiThreadBrowserScene& s, UiWidgetHandle handle) noexcept {
    const auto* node = ui_widget_manager_layout_node(s.widgets, handle);
    return node ? node->rectangle : UiRect{};
}
bool active(const UiThreadBrowserScene& s) noexcept {
    return ui_thread_browser_scene_current(s) && s.forum && s.view->forum &&
        s.view->forum->posts.count == s.count;
}
const UiForumTag* find_tag(const UiForumChannelView& view, u64 id) noexcept {
    for (u32 i = 0; i < view.tags.count; ++i) if (view.tags[i].id == id) return view.tags.data + i;
    return nullptr;
}
bool surface(PaintStream* out, const UiRect& r, const Theme& t, const Color& fill, const Color& border, float radius) noexcept {
    return ui_background_paint_surface(out, r, t, fill, border, radius, 1, .9f,
        UiBackgroundSurfaceMode::gradient, UiBackgroundTier::high);
}
bool text(UiTextOutput* out, const UiThreadBrowserScene& s, UiWidgetHandle owner, const Utf8View& value, const UiRect& r,
    const UiRect& clip, const Color& color, u32 flags, u32 mode = 0) noexcept {
    return ui_scene_text(out, s.widgets, owner, value, r, clip, color, flags, mode);
}
// Tag chips inside a card, in the post's own order; unknown IDs are skipped.
bool card_tags(const UiForumChannelView& view, const UiForumPost& post, const UiRect& row,
    const UiForumStyle& style, UiVisitRef<const UiForumTag&, const UiRect&> visit) noexcept {
    float x = row.left;
    for (u32 i = 0; i < post.tag_count && i < kUiForumPostTagCapacity; ++i) {
        const auto* tag = find_tag(view, post.tags[i]);
        if (!tag) continue;
        const float width = minimum(chip_width(tag->name, style) - 8, row.right - x);
        if (width < style.chip_min_width * .75f) break;
        if (!visit(*tag, UiRect{x, row.top, x + width, row.bottom})) return false;
        x += width + style.chip_gap;
    }
    return true;
}
} // namespace

bool ui_forum_view_valid(const UiForumChannelView& v) noexcept {
    if (v.tags.count > kUiForumTagCapacity || (v.tags.count && !v.tags.data) ||
        (v.posts.count && !v.posts.data) || v.posts.count > kUiThreadBrowserPageCapacity ||
        v.sort > UiForumSort::creation_date || v.layout > UiForumLayout::gallery || v.match > UiForumTagMatch::all)
        return false;
    for (u32 i = 0; i < v.tags.count; ++i) {
        if (!v.tags[i].id || !utf8_validate(v.tags[i].name).valid) return false;
        for (u32 j = 0; j < i; ++j) if (v.tags[j].id == v.tags[i].id) return false;
    }
    for (u32 i = 0; i < v.posts.count; ++i) {
        const auto& post = v.posts[i];
        if (!post.thread_id || post.tag_count > kUiForumPostTagCapacity || !utf8_validate(post.preview).valid ||
            !utf8_validate(post.author_name).valid || !utf8_validate(post.activity).valid) return false;
    }
    return true;
}
bool ui_forum_post_matches(ConstSlice<u64> applied, ConstSlice<u64> selected, UiForumTagMatch match) noexcept {
    if (!selected.count) return true;
    if (!applied.count) return false;
    for (u32 i = 0; i < selected.count; ++i) {
        bool present = false;
        for (u32 j = 0; j < applied.count && !present; ++j) present = applied[j] == selected[i];
        if (present && match == UiForumTagMatch::some) return true;
        if (!present && match == UiForumTagMatch::all) return false;
    }
    return match == UiForumTagMatch::all;
}
void ui_forum_sort(Slice<UiForumSortKey> keys, UiForumSort sort) noexcept {
    if (!keys.data) return;
    // Ties resolve by unique ID, so the unstable heap sort is deterministic.
    heap_sort(keys.data, keys.count,
              [sort](const UiForumSortKey& a, const UiForumSortKey& b) noexcept { return before(a, b, sort); });
}
void ui_forum_toolbar_layout(UiRect band, const UiForumChannelView& view, const UiForumStyle& s,
    UiForumToolbar* out) noexcept {
    if (!out) return;
    reset_value(out);
    const float width = maximum(0.f, ui_rect_width(band));
    const float row_top = band.top + (s.toolbar_row_height - s.control_height) * .5f;
    const float sort_right = minimum(band.left + s.sort_width, band.left + width * .5f);
    out->sort = {band.left, row_top, sort_right, row_top + s.control_height};
    const float layout = minimum(s.layout_width, width * .22f);
    out->gallery = {band.right - layout, row_top, band.right, row_top + s.control_height};
    out->list = {out->gallery.left - layout, row_top, out->gallery.left, row_top + s.control_height};
    // Chips flow left to right and wrap; every chip stays reachable (no overflow menu).
    const float first_row = band.top + s.toolbar_row_height;
    float x = band.left, y = first_row;
    for (u32 i = 0; i < view.tags.count && i < kUiForumTagCapacity; ++i) {
        const float chip = minimum(chip_width(view.tags[i].name, s), width);
        if (x > band.left && x + chip > band.right) { x = band.left; y += s.chip_row_height; }
        const float top = y + (s.chip_row_height - s.chip_height) * .5f;
        out->chips[i] = {x, top, x + chip, top + s.chip_height};
        x += chip + s.chip_gap;
        out->chip_count = i + 1;
    }
    out->height = s.toolbar_row_height + (out->chip_count ? y - first_row + s.chip_row_height + s.chip_gap : 0.f);
}
UiForumGrid ui_forum_grid(float width, UiForumLayout layout, const UiForumStyle& s) noexcept {
    width = maximum(1.f, width);
    if (layout == UiForumLayout::list) return {1, width, s.list_card_height, s.card_gap};
    u32 columns = static_cast<u32>((width + s.card_gap) / (s.gallery_min_width + s.card_gap));
    columns = columns < 1 ? 1 : columns > 6 ? 6 : columns;
    return {columns, (width - s.card_gap * static_cast<float>(columns - 1)) / static_cast<float>(columns),
        s.gallery_card_height, s.card_gap};
}
UiRect ui_forum_card_rectangle(UiRect body, UiForumGrid grid, u32 index, float scroll) noexcept {
    if (!grid.columns) return {};
    const u32 row = index / grid.columns, column = index % grid.columns;
    const float left = body.left + static_cast<float>(column) * (grid.card_width + grid.gap);
    const float top = body.top + static_cast<float>(row) * (grid.card_height + grid.gap) - scroll;
    return {left, top, left + grid.card_width, top + grid.card_height};
}
float ui_forum_content_height(UiForumGrid grid, u32 count) noexcept {
    if (!grid.columns || !count) return 0;
    const u32 rows = (count + grid.columns - 1) / grid.columns;
    return static_cast<float>(rows) * grid.card_height + static_cast<float>(rows - 1) * grid.gap;
}
UiForumCardParts ui_forum_card_parts(UiRect card, UiForumLayout layout, const UiForumPost& post,
    const UiForumStyle& s) noexcept {
    UiForumCardParts parts{};
    const float inset = s.card_inset;
    float right = card.right - inset, y = card.top + inset;
    if (layout == UiForumLayout::list) {
        if (post.has_media) {
            parts.media = {card.right - inset - s.list_media, card.top + inset, card.right - inset,
                card.top + inset + s.list_media};
            right = parts.media.left - inset;
        }
    } else {
        // Gallery: media fills the card's top, also as the no-image placeholder.
        parts.media = {card.left, card.top, card.right, card.top + s.gallery_media_height};
        y = parts.media.bottom + 10;
    }
    if (post.tag_count) {
        parts.tags = {card.left + inset, y, right, y + s.card_tag_height};
        y += s.card_tag_height + 4;
    }
    parts.title = {card.left + inset, y, right, y + 22};
    y += 24;
    const float footer = card.bottom - inset - s.avatar;
    parts.avatar = {card.left + inset, footer, card.left + inset + s.avatar, footer + s.avatar};
    if (layout == UiForumLayout::list) {
        // One preview line under tags, two when the post has no tags.
        const float lines = post.tag_count ? 1.f : 2.f;
        parts.preview = {card.left + inset, y, right, minimum(footer - 4, y + 20 * lines)};
    }
    const float split = card.left + (card.right - card.left) * .5f;
    parts.author = {parts.avatar.right + 6, footer, maximum(parts.avatar.right + 6, split - 4), footer + s.avatar};
    parts.meta = {split, footer, layout == UiForumLayout::list ? right : card.right - inset, footer + s.avatar};
    return parts;
}
Utf8View ui_forum_sort_label(UiForumSort sort) noexcept {
    return sort == UiForumSort::creation_date ? utf8_literal("Creation date") : utf8_literal("Latest activity");
}
bool ui_forum_channel_avatar(const UiThreadBrowserScene& s, u32 index, UiRect* rectangle, UiRect* clip) noexcept {
    if (!rectangle || !clip || !active(s) || index >= s.count || !s.backing->rows[index].generation) return false;
    const auto& post = s.view->forum->posts[index];
    if (!post.author_id) return false;
    const auto card = s.backing->cards[index];
    *rectangle = ui_forum_card_parts(card, s.view->forum->layout, post, s.forum_style).avatar;
    *clip = ui_rect_intersection(ui_rect_intersection(card, s.body), *rectangle);
    return ui_rect_has_area(*clip);
}
bool ui_forum_channel_media(const UiThreadBrowserScene& s, u32 index, UiRect* rectangle, UiRect* clip) noexcept {
    if (!rectangle || !clip || !active(s) || index >= s.count || !s.backing->rows[index].generation) return false;
    const auto& post = s.view->forum->posts[index];
    if (!post.has_media) return false;
    const auto card = s.backing->cards[index];
    *rectangle = ui_forum_card_parts(card, s.view->forum->layout, post, s.forum_style).media;
    *clip = ui_rect_intersection(ui_rect_intersection(card, s.body), *rectangle);
    return ui_rect_has_area(*clip);
}
bool ui_forum_channel_paint(const UiThreadBrowserScene& s, const Theme& t, PaintStream* out) noexcept {
    if (!active(s)) return true;
    const auto& view = *s.view->forum;
    const auto& style = s.forum_style;
    const auto& toolbar = s.backing->toolbar;
    const auto control = [&](UiWidgetHandle handle, bool chosen) noexcept {
        if (!handle.generation) return true;
        const bool hover = ui_widget_handle_matches(handle, s.widgets.hovered), focus = ui_widget_handle_matches(handle, s.widgets.focused);
        return surface(out, bounds(s, handle), t,
            chosen ? ui_emphasis_selected(t, UiElevation::popover) : hover ? t.surface_high : t.surface,
            focus ? t.tokens.border.focus : chosen ? t.tokens.emphasis.outline : t.surface_high, s.style.radius);
    };
    bool ok = control(s.forum_sort, false) && control(s.forum_list, view.layout == UiForumLayout::list) &&
        control(s.forum_gallery, view.layout == UiForumLayout::gallery);
    const UiWidgetHandle icon_owners[]{s.forum_sort, s.forum_list, s.forum_gallery};
    static constexpr UiIcon toolbar_icons[]{UiIcon::sort, UiIcon::thread, UiIcon::picker};
    for (u32 i = 0; ok && i < 3; ++i)
        if (icon_owners[i].generation)
            ok = ui_icon_paint(out, toolbar_icons[i],
                ui_rect_edges(bounds(s, icon_owners[i]), ui_start(8), ui_start(7), ui_start(24), ui_start(23)), t.text_secondary);
    for (u32 i = 0; ok && i < toolbar.chip_count && i < view.tags.count; ++i) {
        const auto handle = s.backing->chips[i];
        if (!handle.generation) continue;
        ok = control(handle, view.tags[i].selected) && (!view.tags[i].selected ||
            ui_icon_paint(out, UiIcon::check,
                ui_rect_edges(toolbar.chips[i], ui_start(8), ui_start(6), ui_start(22), ui_start(20)), t.text_primary));
    }
    if (!ok || !paint_stream_push_clip(out, s.body)) return false;
    for (u32 i = 0; ok && i < s.count; ++i) {
        const auto handle = s.backing->rows[i];
        if (!handle.generation) continue;
        const auto card = s.backing->cards[i];
        const auto& post = view.posts[i];
        const auto parts = ui_forum_card_parts(card, view.layout, post, style);
        ok = surface(out, card, t, ui_widget_handle_matches(handle, s.widgets.hovered) ? t.surface_high : t.surface,
            ui_widget_handle_matches(handle, s.widgets.focused) ? t.tokens.border.focus : t.surface_high, style.radius);
        if (ok && ui_rect_has_area(parts.media)) {
            // Neutral media well; the renderer covers it with the loaded thumbnail.
            ok = paint_stream_push_rounded(out, parts.media, t.base_lowest, view.layout == UiForumLayout::gallery ? 0.f : style.radius);
            const float x = (parts.media.left + parts.media.right) * .5f, y = (parts.media.top + parts.media.bottom) * .5f;
            ok = ok && ui_icon_paint(out, post.has_media ? UiIcon::image : UiIcon::thread,
                {x - 12, y - 12, x + 12, y + 12}, t.text_secondary);
        }
        ok = ok && card_tags(view, post, parts.tags, style, [&](const UiForumTag&, UiRect chip) noexcept {
            return paint_stream_push_rounded(out, chip, t.surface_high, ui_rect_height(chip) * .5f);
        });
        if (ok && post.pinned)
            ok = ui_icon_paint(out, UiIcon::pin,
                ui_rect_edges(parts.title, ui_end(-18), ui_start(3), ui_end(-2), ui_start(19)), t.text_secondary);
        if (ok && post.author_id)
            ok = paint_stream_push_rounded(out, parts.avatar, t.avatar_placeholder, style.avatar * .5f);
        if (ok && post.metadata_known)
            ok = ui_icon_paint(out, UiIcon::message_text,
                ui_rect_edges(parts.meta, ui_start(0), ui_start(2), ui_start(16), ui_start(18)), t.text_secondary);
    }
    return paint_stream_pop_clip(out) && ok;
}
bool ui_forum_channel_text_output(const UiThreadBrowserScene& s, const Theme& t, UiTextOutput* out) noexcept {
    if (!active(s)) return true;
    const auto& view = *s.view->forum;
    const auto& style = s.forum_style;
    const auto& toolbar = s.backing->toolbar;
    if (s.forum_sort.generation) {
        const auto r = bounds(s, s.forum_sort);
        if (!text(out, s, s.forum_sort, ui_forum_sort_label(view.sort), ui_rect_edges(r, ui_start(30), ui_start(0), ui_end(-8), ui_end(0)),
            s.panel, t.text_primary, s.style.title_flags)) return false;
    }
    const UiWidgetHandle layouts[]{s.forum_list, s.forum_gallery};
    const Utf8View labels[]{utf8_literal("List"), utf8_literal("Gallery")};
    for (u32 i = 0; i < 2; ++i) {
        if (!layouts[i].generation) continue;
        const auto r = bounds(s, layouts[i]);
        if (!text(out, s, layouts[i], labels[i], ui_rect_edges(r, ui_start(28), ui_start(0), ui_end(-6), ui_end(0)), s.panel,
            t.text_primary, s.style.detail_flags)) return false;
    }
    for (u32 i = 0; i < toolbar.chip_count && i < view.tags.count; ++i) {
        if (!s.backing->chips[i].generation) continue;
        const auto r = toolbar.chips[i];
        if (!text(out, s, s.backing->chips[i], view.tags[i].name,
            {r.left + (view.tags[i].selected ? 24.f : 10.f), r.top, r.right - 10, r.bottom}, s.panel,
            view.tags[i].selected ? t.text_primary : t.text_secondary, s.style.detail_flags,
            view.tags[i].selected ? 0u : ui_text_center)) return false;
    }
    for (u32 i = 0; i < s.count; ++i) {
        const auto handle = s.backing->rows[i];
        if (!handle.generation) continue;
        const auto card = s.backing->cards[i];
        const auto clip = ui_rect_intersection(card, s.body);
        const auto& post = view.posts[i];
        const auto& row = s.view->rows[i];
        const auto parts = ui_forum_card_parts(card, view.layout, post, style);
        const auto title_color = s.view->loading || s.view->error ? t.text_secondary : t.text_primary;
        UiRect title = parts.title;
        if (post.pinned) title.right -= 22;
        if (!card_tags(view, post, parts.tags, style, [&](const UiForumTag& tag, UiRect chip) noexcept {
                return text(out, s, handle, tag.name, ui_rect_edges(chip, ui_start(8), ui_start(0), ui_end(-8), ui_end(0)), clip,
                    t.text_secondary, s.style.detail_flags, ui_text_center);
            }) ||
            !text(out, s, handle, row.name.bytes ? row.name : utf8_literal("Untitled post"), title, clip, title_color,
                s.style.name_flags) ||
            !text(out, s, handle, post.preview, parts.preview, clip, t.text_secondary, s.style.detail_flags,
                ui_text_wrap) ||
            !text(out, s, handle, post.author_name, parts.author, clip, t.text_secondary, s.style.detail_flags))
            return false;
        if (post.metadata_known) {
            const Utf8View count{s.backing->replies[i], s.backing->reply_bytes[i]};
            const UiRect count_box = ui_rect_edges(parts.meta, ui_start(20), ui_start(0), ui_start(64), ui_end(0));
            if (!text(out, s, handle, count, count_box, clip, t.text_secondary, s.style.detail_flags) ||
                !text(out, s, handle, post.activity, {count_box.right, parts.meta.top, parts.meta.right, parts.meta.bottom},
                    clip, t.text_secondary, s.style.detail_flags, ui_text_end)) return false;
        } else if (!text(out, s, handle, post.activity, parts.meta, clip, t.text_secondary, s.style.detail_flags,
            ui_text_end)) return false;
    }
    if (!s.count) {
        const float left = s.panel.left + s.style.inset, right = s.panel.right - s.style.inset;
        const float top = s.body.top + minimum(52.f, ui_rect_height(s.body) * .35f);
        bool filtered = false;
        for (u32 i = 0; i < view.tags.count; ++i) filtered |= view.tags[i].selected;
        const auto state = s.view->loading ? utf8_literal("Loading posts…")
            : s.view->error ? utf8_literal("Couldn't load posts")
            : filtered ? utf8_literal("No posts match the selected tags")
            : s.tab == UiThreadBrowserTab::search ? utf8_literal("No matching posts")
            : s.tab == UiThreadBrowserTab::archived ? utf8_literal("No older posts on this page")
            : utf8_literal("No posts yet");
        if (!text(out, s, s.root, state, {left, top, right, minimum(s.body.bottom, top + 60)}, s.body,
            t.text_secondary, s.style.title_flags, ui_text_center | ui_text_wrap)) return false;
    }
    return true;
}
} // namespace maicord
