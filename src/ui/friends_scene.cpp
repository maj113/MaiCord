#include "maicord/ui/friends_scene.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
constexpr float row_height = 56, header_height = 76, status_height = 32, search_height = 48;
constexpr float section_height = 32;
// Friend row actions: 36-DIP round icon buttons, 10 DIP apart. Text actions
// keep their 48-DIP pill with a 2-DIP gap.
constexpr float icon_action_extent = 36, icon_action_glyph = 20, icon_action_gap = 10;
constexpr float text_action_width = 48, text_action_gap = 2;
constexpr u32 maximum_visible = kUiFriendsMaximumVisibleRows;
// Active Now column layout (DIPs).
constexpr float active_padding = 16, active_heading = 56, active_card_height = 96, active_card_gap = 12;
constexpr float active_avatar = 32, active_avatar_step = 24, active_toggle_extent = 28;
struct Entry {
    UiFriendsTarget target;
    UiFriendsRow row;
    bool row_label;
    char section_label[48];
    u8 section_label_bytes;
};
Entry* entries(UiFriendsScene& s) noexcept {
    return static_cast<Entry*>(s.storage.sidecar);
}
const Entry* entries(const UiFriendsScene& s) noexcept {
    return static_cast<const Entry*>(s.storage.sidecar);
}
bool current(const UiFriendsScene& s) noexcept {
    return s.active &&
           (!s.source || (s.source->revision == s.source_revision && s.source->count == s.source_count)) &&
           (!s.content || s.content->revision == s.content_revision) &&
           (!s.sections || s.sections->revision == s.sections_revision) &&
           (!s.active_now || s.active_now->revision == s.active_now_revision);
}
// Whether the column fits beside a list of at least the minimum width.
bool active_fits(const UiFriendsActiveNow* active, UiRect viewport, bool modal) noexcept {
    const float width = active && active->collapsed ? kUiFriendsActiveNowCollapsedWidth : kUiFriendsActiveNowWidth;
    return active && !modal && ui_rect_width(viewport) >= kUiFriendsActiveNowMinimumList + width;
}
UiRect active_toggle_rect(const UiFriendsScene& s) noexcept {
    const auto& r = s.active_rect;
    const float top = r.top + (active_heading - active_toggle_extent) * .5f;
    const float left = s.active_collapsed ? (r.left + r.right - active_toggle_extent) * .5f
                                          : r.right - active_padding - active_toggle_extent;
    return {left, top, left + active_toggle_extent, top + active_toggle_extent};
}
u32 active_card_count(const UiFriendsScene& s) noexcept {
    if (!s.active_visible || s.active_collapsed || !s.active_now)
        return 0;
    const float room = ui_rect_height(s.active_rect) - active_heading - active_padding;
    const u32 fit = room > active_card_height
                        ? static_cast<u32>((room + active_card_gap) / (active_card_height + active_card_gap))
                        : 0;
    return minimum(minimum(s.active_now->count, kUiFriendsActiveNowCards), fit);
}
UiRect active_card_rect(const UiFriendsScene& s, u32 index) noexcept {
    const auto& r = s.active_rect;
    const float top = r.top + active_heading + index * (active_card_height + active_card_gap);
    return {r.left + active_padding, top, r.right - active_padding, top + active_card_height};
}
UiRect active_avatar_rect(UiRect card, u32 slot) noexcept {
    const float left = card.left + 14 + slot * active_avatar_step, top = card.top + 14;
    return {left, top, left + active_avatar, top + active_avatar};
}
bool searching(const UiFriendsScene& s) noexcept {
    return s.search_active;
}
bool hidden(const UiFriendsScene& s) noexcept {
    return s.sections && s.sections->hidden_view;
}
bool sectioned(const UiFriendsScene& s) noexcept {
    return s.sections && !hidden(s);
}
float height(const UiFriendsScene& s) noexcept {
    return maximum(1.f, ui_rect_height(s.surface) - (hidden(s) ? 96 : header_height) - status_height -
                            (searching(s) ? search_height : 0));
}
UiRect surface(UiRect viewport, bool modal) noexcept {
    if (!modal)
        return viewport;
    const float width = minimum(480.f, ui_rect_width(viewport) - 32),
                h = minimum(560.f, ui_rect_height(viewport) - 32);
    const float left = (viewport.left + viewport.right - width) * .5f,
                top = (viewport.top + viewport.bottom - h) * .5f;
    return {left, top, left + width, top + h};
}
u32 logical_count(const UiFriendsScene& s) noexcept {
    return s.phase == UiFriendsPhase::ready && s.tab != UiFriendsTab::add_friend ? s.source_count : 0;
}
const UiFriendsSection* section_at(const UiFriendsScene& s, u32 index) noexcept {
    if (!sectioned(s))
        return nullptr;
    for (u32 i = 0; i < s.sections->count; ++i) {
        const auto& section = s.sections->sections[i];
        if (index >= section.first && index - section.first < section.count)
            return &section;
    }
    return nullptr;
}
float extent(const UiFriendsScene& s) noexcept {
    if (!sectioned(s) || s.phase != UiFriendsPhase::ready)
        return static_cast<float>(logical_count(s)) * row_height;
    float value = 0;
    for (u32 i = 0; i < s.sections->count; ++i) {
        const auto& section = s.sections->sections[i];
        value += section_height + (section.collapsed ? 0 : static_cast<float>(section.count) * row_height);
    }
    return value;
}
bool row_top(const UiFriendsScene& s, u32 index, float* out) noexcept {
    if (!sectioned(s)) {
        *out = static_cast<float>(index) * row_height;
        return true;
    }
    float value = 0;
    for (u32 i = 0; i < s.sections->count; ++i) {
        const auto& section = s.sections->sections[i];
        if (index >= section.first && index - section.first < section.count) {
            if (section.collapsed)
                return false;
            *out = value + section_height + static_cast<float>(index - section.first) * row_height;
            return true;
        }
        value += section_height + (section.collapsed ? 0 : static_cast<float>(section.count) * row_height);
    }
    return false;
}
float clamp(const UiFriendsScene& s, float value) noexcept {
    return maximum(0.f, minimum(value, maximum(0.f, extent(s) - height(s))));
}
u32 expanded_count(const UiFriendsScene& s) noexcept {
    if (!sectioned(s))
        return logical_count(s);
    if (s.phase != UiFriendsPhase::ready)
        return 0;
    u32 count = 0;
    for (u32 i = 0; i < s.sections->count; ++i)
        if (!s.sections->sections[i].collapsed)
            count += s.sections->sections[i].count;
    return count;
}
u32 expanded_index(const UiFriendsScene& s, u32 rank) noexcept {
    if (!sectioned(s))
        return rank;
    for (u32 i = 0; i < s.sections->count; ++i) {
        const auto& section = s.sections->sections[i];
        if (section.collapsed)
            continue;
        if (rank < section.count)
            return section.first + rank;
        rank -= section.count;
    }
    return kUiListNoSelection;
}
u32 expanded_rank(const UiFriendsScene& s, u32 index) noexcept {
    if (!sectioned(s))
        return index;
    u32 rank = 0;
    for (u32 i = 0; i < s.sections->count; ++i) {
        const auto& section = s.sections->sections[i];
        if (section.collapsed)
            continue;
        if (index >= section.first && index - section.first < section.count)
            return rank + index - section.first;
        rank += section.count;
    }
    return kUiListNoSelection;
}
struct WindowItem {
    u32 index, section;
    float top;
    bool heading;
};
struct Window {
    WindowItem items[maximum_visible + 4];
    u32 count, rows, first, mask;
};
Window window(const UiFriendsScene& s, float scroll) noexcept {
    Window result{};
    result.first = logical_count(s);
    if (!sectioned(s)) {
        const auto first = minimum(logical_count(s), static_cast<u32>(scroll / row_height));
        const auto count = minimum(logical_count(s) - first, static_cast<u32>(height(s) / row_height) + 2);
        result.first = first;
        for (u32 i = 0; i < count && i < maximum_visible; ++i)
            result.items[result.count++] = {first + i, 0, static_cast<float>(first + i) * row_height,
                                            false};
        result.rows = count;
        return result;
    }
    if (s.phase != UiFriendsPhase::ready)
        return result;
    float top = 0;
    const float end = scroll + height(s) + row_height;
    for (u32 i = 0; i < s.sections->count; ++i) {
        const auto& section = s.sections->sections[i];
        if (top + section_height > scroll && top < end) {
            result.items[result.count++] = {0, i, top, true};
            result.mask |= 1u << i;
        }
        top += section_height;
        if (!section.collapsed && section.count) {
            const float bottom = top + static_cast<float>(section.count) * row_height;
            if (bottom > scroll && top < end) {
                const u32 first =
                    scroll > top ? minimum(section.count, static_cast<u32>((scroll - top) / row_height))
                                 : 0;
                const u32 last =
                    minimum(section.count, static_cast<u32>(maximum(0.f, end - top) / row_height) + 1);
                for (u32 j = first; j < last && result.rows < maximum_visible; ++j) {
                    if (!result.rows)
                        result.first = section.first + j;
                    result.items[result.count++] = {section.first + j, i,
                                                    top + static_cast<float>(j) * row_height, false};
                    ++result.rows;
                }
            }
            top = bottom;
        }
    }
    return result;
}
bool valid_sections(const UiFriendsSections* sections, const UiFriendsSource* source,
                    UiFriendsTab tab) noexcept {
    if (!sections)
        return true;
    if (tab != UiFriendsTab::pending || sections->count > 4 || (sections->count && !sections->sections) ||
        sections->hidden_count > 16384 || (source && source->count > maximum_visible && !source->find))
        return false;
    if (sections->hidden_view)
        return !sections->count && !sections->sections;
    u32 end = 0, total = 0;
    for (u32 i = 0; i < sections->count; ++i) {
        const auto& section = sections->sections[i];
        if (section.kind > UiFriendsSectionKind::ignored ||
            (i && section.kind <= sections->sections[i - 1].kind) || section.first != end ||
            section.count > 16384 - end || section.total_count < section.count ||
            section.total_count > 16384 - total || section.kind > UiFriendsSectionKind::outgoing)
            return false;
        end += section.count;
        total += section.total_count;
    }
    return end == (source ? source->count : 0);
}
Utf8View section_label(UiFriendsSectionKind kind) noexcept {
    switch (kind) {
    case UiFriendsSectionKind::incoming:
        return utf8_literal("Received");
    case UiFriendsSectionKind::outgoing:
        return utf8_literal("Sent");
    case UiFriendsSectionKind::spam:
        return utf8_literal("Spam");
    case UiFriendsSectionKind::ignored:
        return utf8_literal("Ignored");
    }
    return {};
}
void section_heading(Entry& e, const UiFriendsSection& section) noexcept {
    u32 used = 0;
    const auto label = section_label(section.kind);
    for (u32 i = 0; i < label.bytes; ++i)
        e.section_label[used++] = label.data[i];
    e.section_label[used++] = ' ';
    e.section_label[used++] = static_cast<char>(0xe2);
    e.section_label[used++] = static_cast<char>(0x80);
    e.section_label[used++] = static_cast<char>(0x94);
    e.section_label[used++] = ' ';
    char digits[10];
    u32 n = 0, value = section.count;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (n)
        e.section_label[used++] = digits[--n];
    e.section_label[used++] = ' ';
    e.section_label[used++] = section.collapsed ? '+' : '-';
    e.section_label_bytes = static_cast<u8>(used);
}
Utf8View tab_label(UiFriendsTab t) noexcept {
    switch (t) {
    case UiFriendsTab::online:
        return utf8_literal("Online");
    case UiFriendsTab::all:
        return utf8_literal("All");
    case UiFriendsTab::pending:
        return utf8_literal("Pending");
    case UiFriendsTab::blocked:
        return utf8_literal("Blocked");
    case UiFriendsTab::add_friend:
        return utf8_literal("Add friend");
    }
    return {};
}
Utf8View action_label(UiFriendsAction a) noexcept {
    switch (a) {
    case UiFriendsAction::open_dm:
        return utf8_literal("Chat");
    case UiFriendsAction::profile:
        return utf8_literal("Profile");
    case UiFriendsAction::accept:
        return utf8_literal("Accept");
    case UiFriendsAction::ignore:
        return utf8_literal("Ignore");
    case UiFriendsAction::cancel:
        return utf8_literal("Cancel");
    case UiFriendsAction::remove:
        return utf8_literal("Remove");
    case UiFriendsAction::block:
        return utf8_literal("Block");
    case UiFriendsAction::unblock:
        return utf8_literal("Unblock");
    case UiFriendsAction::add_friend:
        return utf8_literal("Add friend");
    case UiFriendsAction::retry:
        return utf8_literal("Retry");
    case UiFriendsAction::more:
        return utf8_literal("More");
    case UiFriendsAction::decline:
        return utf8_literal("X");
    case UiFriendsAction::clear_incoming:
        return utf8_literal("Clear");
    case UiFriendsAction::clear_search:
        return utf8_literal("X");
    default:
        return {};
    }
}
Utf8View status_label(const UiFriendsScene& s) noexcept {
    if (s.status.bytes)
        return s.status;
    if (s.phase == UiFriendsPhase::loading)
        return utf8_literal("Loading friends…");
    if (s.phase == UiFriendsPhase::error)
        return utf8_literal("Friends could not be loaded. Try again.");
    if (s.tab == UiFriendsTab::add_friend)
        return utf8_literal("Add a friend using their exact username.");
    if (searching(s) && s.content->search.text.bytes && !logical_count(s))
        return utf8_literal("No matching friends.");
    if (!logical_count(s)) {
        switch (s.tab) {
        case UiFriendsTab::online:
            return utf8_literal("No friends are online right now.");
        case UiFriendsTab::pending:
            return utf8_literal("No pending friend requests.");
        case UiFriendsTab::blocked:
            return utf8_literal("You have not blocked anyone.");
        default:
            return utf8_literal("Your friends will appear here.");
        }
    }
    return tab_label(s.tab);
}
bool valid_row(const UiFriendsRow& r, UiFriendsTab tab) noexcept {
    if (!r.user_id || r.kind > UiFriendsKind::blocked || r.presence > UiPresence::offline ||
        (!r.display_name.bytes && !r.username.bytes) || r.display_name.bytes > 256 ||
        r.username.bytes > 256 || r.status.bytes > 512 || !utf8_validate(r.display_name).valid ||
        !utf8_validate(r.username).valid || !utf8_validate(r.status).valid)
        return false;
    return tab == UiFriendsTab::pending
               ? (r.kind == UiFriendsKind::incoming || r.kind == UiFriendsKind::outgoing)
           : tab == UiFriendsTab::blocked
               ? r.kind == UiFriendsKind::blocked
               : r.kind == UiFriendsKind::friend_user &&
                     (tab != UiFriendsTab::online || r.presence == UiPresence::online ||
                      r.presence == UiPresence::idle || r.presence == UiPresence::busy);
}
bool resolve(const UiFriendsScene& s, u32 index, UiFriendsRow* out) noexcept {
    if (!s.source || !s.source->resolve || index >= logical_count(s) ||
        !s.source->resolve(s.source->context, index, out) || !valid_row(*out, s.tab))
        return false;
    if (hidden(s))
        return out->kind == UiFriendsKind::incoming && (out->spam || out->ignored);
    const auto* section = section_at(s, index);
    return !sectioned(s) ||
           (section && (section->kind == UiFriendsSectionKind::outgoing
                            ? out->kind == UiFriendsKind::outgoing
                            : out->kind == UiFriendsKind::incoming && !out->spam && !out->ignored));
}
UiIcon action_icon(UiFriendsAction a) noexcept {
    return a == UiFriendsAction::open_dm ? UiIcon::direct_message
           : a == UiFriendsAction::more  ? UiIcon::ellipsis_vertical
                                         : UiIcon::count;
}
float action_width(UiFriendsAction a) noexcept {
    return action_icon(a) != UiIcon::count ? icon_action_extent : text_action_width;
}
float action_step(UiFriendsAction a) noexcept {
    return action_width(a) + (action_icon(a) != UiIcon::count ? icon_action_gap : text_action_gap);
}
UiFriendsAction primary(const UiFriendsRow& r) noexcept {
    return r.kind == UiFriendsKind::friend_user ? UiFriendsAction::open_dm : UiFriendsAction::profile;
}
u32 actions(const UiFriendsRow& r, UiFriendsAction* out, bool hidden_view = false) noexcept {
    u32 n = 0;
    if (r.kind == UiFriendsKind::friend_user)
        out[n++] = UiFriendsAction::open_dm;
    else if (r.kind == UiFriendsKind::incoming) {
        out[n++] = UiFriendsAction::accept;
        out[n++] = UiFriendsAction::decline;
    } else if (r.kind == UiFriendsKind::outgoing)
        out[n++] = UiFriendsAction::cancel;
    else
        out[n++] = UiFriendsAction::unblock;
    if (!hidden_view)
        out[n++] = UiFriendsAction::more;
    return n;
}
bool allowed(const UiFriendsRow& r, UiFriendsAction a) noexcept {
    if (a == UiFriendsAction::more || a == UiFriendsAction::profile)
        return true;
    if (r.busy && a != UiFriendsAction::open_dm)
        return false;
    if (a == UiFriendsAction::ignore || a == UiFriendsAction::unignore)
        return r.kind != UiFriendsKind::blocked &&
               (a == UiFriendsAction::unignore ? r.ignored : !r.ignored);
    if (r.kind == UiFriendsKind::friend_user &&
        (a == UiFriendsAction::remove || a == UiFriendsAction::block))
        return true;
    if ((r.kind == UiFriendsKind::incoming || r.kind == UiFriendsKind::outgoing) &&
        a == UiFriendsAction::block)
        return true;
    UiFriendsAction available[4]{};
    const auto n = actions(r, available);
    for (u32 i = 0; i < n; ++i)
        if (a == available[i])
            return true;
    return false;
}
void count_heading(UiFriendsScene& s) noexcept {
    u32 used = 0;
    const auto append_number = [&](u32 value) noexcept {
        char digits[10];
        u32 count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value && count < 10);
        while (count && used < sizeof(s.count_heading))
            s.count_heading[used++] = digits[--count];
    };
    const u32 count = logical_count(s);
    append_number(count);
    const u32 total = s.content && s.content->total_count ? s.content->total_count : count;
    const auto suffix = total != count ? utf8_literal(" / ")
                        : count == 1   ? utf8_literal(" result")
                                       : utf8_literal(" results");
    for (u32 i = 0; i < suffix.bytes && used < sizeof(s.count_heading); ++i)
        s.count_heading[used++] = suffix.data[i];
    if (total != count)
        append_number(total);
    s.count_heading_bytes = static_cast<u8>(used);
}
bool find(const UiFriendsScene& s, u64 id, u32* index, UiFriendsRow* row) noexcept {
    if (!id || !current(s) || !s.source || !logical_count(s))
        return false;
    if (s.source->find) {
        u32 at = 0;
        if (!s.source->find(s.source->context, id, &at) || !resolve(s, at, row) || row->user_id != id)
            return false;
        *index = at;
        return true;
    }
    const u32 limit = s.sections ? minimum(logical_count(s), maximum_visible) : logical_count(s);
    for (u32 i = 0; i < limit; ++i) {
        if (!resolve(s, i, row))
            return false;
        if (row->user_id == id) {
            *index = i;
            return true;
        }
    }
    return false;
}
UiLayoutStyle size(float width, float h) noexcept {
    auto st = ui_layout_style();
    st.width = width < 0 ? ui_layout_fill() : ui_layout_fixed(width);
    st.height = h < 0 ? ui_layout_fill() : ui_layout_fixed(h);
    st.flags |= ui_layout_node_allow_zero_size | ui_layout_node_clip_children;
    return st;
}
UiWidgetHandle add(UiFriendsScene& s, UiWidgetHandle parent, UiWidgetKind kind, UiLayoutStyle st,
                   UiFriendsTarget target = {}, const UiFriendsRow& row = {}, bool row_label = false,
                   bool enabled = true) noexcept {
    const auto name = row_label ? (row.display_name.bytes ? row.display_name : row.username)
                      : target.action == UiFriendsAction::select_tab ? tab_label(target.tab)
                      : target.action == UiFriendsAction::search     ? utf8_literal("Search friends")
                      : target.action == UiFriendsAction::none       ? utf8_literal("Friends")
                                                                     : action_label(target.action);
    const u32 id = s.widgets.count + 1;
    const auto result = ui_widget_manager_add_prevalidated(
        &s.widgets, parent,
        {id,
         kind,
         static_cast<u16>(target.action != UiFriendsAction::none && enabled
                              ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable
                              : ui_widget_none),
         {name, st, {}, 0, id}});
    if (!result.ok())
        return ui_widget_invalid_handle();
    entries(s)[result.handle.slot] = {target, row, row_label};
    return result.handle;
}
UiWidgetHandle handle(const UiFriendsScene& s, UiFriendsTarget target) noexcept {
    for (u32 i = 0; i < s.widgets.count; ++i)
        if (ui_friends_target_equal(entries(s)[i].target, target))
            return {static_cast<u16>(i), s.widgets.storage[i].generation};
    return ui_widget_invalid_handle();
}
bool rebuild(UiFriendsScene& s) noexcept {
    const auto visible_window = window(s, s.scroll_dips);
    const u32 first = visible_window.first, count = visible_window.rows;
    if (count > maximum_visible)
        return false;
    UiFriendsRow visible[maximum_visible]{};
    u32 resolved = 0;
    for (u32 i = 0; i < visible_window.count; ++i) {
        const auto& item = visible_window.items[i];
        if (item.heading)
            continue;
        if (!resolve(s, item.index, &visible[resolved]))
            return false;
        for (u32 j = 0; j < resolved; ++j)
            if (visible[j].user_id == visible[resolved].user_id)
                return false;
        ++resolved;
    }
    const u32 required = 24 + 4 * count;
    if (s.capacity < required) {
        ui_fixed_widget_storage_release(&s.storage);
        s.widgets = {};
        s.capacity = 0;
        const u32 capacity = (required + 15) & ~15u;
        if (!ui_fixed_widget_storage_initialize(
                &s.storage, capacity, capacity * static_cast<u32>(sizeof(Entry)), alignof(Entry)) ||
            !ui_widget_manager_initialize(&s.widgets, s.storage.widgets, s.storage.layout_nodes))
            return false;
        s.capacity = capacity;
    } else
        ui_widget_manager_reset(&s.widgets);
    auto st = size(-1, -1);
    st.flow = UiLayoutFlow::vertical;
    const auto root = add(s, ui_widget_invalid_handle(), UiWidgetKind::container, st);
    st = size(-1, hidden(s) ? 96 : header_height);
    st.flow = UiLayoutFlow::vertical;
    st.padding = {12, 8, 12, 8};
    st.gap = 4;
    const auto header = add(s, root, UiWidgetKind::container, st);
    s.heading = add(s, header, UiWidgetKind::text, size(-1, 24));
    if (hidden(s))
        s.status_widget = add(s, header, UiWidgetKind::text, size(-1, 48));
    if (!hidden(s)) {
        st = size(-1, 32);
        st.flow = UiLayoutFlow::horizontal;
        st.gap = 4;
        const auto tabs = add(s, header, UiWidgetKind::container, st);
        const float widths[]{54, 32, 68, 68, 84};
        const float tab_scale = minimum(1.f, maximum(1.f, ui_rect_width(s.viewport) - 40) / 306.f);
        for (u32 i = 0; i < 5; ++i)
            if (!add(s, tabs, UiWidgetKind::button, size(widths[i] * tab_scale, 32),
                     {UiFriendsAction::select_tab, 0, static_cast<UiFriendsTab>(i)})
                     .generation)
                return false;
    }
    s.search_field = s.clear_search = ui_widget_invalid_handle();
    if (searching(s)) {
        st = size(-1, search_height);
        st.flow = UiLayoutFlow::overlay;
        st.padding = {12, 6, 12, 6};
        const auto search_row = add(s, root, UiWidgetKind::container, st);
        st = size(-1, -1);
        st.flow = UiLayoutFlow::overlay;
        s.search_field =
            add(s, search_row, UiWidgetKind::text_field, st, {UiFriendsAction::search, 0, s.tab});
        st = size(28, 28);
        st.horizontal_alignment = UiLayoutAlignment::end;
        st.vertical_alignment = UiLayoutAlignment::center;
        st.margin.right = 4;
        s.clear_search =
            add(s, s.search_field, UiWidgetKind::button, st, {UiFriendsAction::clear_search, 0, s.tab}, {},
                false, s.content->search.text.bytes != 0);
        if (!s.search_field.generation || !s.clear_search.generation)
            return false;
    }
    st = size(-1, status_height);
    st.flow = UiLayoutFlow::horizontal;
    st.padding = {12, 2, 12, 2};
    st.gap = 8;
    const auto status_row = add(s, root, UiWidgetKind::container, st);
    if (!hidden(s))
        s.status_widget = add(s, status_row, UiWidgetKind::text, size(-1, -1));
    if (s.sections && (s.sections->hidden_available || s.sections->hidden_view)) {
        if (!add(s, status_row, UiWidgetKind::button, size(hidden(s) ? 64.f : 132.f, -1),
                 {UiFriendsAction::toggle_hidden, 0, s.tab})
                 .generation)
            return false;
    }
    if (hidden(s) && s.sections->hidden_count > 1)
        if (!add(s, status_row, UiWidgetKind::button, size(64, -1),
                 {UiFriendsAction::clear_hidden, 0, s.tab})
                 .generation)
            return false;
    s.count_widget = add(s, status_row, UiWidgetKind::text, size(hidden(s) ? 64.f : 112.f, -1));
    count_heading(s);
    st = size(-1, -1);
    st.flow = UiLayoutFlow::vertical;
    st.flags |= ui_layout_node_scroll_vertical;
    s.body = add(s, root, UiWidgetKind::scroll_view, st);
    float cursor = 0;
    u32 row_slot = 0;
    for (u32 i = 0; i < visible_window.count; ++i) {
        const auto& item = visible_window.items[i];
        if (item.top > cursor &&
            !add(s, s.body, UiWidgetKind::container, size(-1, item.top - cursor)).generation)
            return false;
        if (item.heading) {
            const auto& section = s.sections->sections[item.section];
            st = size(-1, section_height);
            st.padding = {12, 2, 12, 2};
            st.flow = UiLayoutFlow::overlay;
            const auto h = add(s, s.body, UiWidgetKind::button, st,
                               {UiFriendsAction::toggle_section, 0, s.tab, section.kind});
            if (!h.generation)
                return false;
            section_heading(entries(s)[h.slot], section);
            if (section.kind == UiFriendsSectionKind::incoming && section.allow_clear &&
                section.count >= 5) {
                st = size(64, 28);
                st.horizontal_alignment = UiLayoutAlignment::end;
                if (!add(s, h, UiWidgetKind::button, st, {UiFriendsAction::clear_incoming, 0, s.tab})
                         .generation)
                    return false;
            }
            cursor = item.top + section_height;
            continue;
        }
        const auto& r = visible[row_slot];
        UiFriendsAction available[4]{};
        const auto n = actions(r, available, hidden(s));
        st = size(-1, row_height);
        st.flow = UiLayoutFlow::overlay;
        st.padding = {12, 5, 12, 5};
        const auto row_parent =
            add(s, s.body, UiWidgetKind::button, st, {primary(r), r.user_id, s.tab}, r, true);
        if (!row_parent.generation)
            return false;
        s.visible_rows[row_slot++] = row_parent;
        for (u32 j = 0; j < n; ++j) {
            const bool icon = action_icon(available[j]) != UiIcon::count;
            st = size(action_width(available[j]), icon ? icon_action_extent : 30);
            st.horizontal_alignment = UiLayoutAlignment::end;
            st.vertical_alignment = UiLayoutAlignment::center;
            st.margin.right = 0;
            for (u32 k = j + 1; k < n; ++k)
                st.margin.right += action_step(available[k]);
            if (!add(s, row_parent, UiWidgetKind::button, st, {available[j], r.user_id, s.tab}, r, false,
                     allowed(r, available[j]))
                     .generation)
                return false;
        }
        cursor = item.top + row_height;
    }
    if (!add(s, s.body, UiWidgetKind::container, size(-1, maximum(0.f, extent(s) - cursor))).generation)
        return false;
    if (s.tab == UiFriendsTab::add_friend && s.phase == UiFriendsPhase::ready)
        if (!add(s, s.body, UiWidgetKind::button, size(136, 36), {UiFriendsAction::add_friend, 0, s.tab})
                 .generation)
            return false;
    if (s.phase == UiFriendsPhase::error)
        if (!add(s, s.body, UiWidgetKind::button, size(96, 36), {UiFriendsAction::retry, 0, s.tab})
                 .generation)
            return false;
    if (!s.body.generation ||
        !ui_layout_engine_set_scroll(&s.widgets.layout, s.widgets.storage[s.body.slot].layout,
                                     {0, s.scroll_dips}) ||
        !ui_widget_manager_layout_rectangle(&s.widgets, s.surface).ok())
        return false;
    auto focused = handle(s, s.focus);
    if (focused.generation)
        ui_widget_manager_focus(&s.widgets, focused);
    s.first = first;
    s.count = count;
    s.section_mask = visible_window.mask;
    ++s.revision;
    return true;
}
bool text(UiTextOutput* stream, const UiFriendsScene& s, UiWidgetHandle h, Utf8View label, UiRect r,
          Color color, u32 style = 0, UiTextAlignment align = UiTextAlignment::start) noexcept {
    const auto* node = ui_widget_manager_layout_node(s.widgets, h);
    if (!node)
        return false;
    const auto clip = ui_rect_intersection(r, node->clip_rectangle);
    if (!label.bytes || !ui_rect_has_area(clip))
        return true;
    return ui_text_output_push(stream, {label,
                                        {&s.widgets, h},
                                        r,
                                        clip,
                                        color,
                                        style,
                                        align,
                                        UiTextAlignment::center,
                                        UiTextWrapping::no_wrap,
                                        UiTextOverflow::ellipsis});
}
} // namespace
bool ui_friends_scene_update(UiFriendsScene* scene, const UiFriendsSource* source, UiFriendsTab tab,
                             UiFriendsPhase phase, Utf8View status, UiRect viewport,
                             const UiFriendsContent* content, const UiFriendsSections* sections,
                             const UiFriendsActiveNow* active_now) noexcept {
    if (!scene || tab > UiFriendsTab::add_friend || phase > UiFriendsPhase::error ||
        !ui_rect_valid(viewport) || ui_rect_width(viewport) < 320 || ui_rect_height(viewport) < 200 ||
        ui_rect_height(viewport) > 8192 || status.bytes > 512 || !utf8_validate(status).valid ||
        (source && (source->count > 16384 || (source->count && !source->resolve))) ||
        (content && (content->total_count > 16384 || content->search.text.bytes > 256 ||
                     !utf8_validate(content->search.text).valid ||
                     content->search.anchor > content->search.text.bytes ||
                     content->search.caret > content->search.text.bytes)) ||
        !valid_sections(sections, source, tab) ||
        (sections && sections->hidden_view && content && content->search.active) ||
        (active_now && active_now->count && !active_now->cards))
        return false;
    const bool modal = sections && sections->hidden_view;
    const bool active_visible = active_fits(active_now, viewport, modal);
    const bool active_collapsed = active_visible && active_now->collapsed;
    const bool same_column = scene->active_visible == active_visible && scene->active_collapsed == active_collapsed;
    if (current(*scene) && scene->source == source && scene->tab == tab && scene->phase == phase &&
        ui_rect_equal(scene->viewport, viewport) && scene->status.bytes == status.bytes &&
        scene->status.data == status.data && scene->content == content && scene->sections == sections &&
        scene->active_now == active_now && same_column)
        return true;
    // Cards are painted from the borrowed projection: a new projection needs
    // no list rebuild unless the column appears, disappears or changes width.
    scene->active_now = active_now;
    scene->active_now_revision = active_now ? active_now->revision : 0;
    const bool same_rows =
        scene->active && scene->source == source &&
        scene->source_revision == (source ? source->revision : 0) &&
        scene->source_count == (source ? source->count : 0) && scene->tab == tab && scene->phase == phase &&
        ui_rect_equal(scene->viewport, viewport) && scene->status.bytes == status.bytes &&
        scene->status.data == status.data && scene->search_active == (content && content->search.active) &&
        scene->sections == sections && scene->sections_revision == (sections ? sections->revision : 0) &&
        same_column;
    if (same_rows) {
        scene->content = content;
        scene->content_revision = content ? content->revision : 0;
        count_heading(*scene);
        if (searching(*scene)) {
            auto& clear = scene->widgets.storage[scene->clear_search.slot];
            clear.flags = static_cast<u16>(
                content->search.text.bytes ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable
                                           : ui_widget_none);
            if (!content->search.text.bytes && scene->focus.action == UiFriendsAction::clear_search) {
                scene->focus = {UiFriendsAction::search, 0, tab};
                ui_widget_manager_focus(&scene->widgets, scene->search_field);
            }
        }
        ++scene->revision;
        return true;
    }
    const bool changed_tab = scene->tab != tab;
    scene->source = source;
    scene->source_count = source ? source->count : 0;
    scene->source_revision = source ? source->revision : 0;
    scene->content = content;
    scene->content_revision = content ? content->revision : 0;
    scene->sections = sections;
    scene->sections_revision = sections ? sections->revision : 0;
    scene->search_active = content && content->search.active;
    scene->tab = tab;
    scene->phase = phase;
    scene->status = status;
    scene->viewport = viewport;
    scene->surface = surface(viewport, modal);
    scene->active_visible = active_visible;
    scene->active_collapsed = active_collapsed;
    scene->active_rect = {};
    if (active_visible) {
        const float width = active_collapsed ? kUiFriendsActiveNowCollapsedWidth : kUiFriendsActiveNowWidth;
        scene->surface.right -= width;
        scene->active_rect = {scene->surface.right, viewport.top, viewport.right, viewport.bottom};
    }
    scene->active_hover = 0;
    scene->active = true;
    if (changed_tab)
        scene->scroll_dips = 0;
    scene->scroll_dips = clamp(*scene, scene->scroll_dips);
    UiFriendsTarget accepted{};
    if (!ui_friends_scene_activation(*scene, scene->focus, &accepted))
        scene->focus = {hidden(*scene) ? UiFriendsAction::toggle_hidden : UiFriendsAction::select_tab, 0,
                        tab};
    if (scene->focus.user_id) {
        u32 at = 0;
        UiFriendsRow row{};
        float top = 0;
        if (find(*scene, scene->focus.user_id, &at, &row) && row_top(*scene, at, &top)) {
            const float bottom = top + row_height;
            if (top < scene->scroll_dips)
                scene->scroll_dips = top;
            else if (bottom > scene->scroll_dips + height(*scene))
                scene->scroll_dips = bottom - height(*scene);
            scene->scroll_dips = clamp(*scene, scene->scroll_dips);
        } else
            scene->focus = {hidden(*scene) ? UiFriendsAction::toggle_hidden : UiFriendsAction::select_tab,
                            0, tab};
    }
    if (!rebuild(*scene)) {
        ui_friends_scene_release(scene);
        return false;
    }
    return true;
}
void ui_friends_scene_release(UiFriendsScene* s) noexcept {
    if (!s)
        return;
    const auto revision = s->revision + 1;
    ui_fixed_widget_storage_release(&s->storage);
    *s = {};
    s->revision = revision;
}
bool ui_friends_scene_scroll(UiFriendsScene* s, float delta) noexcept {
    if (!s || !current(*s) || !ui_coordinate_valid(delta))
        return false;
    const auto next = clamp(*s, s->scroll_dips + delta);
    if (next == s->scroll_dips)
        return true;
    const auto next_window = window(*s, next);
    const bool same = next_window.first == s->first && next_window.rows == s->count &&
                      next_window.mask == s->section_mask;
    s->scroll_dips = next;
    if (!same)
        return rebuild(*s);
    if (!ui_layout_engine_set_scroll(&s->widgets.layout, s->widgets.storage[s->body.slot].layout,
                                     {0, next}) ||
        !ui_widget_manager_layout_rectangle(&s->widgets, s->surface).ok())
        return false;
    ++s->revision;
    return true;
}
bool ui_friends_scene_activation(const UiFriendsScene& s, UiFriendsTarget t,
                                 UiFriendsTarget* out) noexcept {
    if (!out)
        return false;
    *out = {};
    if (!current(s) || t.action == UiFriendsAction::none || t.action > UiFriendsAction::active_card)
        return false;
    if (t.action == UiFriendsAction::active_toggle || t.action == UiFriendsAction::active_card) {
        if (!s.active_visible || (t.action == UiFriendsAction::active_toggle) == (t.user_id != 0))
            return false;
        bool listed = t.action == UiFriendsAction::active_toggle;
        for (u32 i = 0; !listed && i < active_card_count(s); ++i)
            listed = s.active_now->cards[i].member_count && s.active_now->cards[i].members[0] == t.user_id;
        if (!listed)
            return false;
        *out = t;
        return true;
    }
    if (t.action == UiFriendsAction::select_tab) {
        if (t.user_id || t.tab > UiFriendsTab::add_friend || hidden(s))
            return false;
        *out = t;
        return true;
    }
    if (t.tab != s.tab)
        return false;
    if (t.action == UiFriendsAction::clear_incoming) {
        if (t.user_id || !sectioned(s) || s.phase != UiFriendsPhase::ready)
            return false;
        for (u32 i = 0; i < s.sections->count; ++i) {
            const auto& section = s.sections->sections[i];
            if (section.kind == UiFriendsSectionKind::incoming && section.count >= 5 &&
                section.allow_clear) {
                *out = t;
                return true;
            }
        }
        return false;
    }
    if (t.action == UiFriendsAction::clear_hidden) {
        if (t.user_id || !hidden(s) || s.phase != UiFriendsPhase::ready || s.sections->hidden_count <= 1)
            return false;
        *out = t;
        return true;
    }
    if (t.action == UiFriendsAction::toggle_hidden) {
        if (t.user_id || !s.sections ||
            (!s.sections->hidden_view &&
             (s.phase != UiFriendsPhase::ready || !s.sections->hidden_available)))
            return false;
        *out = t;
        return true;
    }
    if (t.action == UiFriendsAction::toggle_section) {
        if (t.user_id || !s.sections || s.phase != UiFriendsPhase::ready)
            return false;
        for (u32 i = 0; i < s.sections->count; ++i)
            if (s.sections->sections[i].kind == t.section) {
                *out = t;
                return true;
            }
        return false;
    }
    if (t.action == UiFriendsAction::search || t.action == UiFriendsAction::clear_search) {
        if (t.user_id || !searching(s) ||
            (t.action == UiFriendsAction::clear_search && !s.content->search.text.bytes))
            return false;
        *out = t;
        return true;
    }
    if (t.action == UiFriendsAction::add_friend || t.action == UiFriendsAction::retry) {
        if (t.user_id || (t.action == UiFriendsAction::add_friend
                              ? (s.tab != UiFriendsTab::add_friend || s.phase != UiFriendsPhase::ready)
                              : s.phase != UiFriendsPhase::error))
            return false;
    } else {
        u32 at = 0;
        UiFriendsRow row{};
        float top = 0;
        if (!find(s, t.user_id, &at, &row) || !row_top(s, at, &top) || !allowed(row, t.action))
            return false;
    }
    *out = t;
    return true;
}
bool ui_friends_scene_focus(UiFriendsScene* s, UiFriendsTarget t) noexcept {
    UiFriendsTarget checked{};
    if (!s || !ui_friends_scene_activation(*s, t, &checked))
        return false;
    if (t.user_id) {
        u32 at = 0;
        UiFriendsRow row{};
        if (!find(*s, t.user_id, &at, &row))
            return false;
        float top = 0;
        if (!row_top(*s, at, &top))
            return false;
        const float bottom = top + row_height;
        float offset = s->scroll_dips;
        if (top < offset)
            offset = top;
        else if (bottom > offset + height(*s))
            offset = bottom - height(*s);
        if (!ui_friends_scene_scroll(s, offset - s->scroll_dips))
            return false;
    }
    auto h = handle(*s, t);
    if (!h.generation && t.action == UiFriendsAction::toggle_section && s->sections) {
        float top = 0;
        for (u32 i = 0; i < s->sections->count; ++i) {
            const auto& section = s->sections->sections[i];
            if (section.kind == t.section) {
                if (!ui_friends_scene_scroll(s, top - s->scroll_dips))
                    return false;
                h = handle(*s, t);
                break;
            }
            top +=
                section_height + (section.collapsed ? 0 : static_cast<float>(section.count) * row_height);
        }
    }
    if (!h.generation || !ui_widget_manager_focus(&s->widgets, h))
        return false;
    if (!ui_friends_target_equal(s->focus, t)) {
        s->focus = t;
        ++s->revision;
    }
    return true;
}
void ui_friends_scene_set_focus_visible(UiFriendsScene* s, bool visible) noexcept {
    if (s && s->focus_visible != visible) {
        s->focus_visible = visible;
        ++s->revision;
    }
}
UiFriendsTarget ui_friends_scene_focused(const UiFriendsScene& s) noexcept {
    return current(s) ? s.focus : UiFriendsTarget{};
}
UiFriendsTarget ui_friends_scene_hit_test(const UiFriendsScene& s, float x, float y) noexcept {
    if (!current(s) || !ui_coordinate_valid(x) || !ui_coordinate_valid(y))
        return {};
    if (s.active_visible && ui_rect_contains(s.active_rect, x, y)) {
        if (ui_rect_contains(active_toggle_rect(s), x, y))
            return {UiFriendsAction::active_toggle, 0, s.tab};
        for (u32 i = 0; i < active_card_count(s); ++i) {
            const auto& card = s.active_now->cards[i];
            if (card.member_count && ui_rect_contains(active_card_rect(s, i), x, y))
                return {UiFriendsAction::active_card, card.members[0], s.tab};
        }
        return {};
    }
    for (u32 reverse = s.widgets.count; reverse; --reverse) {
        const u32 i = reverse - 1;
        const auto& e = entries(s)[i];
        if (e.target.action == UiFriendsAction::none)
            continue;
        const UiWidgetHandle h{static_cast<u16>(i), s.widgets.storage[i].generation};
        if (e.target.action == UiFriendsAction::clear_search && !s.content->search.text.bytes)
            continue;
        const auto* node = ui_widget_manager_layout_node(s.widgets, h);
        if (node && ui_rect_contains(node->rectangle, x, y) && ui_rect_contains(node->clip_rectangle, x, y))
            return s.widgets.storage[i].flags & ui_widget_enabled ? e.target : UiFriendsTarget{};
    }
    return {};
}
UiFriendsTarget ui_friends_scene_navigate(UiFriendsScene* s, UiListMove move) noexcept {
    if (!s || !current(*s) || !expanded_count(*s))
        return {};
    u32 at = kUiListNoSelection;
    UiFriendsRow row{};
    if (s->focus.user_id && find(*s, s->focus.user_id, &at, &row))
        at = expanded_rank(*s, at);
    const auto next =
        ui_list_move(expanded_count(*s), at, move, +[](void*, u32) noexcept { return true; }, nullptr);
    if (next == kUiListNoSelection || !resolve(*s, expanded_index(*s, next), &row))
        return {};
    const UiFriendsTarget t{primary(row), row.user_id, s->tab};
    return ui_friends_scene_focus(s, t) ? t : UiFriendsTarget{};
}
UiFriendsTarget ui_friends_scene_page(UiFriendsScene* s, bool reverse) noexcept {
    if (!s || !current(*s) || !expanded_count(*s))
        return {};
    u32 at = s->first;
    UiFriendsRow row{};
    if (s->focus.user_id)
        find(*s, s->focus.user_id, &at, &row);
    at = expanded_rank(*s, at);
    if (at == kUiListNoSelection)
        at = 0;
    const u32 distance = maximum(1u, static_cast<u32>(height(*s) / row_height));
    at = reverse ? (at > distance ? at - distance : 0) : minimum(expanded_count(*s) - 1, at + distance);
    if (!resolve(*s, expanded_index(*s, at), &row))
        return {};
    const UiFriendsTarget t{primary(row), row.user_id, s->tab};
    return ui_friends_scene_focus(s, t) ? t : UiFriendsTarget{};
}
UiScrollbarGeometry ui_friends_scene_scrollbar(const UiFriendsScene& s) noexcept {
    if (!current(s))
        return {};
    const auto* node = ui_widget_manager_layout_node(s.widgets, s.body);
    return node ? ui_scrollbar_geometry(ui_rect_intersection(node->content_rectangle, node->clip_rectangle),
                                        s.scroll_dips, extent(s))
                : UiScrollbarGeometry{};
}
UiFriendsSearchGeometry ui_friends_scene_search_geometry(const UiFriendsScene& s) noexcept {
    if (!current(s) || !searching(s))
        return {};
    const auto* field = ui_widget_manager_layout_node(s.widgets, s.search_field);
    const auto* clear = ui_widget_manager_layout_node(s.widgets, s.clear_search);
    if (!field || !clear)
        return {};
    return {field->rectangle, ui_rect_inset(field->content_rectangle, {10, 6, 36, 6}), clear->rectangle};
}
u32 ui_friends_scene_visible_count(const UiFriendsScene& s) noexcept {
    return current(s) ? s.count : 0;
}
bool ui_friends_scene_visible_row(const UiFriendsScene& s, u32 slot, UiFriendsVisibleRow* out) noexcept {
    if (!out)
        return false;
    *out = {};
    if (!current(s) || slot >= s.count)
        return false;
    const auto h = s.visible_rows[slot];
    const auto* node = ui_widget_manager_layout_node(s.widgets, h);
    if (!node)
        return false;
    const auto& e = entries(s)[h.slot];
    const auto r = node->content_rectangle;
    const float top = (r.top + r.bottom - 32) * .5f;
    const UiRect avatar{r.left, top, r.left + 32, top + 32};
    *out = {e.row,
            {&s.widgets, h},
            node->rectangle,
            avatar,
            {avatar.right - 8, avatar.bottom - 8, avatar.right + 2, avatar.bottom + 2},
            ui_rect_intersection(node->rectangle, node->clip_rectangle)};
    return true;
}
bool ui_friends_scene_presence_paint(const UiFriendsScene& s, const Theme& theme,
                                     PaintStream* paint) noexcept {
    if (!s.active)
        return true;
    if (!paint || !current(s))
        return false;
    const auto* body = ui_widget_manager_layout_node(s.widgets, s.body);
    if (!body)
        return false;
    const auto clip = ui_rect_intersection(body->content_rectangle, body->clip_rectangle);
    if (!paint_stream_push_clip(paint, clip.left, clip.top, clip.right, clip.bottom))
        return false;
    bool ok = true;
    for (u32 i = 0; ok && i < s.count; ++i) {
        UiFriendsVisibleRow row{};
        if (!ui_friends_scene_visible_row(s, i, &row)) {
            ok = false;
            break;
        }
        if (!ui_rect_has_area(ui_rect_intersection(row.presence, row.clip)))
            continue;
        const bool blocked = row.row.kind == UiFriendsKind::blocked;
        ok = ui_presence_paint(paint, row.presence, blocked ? UiPresence::offline : row.row.presence,
                               theme.base_lowest, theme, !blocked && row.row.mobile);
    }
    return paint_stream_pop_clip(paint) && ok;
}
bool ui_friends_scene_paint(const UiFriendsScene& s, const Theme& theme, PaintStream* paint,
                            const UiEditGeometry* edit, u32 dpi, bool include_presence) noexcept {
    if (edit && edit->selection_count > 48)
        return false;
    if (!s.active)
        return true;
    if (!paint || !current(s) ||
        !paint_stream_push_clip(paint, s.viewport.left, s.viewport.top, s.viewport.right,
                                s.viewport.bottom))
        return false;
    bool ok = hidden(s) ? paint_stream_push(paint, s.viewport.left, s.viewport.top, s.viewport.right,
                                            s.viewport.bottom, {0, 0, 0, .65f})
                        : true;
    // The shell owns the full Friends canvas contrast plate. Adding another
    // one here compounds its tint and hides the selected background.
    if (ok && (hidden(s) || !ui_background_active(theme))) {
        if (!hidden(s) && background_effects_active(theme.background))
            ok = ui_background_paint_canvas(paint, s.surface, theme, theme.base_lowest);
        else
            ok = paint_stream_push(paint, s.surface.left, s.surface.top, s.surface.right, s.surface.bottom,
                                   ui_background_surface_color(theme, theme.base_lowest, .94f));
    }
    const auto* body = ui_widget_manager_layout_node(s.widgets, s.body);
    if (!body) {
        paint_stream_pop_clip(paint);
        return false;
    }
    const UiRect body_clip = ui_rect_intersection(body->content_rectangle, body->clip_rectangle);
    bool clip_open = false;
    UiRect previous_clip{};
    for (u32 i = 0; ok && i < s.widgets.count; ++i) {
        const auto& e = entries(s)[i];
        if (e.target.action == UiFriendsAction::none)
            continue;
        const UiWidgetHandle h{static_cast<u16>(i), s.widgets.storage[i].generation};
        const auto* node = ui_widget_manager_layout_node(s.widgets, h);
        if (e.target.action == UiFriendsAction::search ||
            (e.target.action == UiFriendsAction::clear_search && !s.content->search.text.bytes))
            continue;
        if (!node || !ui_rect_has_area(ui_rect_intersection(node->rectangle, node->clip_rectangle)))
            continue;
        const bool selected = e.target.action == UiFriendsAction::select_tab && e.target.tab == s.tab;
        const bool focused = s.focus_visible && s.widgets.focused.slot == h.slot &&
                             s.widgets.focused.generation == h.generation;
        const bool hovered =
            s.widgets.hovered.slot == h.slot && s.widgets.hovered.generation == h.generation;
        const bool pressed =
            hovered && s.widgets.captured.slot == h.slot && s.widgets.captured.generation == h.generation;
        UiRect clip = node->clip_rectangle;
        // Every row primitive stays inside its own rectangle, so rows and actions
        // share the body clip instead of using one global clip slot per row.
        if (e.row.user_id || e.target.action == UiFriendsAction::toggle_section ||
            e.target.action == UiFriendsAction::clear_incoming)
            clip = body_clip;
        if (!clip_open || !ui_rect_equal(previous_clip, clip)) {
            if (clip_open && !paint_stream_pop_clip(paint)) {
                ok = false;
                break;
            }
            clip_open = false;
            if (!paint_stream_push_clip(paint, clip.left, clip.top, clip.right, clip.bottom)) {
                ok = false;
                break;
            }
            previous_clip = clip;
            clip_open = true;
        }
        const UiIcon icon = e.row.user_id && !e.row_label ? action_icon(e.target.action) : UiIcon::count;
        if (icon != UiIcon::count) {
            const auto& r = node->rectangle;
            const float cx = (r.left + r.right) * .5f, cy = (r.top + r.bottom) * .5f;
            const float half = icon_action_extent * .5f, glyph = icon_action_glyph * .5f;
            const bool enabled = (s.widgets.storage[i].flags & ui_widget_enabled) != 0;
            ok = paint_stream_push_rounded_border(
                     paint, cx - half, cy - half, cx + half, cy + half,
                     ui_background_surface_color(theme, hovered || focused ? theme.surface : theme.base_lower,
                                                 pressed ? .5f : hovered ? .4f : .3f),
                     focused ? theme.tokens.border.focus : ui_theme_color_alpha(theme.tokens.border.focus, 0), half, focused ? 2.f : 0.f) &&
                 ui_icon_paint(paint, icon, {cx - glyph, cy - glyph, cx + glyph, cy + glyph},
                               hovered && enabled ? theme.text_primary : theme.text_secondary);
            continue;
        }
        // Idle row/action background and border quads are skipped: the labels
        // already show the affordance and the quads would exceed the shared native
        // paint budget at tall viewports. Tab chrome and hover/focus feedback stay.
        if ((!e.row.user_id && e.target.action != UiFriendsAction::toggle_section) || hovered || focused)
            ok = paint_stream_push_rounded_border(
                paint, node->rectangle.left + 1, node->rectangle.top + 1, node->rectangle.right - 1,
                node->rectangle.bottom - 1,
                (selected || hovered ? ui_background_hover_color : ui_background_surface_color)(
                    theme,
                    selected || hovered ? theme.interactive_hover
                    : e.row_label       ? theme.base_lowest
                                        : theme.surface_high,
                    pressed    ? .32f
                    : selected ? .24f
                    : hovered  ? .20f
                               : .12f),
                focused ? theme.tokens.border.focus : ui_background_surface_color(theme, theme.interactive, .38f),
                theme.scale.radius_sm, focused ? 2.f : 1.f);
        if (ok && e.row_label) {
            constexpr float avatar = 32;
            const auto r = node->content_rectangle;
            const float top = (r.top + r.bottom - avatar) * .5f;
            ok = paint_stream_push_rounded(paint, r.left, top, r.left + avatar, top + avatar, theme.interactive,
                                           avatar * .5f);
        }
    }
    if (clip_open)
        ok = paint_stream_pop_clip(paint) && ok;
    if (ok)
        ok = ui_scrollbar_paint(paint, ui_friends_scene_scrollbar(s), theme);
    if (ok && searching(s)) {
        const auto g = ui_friends_scene_search_geometry(s);
        ok = ui_edit_field_paint(paint, g.field, g.text, s.content->search, edit,
                                 ui_background_surface_color(theme, theme.surface_high, .12f), theme, dpi);
    }
    if (ok && include_presence)
        ok = ui_friends_scene_presence_paint(s, theme, paint);
    if (ok && s.active_visible) {
        const auto& r = s.active_rect;
        // A hairline divides the column from the list.
        ok = paint_stream_push(paint, r.left, r.top, r.left + 1, r.bottom,
                               ui_background_surface_color(theme, theme.interactive, .30f));
        const auto toggle = active_toggle_rect(s);
        const bool toggle_hover = s.active_hover == ~0u;
        if (ok && toggle_hover)
            ok = paint_stream_push_rounded(paint, toggle.left, toggle.top, toggle.right, toggle.bottom,
                                           ui_background_hover_color(theme, theme.interactive_hover, .20f),
                                           theme.scale.radius_sm);
        const float glyph = 9;
        const float cx = (toggle.left + toggle.right) * .5f, cy = (toggle.top + toggle.bottom) * .5f;
        if (ok)
            ok = ui_icon_paint(paint, s.active_collapsed ? UiIcon::chevron_left : UiIcon::chevron_right,
                               {cx - glyph, cy - glyph, cx + glyph, cy + glyph},
                               toggle_hover ? theme.text_primary : theme.text_secondary);
        for (u32 i = 0; ok && i < active_card_count(s); ++i) {
            const auto card = active_card_rect(s, i);
            const bool hovered = s.active_hover == i + 1;
            ok = paint_stream_push_rounded_border(
                paint, card.left, card.top, card.right, card.bottom,
                (hovered ? ui_background_hover_color : ui_background_surface_color)(
                    theme, hovered ? theme.interactive_hover : theme.surface_high, hovered ? .22f : .14f),
                ui_background_surface_color(theme, theme.interactive, .30f), theme.scale.radius_md, 1.f);
            const auto& data = s.active_now->cards[i];
            for (u32 m = 0; ok && m < data.member_count && m < kUiFriendsActiveNowAvatars; ++m) {
                const auto a = active_avatar_rect(card, m);
                // A ring in the card colour separates overlapping avatars.
                ok = paint_stream_push_rounded(paint, a.left - 2, a.top - 2, a.right + 2, a.bottom + 2,
                                               theme.base_lower, active_avatar * .5f + 2) &&
                     paint_stream_push_rounded(paint, a.left, a.top, a.right, a.bottom, theme.interactive,
                                               active_avatar * .5f);
            }
            if (ok) {
                const UiRect icon{card.right - 30, card.top + 18, card.right - 14, card.top + 34};
                ok = ui_icon_paint(paint, data.kind == UiFriendsActiveKind::voice ? UiIcon::speaker : UiIcon::play,
                                   icon, theme.text_secondary);
            }
        }
    }
    return paint_stream_pop_clip(paint) && ok;
}
bool ui_friends_scene_text_output(const UiFriendsScene& s, const Theme& theme, UiTextOutput* stream,
                                  const UiEditGeometry* edit) noexcept {
    if (edit && edit->selection_count > 48)
        return false;
    if (!s.active)
        return true;
    if (!stream || !current(s))
        return false;
    const auto* heading = ui_widget_manager_layout_node(s.widgets, s.heading);
    const auto* status = ui_widget_manager_layout_node(s.widgets, s.status_widget);
    if (!heading || !status ||
        !text(stream, s, s.heading, hidden(s) ? utf8_literal("Other requests") : utf8_literal("Friends"),
              heading->content_rectangle, theme.text_primary, text_style_heading_2 | text_style_bold))
        return false;
    if (hidden(s)) {
        const auto r = status->content_rectangle;
        if (!ui_text_output_push(
                stream,
                {utf8_literal("Requests from people you've ignored and possible spam."),
                 {&s.widgets, s.status_widget},
                 r,
                 ui_rect_intersection(r, status->clip_rectangle),
                 theme.text_secondary,
                 text_style_subtext,
                 UiTextAlignment::start,
                 UiTextAlignment::center,
                 UiTextWrapping::wrap,
                 UiTextOverflow::clip}))
            return false;
    } else if (!text(stream, s, s.status_widget, status_label(s), status->content_rectangle,
                     theme.text_secondary, text_style_subtext))
        return false;
    const auto* count = ui_widget_manager_layout_node(s.widgets, s.count_widget);
    if (!count || !text(stream, s, s.count_widget, {s.count_heading, s.count_heading_bytes},
                        count->content_rectangle, theme.text_secondary, text_style_subtext))
        return false;
    if (searching(s)) {
        const auto g = ui_friends_scene_search_geometry(s);
        if (!ui_edit_field_text_output(stream, {&s.widgets, s.search_field}, g.text, s.content->search,
                                       edit, utf8_literal("Search friends"), theme))
            return false;
    }
    if (s.active_visible && !s.active_collapsed) {
        const auto& r = s.active_rect;
        const auto line = [&](Utf8View label, UiRect area, Color color, u32 style,
                              UiTextWrapping wrapping = UiTextWrapping::no_wrap) noexcept {
            if (!label.bytes)
                return true;
            return ui_text_output_push(stream, {label,
                                                {&s.widgets, s.heading},
                                                area,
                                                ui_rect_intersection(area, r),
                                                color,
                                                style,
                                                UiTextAlignment::start,
                                                UiTextAlignment::center,
                                                wrapping,
                                                wrapping == UiTextWrapping::wrap ? UiTextOverflow::clip
                                                                                 : UiTextOverflow::ellipsis});
        };
        const auto toggle = active_toggle_rect(s);
        if (!line(utf8_literal("Active now"), {r.left + active_padding, r.top + 12, toggle.left - 8, r.top + 44},
                  theme.text_primary, text_style_heading_2 | text_style_bold))
            return false;
        const u32 cards = active_card_count(s);
        if (!cards) {
            const UiRect title{r.left + active_padding, r.top + active_heading + 24, r.right - active_padding,
                               r.top + active_heading + 48};
            const UiRect body{title.left, title.bottom + 4, title.right, title.bottom + 64};
            if (!line(utf8_literal("It's quiet for now"), title, theme.text_primary, text_style_bold) ||
                !line(utf8_literal("When a friend starts an activity, like playing a game or hanging out in voice, "
                                   "it shows here."),
                      body, theme.text_secondary, text_style_subtext, UiTextWrapping::wrap))
                return false;
        }
        for (u32 i = 0; i < cards; ++i) {
            const auto card = active_card_rect(s, i);
            const auto& data = s.active_now->cards[i];
            const float left = card.left + 14, right = card.right - 36;
            const float avatars_right =
                active_avatar_rect(card, minimum(data.member_count, kUiFriendsActiveNowAvatars) - 1).right + 8;
            char more[16];
            u32 more_bytes = 0;
            if (data.total > data.member_count) {
                more[more_bytes++] = '+';
                char digits[10];
                u32 n = 0, value = data.total - data.member_count;
                do {
                    digits[n++] = static_cast<char>('0' + value % 10);
                    value /= 10;
                } while (value && n < 10);
                while (n)
                    more[more_bytes++] = digits[--n];
            }
            if (!line({more, more_bytes}, {avatars_right, card.top + 14, right, card.top + 46}, theme.text_secondary,
                      text_style_subtext) ||
                !line(data.title, {left, card.top + 52, right, card.top + 72}, theme.text_primary, text_style_bold) ||
                !line(data.subtitle, {left, card.top + 72, card.right - 14, card.top + 90}, theme.text_secondary,
                      text_style_subtext))
                return false;
        }
    }
    for (u32 i = 0; i < s.widgets.count; ++i) {
        const auto& e = entries(s)[i];
        if (e.target.action == UiFriendsAction::none)
            continue;
        const UiWidgetHandle h{static_cast<u16>(i), s.widgets.storage[i].generation};
        const auto* node = ui_widget_manager_layout_node(s.widgets, h);
        if (!node)
            return false;
        if (e.target.action == UiFriendsAction::search ||
            (e.target.action == UiFriendsAction::clear_search && !s.content->search.text.bytes))
            continue;
        if (e.row_label) {
            UiFriendsAction available[4]{};
            const u32 n = actions(e.row, available, hidden(s));
            float actions_width = 0;
            for (u32 j = 0; j < n; ++j)
                actions_width += action_step(available[j]);
            auto r = node->content_rectangle;
            r.left += 44;
            r.right = maximum(r.left, r.right - actions_width - 8);
            r.bottom = r.top + 22;
            if (!text(stream, s, h, e.row.display_name.bytes ? e.row.display_name : e.row.username, r,
                      theme.text_primary, text_style_bold))
                return false;
            r.top += 23;
            r.bottom = node->content_rectangle.bottom;
            const auto label =
                e.row.status.bytes ? e.row.status
                : hidden(s) ? (e.row.ignored ? utf8_literal("Ignored") : utf8_literal("Potential spam"))
                : e.row.kind == UiFriendsKind::incoming ? utf8_literal("Incoming friend request")
                : e.row.kind == UiFriendsKind::outgoing ? utf8_literal("Outgoing friend request")
                : e.row.kind == UiFriendsKind::blocked  ? utf8_literal("Blocked")
                : e.row.presence == UiPresence::online  ? utf8_literal("Online")
                : e.row.presence == UiPresence::idle    ? utf8_literal("Idle")
                : e.row.presence == UiPresence::busy    ? utf8_literal("Do Not Disturb")
                : e.row.presence == UiPresence::unknown ? utf8_literal("Status unavailable")
                                                        : utf8_literal("Offline");
            if (!text(stream, s, h, label, r, theme.text_secondary, text_style_subtext))
                return false;
        } else if (e.row.user_id && action_icon(e.target.action) != UiIcon::count) {
            continue; // painted as an icon button
        } else {
            auto r = node->content_rectangle;
            if (e.target.action == UiFriendsAction::toggle_section &&
                e.target.section == UiFriendsSectionKind::incoming && s.sections)
                for (u32 j = 0; j < s.sections->count; ++j) {
                    const auto& section = s.sections->sections[j];
                    if (section.kind == e.target.section && section.allow_clear && section.count >= 5)
                        r.right = maximum(r.left, r.right - 72);
                }
            if (!text(stream, s, h,
                      e.target.action == UiFriendsAction::toggle_section
                          ? Utf8View{e.section_label, e.section_label_bytes}
                      : e.target.action == UiFriendsAction::toggle_hidden
                          ? (s.sections->hidden_view ? utf8_literal("Close")
                                                     : utf8_literal("View other requests"))
                      : e.target.action == UiFriendsAction::clear_hidden         ? utf8_literal("Clear all")
                      : e.target.action == UiFriendsAction::cancel && s.sections ? utf8_literal("X")
                      : e.target.action == UiFriendsAction::select_tab ? tab_label(e.target.tab)
                                                                       : action_label(e.target.action),
                      r,
                      (s.widgets.storage[i].flags & ui_widget_enabled) ? theme.text_primary
                                                                       : theme.text_secondary,
                      text_style_subtext,
                      e.target.action == UiFriendsAction::toggle_section ? UiTextAlignment::start
                                                                         : UiTextAlignment::center))
                return false;
        }
    }
    return true;
}
} // namespace maicord

namespace maicord {
u32 ui_friends_scene_active_avatar_count(const UiFriendsScene& s) noexcept {
    if (!current(s))
        return 0;
    u32 count = 0;
    for (u32 i = 0; i < active_card_count(s); ++i)
        count += minimum(s.active_now->cards[i].member_count, kUiFriendsActiveNowAvatars);
    return count;
}
bool ui_friends_scene_active_avatar(const UiFriendsScene& s, u32 index, UiFriendsActiveAvatar* out) noexcept {
    if (!out || !current(s))
        return false;
    for (u32 i = 0; i < active_card_count(s); ++i) {
        const auto& card = s.active_now->cards[i];
        const u32 members = minimum(card.member_count, kUiFriendsActiveNowAvatars);
        if (index >= members) {
            index -= members;
            continue;
        }
        const auto rect = active_avatar_rect(active_card_rect(s, i), index);
        *out = {card.members[index], rect, ui_rect_intersection(rect, s.active_rect)};
        return out->user_id != 0;
    }
    return false;
}
bool ui_friends_scene_active_hover(UiFriendsScene* s, float x, float y) noexcept {
    if (!s || !current(*s))
        return false;
    u32 hover = 0;
    if (s->active_visible && ui_rect_contains(s->active_rect, x, y)) {
        if (ui_rect_contains(active_toggle_rect(*s), x, y))
            hover = ~0u;
        for (u32 i = 0; !hover && i < active_card_count(*s); ++i)
            if (ui_rect_contains(active_card_rect(*s, i), x, y))
                hover = i + 1;
    }
    if (hover == s->active_hover)
        return false;
    s->active_hover = hover;
    ++s->revision;
    return true;
}
bool ui_friends_scene_icon_action(const UiFriendsScene& s, bool keyboard, float x, float y,
                                  UiWidgetHandle* widget, UiFriendsTarget* target, Utf8View* tooltip) noexcept {
    if (!widget || !target || !tooltip || !current(s))
        return false;
    for (u32 i = 0; i < s.widgets.count; ++i) {
        const auto& e = entries(s)[i];
        if (e.row_label || !e.row.user_id || action_icon(e.target.action) == UiIcon::count)
            continue;
        const UiWidgetHandle h{static_cast<u16>(i), s.widgets.storage[i].generation};
        const auto* node = ui_widget_manager_layout_node(s.widgets, h);
        const bool selected = keyboard ? s.widgets.focused.slot == h.slot && s.widgets.focused.generation == h.generation
                                       : node && ui_rect_contains(node->rectangle, x, y) &&
                                             ui_rect_contains(node->clip_rectangle, x, y);
        if (!selected)
            continue;
        *widget = h;
        *target = e.target;
        *tooltip = e.target.action == UiFriendsAction::open_dm ? utf8_literal("Message") : utf8_literal("More");
        return true;
    }
    return false;
}
} // namespace maicord
