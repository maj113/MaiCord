#include "maicord/ui/chat/threads/forum_channel_scene.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/chat/threads/browser_scene.hpp"
#include "maicord/ui/layout/rect_edges.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/ui/core/visit_ref.hpp"

namespace maicord {
namespace {
struct CardParts {
    UiRect tags, title, preview, avatar, author, meta, media;
};
float chip_width(const Utf8View& name, const UiForumStyle& s) noexcept {
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

bool active(const UiThreadBrowserScene& s) noexcept {
    return ui_thread_browser_scene_current(s) && s.forum && s.view->forum &&
        s.view->forum->posts.count == s.count;
}
const UiForumTag* find_tag(const UiForumChannelView& view, u64 id) noexcept {
    for (u32 i = 0; i < view.tags.count; ++i) if (view.tags[i].id == id) return view.tags.data + i;
    return nullptr;
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
bool ui_forum_post_matches(const ConstSlice<u64>& applied, const ConstSlice<u64>& selected, UiForumTagMatch match) noexcept {
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
void ui_forum_sort(const Slice<UiForumSortKey>& keys, UiForumSort sort) noexcept {
    if (!keys.data) return;
    // Ties resolve by unique ID, so the unstable heap sort is deterministic.
    heap_sort_shared(keys.data, keys.count,
              [sort](const UiForumSortKey& a, const UiForumSortKey& b) noexcept { return before(a, b, sort); });
}
void ui_forum_toolbar_layout(const UiRect& band, const UiForumChannelView& view, const UiForumStyle& s,
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
    columns = clamp_value(columns, 1u, 6u);
    return {columns, (width - s.card_gap * static_cast<float>(columns - 1)) / static_cast<float>(columns),
        s.gallery_card_height, s.card_gap};
}
UiRect ui_forum_card_rectangle(const UiRect& body, UiForumGrid grid, u32 index, float scroll) noexcept {
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
static CardParts card_parts(const UiRect& card, UiForumLayout layout, const UiForumPost& post,
    const UiForumStyle& s) noexcept {
    CardParts parts{};
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
    *rectangle = card_parts(card, s.view->forum->layout, post, s.forum_style).avatar;
    *clip = ui_rect_intersection(ui_rect_intersection(card, s.body), *rectangle);
    return ui_rect_has_area(*clip);
}
bool ui_forum_channel_media(const UiThreadBrowserScene& s, u32 index, UiRect* rectangle, UiRect* clip) noexcept {
    if (!rectangle || !clip || !active(s) || index >= s.count || !s.backing->rows[index].generation) return false;
    const auto& post = s.view->forum->posts[index];
    if (!post.has_media) return false;
    const auto card = s.backing->cards[index];
    *rectangle = card_parts(card, s.view->forum->layout, post, s.forum_style).media;
    *clip = ui_rect_intersection(ui_rect_intersection(card, s.body), *rectangle);
    return ui_rect_has_area(*clip);
}
bool ui_forum_channel_paint(const UiThreadBrowserScene& s, const Theme& t, PaintStream* out) noexcept {
    if (!active(s)) return true;
    UiPaint p{out, t};
    const auto& view = *s.view->forum;
    const auto& style = s.forum_style;
    const auto& toolbar = s.backing->toolbar;
    const auto look = ui_thread_browser_look(s);
    p.control(s.widgets, s.forum_sort, look).control(s.widgets, s.forum_list, look, view.layout == UiForumLayout::list)
        .control(s.widgets, s.forum_gallery, look, view.layout == UiForumLayout::gallery);
    const UiWidgetHandle icon_owners[]{s.forum_sort, s.forum_list, s.forum_gallery};
    static constexpr UiIcon toolbar_icons[]{UiIcon::sort, UiIcon::thread, UiIcon::picker};
    for (u32 i = 0; i < 3; ++i)
        if (icon_owners[i].generation)
            p.icon(toolbar_icons[i], ui_rect_edges(ui_bounds(s, icon_owners[i]), ui_start(8), ui_start(7), ui_start(24),
                                                   ui_start(23)), t.text_secondary);
    for (u32 i = 0; p && i < toolbar.chip_count && i < view.tags.count; ++i) {
        p.control(s.widgets, s.backing->chips[i], look, view.tags[i].selected);
        if (s.backing->chips[i].generation && view.tags[i].selected)
            p.icon(UiIcon::check, ui_rect_edges(toolbar.chips[i], ui_start(8), ui_start(6), ui_start(22), ui_start(20)),
                   t.text_primary);
    }
    p.clip(s.body);
    for (u32 i = 0; p && i < s.count; ++i) {
        const auto handle = s.backing->rows[i];
        if (!handle.generation) continue;
        const auto card = s.backing->cards[i];
        const auto& post = view.posts[i];
        const auto parts = card_parts(card, view.layout, post, style);
        p.look(card, {.fill = UI_TOKEN(surface), .hover = UI_TOKEN(surface_high), .border = UI_TOKEN(surface_high),
                      .radius = style.radius}, ui_widget_state(s.widgets, handle));
        if (ui_rect_has_area(parts.media)) {
            // Neutral media well; the renderer covers it with the loaded thumbnail.
            const float x = (parts.media.left + parts.media.right) * .5f, y = (parts.media.top + parts.media.bottom) * .5f;
            p.rounded(parts.media, t.base_lowest, view.layout == UiForumLayout::gallery ? 0.f : style.radius)
                .icon(post.has_media ? UiIcon::image : UiIcon::thread, {x - 12, y - 12, x + 12, y + 12}, t.text_secondary);
        }
        card_tags(view, post, parts.tags, style, [&](const UiForumTag&, UiRect chip) noexcept {
            return p.rounded(chip, t.surface_high, ui_rect_height(chip) * .5f).ok();
        });
        if (post.pinned)
            p.icon(UiIcon::pin, ui_rect_edges(parts.title, ui_end(-18), ui_start(3), ui_end(-2), ui_start(19)),
                   t.text_secondary);
        if (post.author_id)
            p.rounded(parts.avatar, t.avatar_placeholder, style.avatar * .5f);
        if (post.metadata_known)
            p.icon(UiIcon::message_text, ui_rect_edges(parts.meta, ui_start(0), ui_start(2), ui_start(16), ui_start(18)),
                   t.text_secondary);
    }
    return p.end();
}
bool ui_forum_channel_text_output(const UiThreadBrowserScene& s, const Theme& t, UiTextOutput* out) noexcept {
    if (!active(s)) return true;
    UiTextPen text{out, s.widgets, s.panel, s.body};
    const auto& view = *s.view->forum;
    const auto& style = s.forum_style;
    const auto& toolbar = s.backing->toolbar;
    if (s.forum_sort.generation)
        text(s.forum_sort, ui_forum_sort_label(view.sort),
             ui_rect_edges(ui_bounds(s, s.forum_sort), ui_start(30), ui_start(0), ui_end(-8), ui_end(0)), t.text_primary,
             s.style.title_flags);
    const UiWidgetHandle layouts[]{s.forum_list, s.forum_gallery};
    const Utf8View labels[]{utf8_literal("List"), utf8_literal("Gallery")};
    for (u32 i = 0; i < 2; ++i)
        if (layouts[i].generation)
            text(layouts[i], labels[i], ui_rect_edges(ui_bounds(s, layouts[i]), ui_start(28), ui_start(0), ui_end(-6), ui_end(0)),
                 t.text_primary, s.style.detail_flags);
    for (u32 i = 0; i < toolbar.chip_count && i < view.tags.count; ++i) {
        if (!s.backing->chips[i].generation) continue;
        const auto r = toolbar.chips[i];
        const bool selected = view.tags[i].selected;
        text(s.backing->chips[i], view.tags[i].name, {r.left + (selected ? 24.f : 10.f), r.top, r.right - 10, r.bottom},
             selected ? t.text_primary : t.text_secondary, s.style.detail_flags, selected ? 0u : ui_text_center);
    }
    for (u32 i = 0; text.ok() && i < s.count; ++i) {
        const auto handle = s.backing->rows[i];
        if (!handle.generation) continue;
        const auto card = s.backing->cards[i];
        auto in_card = text.in(ui_rect_intersection(card, s.body));
        const auto& post = view.posts[i];
        const auto& row = s.view->rows[i];
        const auto parts = card_parts(card, view.layout, post, style);
        UiRect title = parts.title;
        if (post.pinned) title.right -= 22;
        card_tags(view, post, parts.tags, style, [&](const UiForumTag& tag, UiRect chip) noexcept {
            return in_card(handle, tag.name, ui_rect_edges(chip, ui_start(8), ui_start(0), ui_end(-8), ui_end(0)),
                           t.text_secondary, s.style.detail_flags, ui_text_center).pen->ok();
        });
        in_card(handle, row.name.bytes ? row.name : utf8_literal("Untitled post"), title,
                s.view->loading || s.view->error ? t.text_secondary : t.text_primary, s.style.name_flags)
            (handle, post.preview, parts.preview, t.text_secondary, s.style.detail_flags, ui_text_wrap)
            (handle, post.author_name, parts.author, t.text_secondary, s.style.detail_flags);
        if (post.metadata_known) {
            const UiRect count_box = ui_rect_edges(parts.meta, ui_start(20), ui_start(0), ui_start(64), ui_end(0));
            in_card(handle, {s.backing->replies[i], s.backing->reply_bytes[i]}, count_box, t.text_secondary,
                    s.style.detail_flags)
                (handle, post.activity, {count_box.right, parts.meta.top, parts.meta.right, parts.meta.bottom},
                 t.text_secondary, s.style.detail_flags, ui_text_end);
        } else {
            in_card(handle, post.activity, parts.meta, t.text_secondary, s.style.detail_flags, ui_text_end);
        }
    }
    if (s.count) return text.ok();
    const float left = s.panel.left + s.style.inset, right = s.panel.right - s.style.inset;
    const float top = s.body.top + minimum(52.f, ui_rect_height(s.body) * .35f);
    bool filtered = false;
    for (u32 i = 0; i < view.tags.count; ++i) filtered |= view.tags[i].selected;
    text(s.root, s.view->loading ? utf8_literal("Loading posts…")
                 : s.view->error ? utf8_literal("Couldn't load posts")
                 : filtered ? utf8_literal("No posts match the selected tags")
                 : s.tab == UiThreadBrowserTab::search ? utf8_literal("No matching posts")
                 : s.tab == UiThreadBrowserTab::archived ? utf8_literal("No older posts on this page")
                 : utf8_literal("No posts yet"),
         {left, top, right, minimum(s.body.bottom, top + 60)}, t.text_secondary, s.style.title_flags,
         ui_text_in_body | ui_text_center | ui_text_wrap);
    return text.ok();
}
} // namespace maicord
