#include "maicord/ui/account/friends_scene.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/theme/background_presentation.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/widgets/presence.hpp"
#include "maicord/ui/text/style.hpp"
#include "maicord/ui/chat/messages/mention_badge.hpp"
#include "maicord/ui/widgets/activity_status.hpp"
#include "maicord/ui/widgets/icon.hpp"
#include "maicord/model/user/profile.hpp"
#include "maicord/ui/account/activity_card.hpp"
#include "maicord/ui/core/transition.hpp"

namespace maicord {
namespace {
constexpr float row_height = 56, header_height = 76, status_height = 32, search_height = 48;
constexpr float section_height = 32;
// Add Friend: the title and hint above the username field, and the field's Send button.
constexpr float intro_height = 64, send_width = 152;
// Add Friend's request note field, and the width of its characters-left counter.
constexpr float note_height = 48, note_counter_width = 36;
// The toolbar: the people glyph and its gap, the title, then a divider before the tabs.
constexpr float toolbar_glyph = 32, toolbar_title = 64, toolbar_requests_title = 156, toolbar_divider = 17;
// Friend row actions: 36-DIP round icon buttons, 10 DIP apart. Text actions
// keep their 48-DIP pill with a 2-DIP gap.
constexpr float icon_action_extent = 36, icon_action_glyph = 20, icon_action_gap = 10;
constexpr float text_action_width = 48, text_action_gap = 2;
constexpr u32 maximum_visible = kUiFriendsMaximumVisibleRows;
// Active Now column layout (DIPs).
constexpr float active_padding = 16, active_heading = 56, active_card_height = 96, active_card_gap = 12;
constexpr float active_avatar = 32, active_avatar_step = 24, active_toggle_extent = 28, active_detail_gap = 12;
constexpr float active_solo_height = 60;
// Count labels packed in one literal ("a\0b", empty ones allowed), looked up by index through their offsets: two
// bytes a label, no relocation and no walk.
template <u32 Count> struct Labels {
    const char* text;
    u16 start[Count + 1];
    template <usize Bytes> consteval Labels(const char (&names)[Bytes]) noexcept : text(names), start{} {
        u32 n = 0;
        for (u32 i = 0; i < Bytes; ++i)
            if (!names[i])
                start[++n] = static_cast<u16>(i + 1);
        if (n != Count)
            start[Count + 1] = 0; // not a constant expression: the literal holds another count of labels
    }
    Utf8View operator[](u32 index) const noexcept {
        return {text + start[index], static_cast<u32>(start[index + 1] - start[index] - 1)};
    }
};
constexpr Labels<6> kTabLabels{"Online\0" "All\0" "Pending\0" "Add Friend\0" "Requests\0" "Spam"};
// By UiFriendsAction; actions named elsewhere (or not at all) are empty.
constexpr Labels<27> kActionLabels{"\0" "\0" "Message\0" "Profile\0" "Accept\0" "Ignore\0" "Cancel\0" "Remove\0"
                                   "Block\0" "Add Friend\0" "Retry\0" "\0" "X\0" "More\0" "\0" "\0" "\0" "Clear all\0"
                                   "Ignore\0" "\0" "\0" "\0" "\0" "\0" "Personalize your request (Optional)\0"
                                   "Accept DM\0" "Ignore"};
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
bool active_fits(const UiFriendsActiveNow* active, const UiRect& viewport, bool modal) noexcept {
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
// One friend's card heads with their avatar beside their name; a shared one lists its members above its title.
bool active_solo(const UiFriendsActiveCard& card) noexcept {
    return card.total == 1 && card.member_count == 1;
}
float active_head(const UiFriendsActiveCard& card) noexcept {
    return active_solo(card) ? active_solo_height : active_card_height;
}
// A card's height: the members and title, then its activity's details.
float active_card_extent(const UiFriendsActiveCard& card) noexcept {
    return active_head(card) + (card.activity ? card.activity->height + active_detail_gap : 0.f);
}
u32 active_card_count(const UiFriendsScene& s) noexcept {
    if (!s.active_visible || s.active_collapsed || !s.active_now)
        return 0;
    const float room = ui_rect_height(s.active_rect) - active_heading - active_padding;
    const u32 count = minimum(s.active_now->count, kUiFriendsActiveNowCards);
    float used = 0;
    u32 fit = 0;
    while (fit < count && used + active_card_extent(s.active_now->cards[fit]) <= room)
        used += active_card_extent(s.active_now->cards[fit++]) + active_card_gap;
    return fit;
}
UiRect active_card_rect(const UiFriendsScene& s, u32 index) noexcept {
    const auto& r = s.active_rect;
    float top = r.top + active_heading;
    for (u32 i = 0; i < index; ++i)
        top += active_card_extent(s.active_now->cards[i]) + active_card_gap;
    return {r.left + active_padding, top, r.right - active_padding,
            top + active_card_extent(s.active_now->cards[index])};
}
// Where a card's activity details start (UiActivityCardSize::compact, no padding of its own).
UiPoint active_detail_origin(const UiRect& card, const UiFriendsActiveCard& data) noexcept {
    return {card.left + 14, card.top + active_head(data)};
}
UiRect active_avatar_rect(const UiRect& card, u32 slot) noexcept {
    const float left = card.left + 14 + slot * active_avatar_step, top = card.top + 14;
    return {left, top, left + active_avatar, top + active_avatar};
}
bool searching(const UiFriendsScene& s) noexcept {
    return s.search_active;
}
bool adding(const UiFriendsScene& s) noexcept {
    return s.tab == UiFriendsTab::add_friend;
}
bool hidden(const UiFriendsScene& s) noexcept {
    return s.sections && s.sections->hidden_view;
}
bool sectioned(const UiFriendsScene& s) noexcept {
    return s.sections && !hidden(s);
}
// A loaded list tab without rows shows its empty state instead of headings.
bool empty_state(const UiFriendsScene& s) noexcept {
    return s.phase == UiFriendsPhase::ready && !adding(s) && !hidden(s) && !s.source_count;
}
bool in_toolbar(const UiFriendsScene& s) noexcept {
    return ui_rect_has_area(s.toolbar);
}
// The title and tabs: in the toolbar, or atop the page (taller for the requests dialog's explanation).
float top_height(const UiFriendsScene& s) noexcept {
    return hidden(s) ? 96 : in_toolbar(s) ? ui_rect_height(s.toolbar) : header_height;
}
// What the page draws in: its viewport and the toolbar above it.
UiRect bounds(const UiFriendsScene& s) noexcept {
    return in_toolbar(s) ? UiRect{minimum(s.viewport.left, s.toolbar.left), s.toolbar.top,
                                  maximum(s.viewport.right, s.toolbar.right), s.viewport.bottom}
                         : s.viewport;
}
float height(const UiFriendsScene& s) noexcept {
    return maximum(1.f, ui_rect_height(s.surface) - top_height(s) - status_height -
                            (searching(s) ? search_height + (adding(s) ? note_height : 0) : 0) -
                            (adding(s) ? intro_height : 0));
}
UiRect surface(const UiRect& viewport, bool modal) noexcept {
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
    if (!sectioned(s) || s.phase != UiFriendsPhase::ready || empty_state(s))
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
    u32 index;
    float top;
    u8 section;
    bool heading;
};
struct Window {
    WindowItem items[maximum_visible + 4];
    u32 count = 0, rows = 0, first = 0, mask = 0;
};
Window window(const UiFriendsScene& s, float scroll) noexcept {
    Window result;
    result.first = logical_count(s);
    if (!sectioned(s)) {
        const auto first = minimum(logical_count(s), static_cast<u32>(scroll / row_height));
        const auto count = minimum(logical_count(s) - first, static_cast<u32>(height(s) / row_height) + 2);
        result.first = first;
        for (u32 i = 0; i < count && i < maximum_visible; ++i)
            result.items[result.count++] = {first + i, static_cast<float>(first + i) * row_height, 0, false};
        result.rows = count;
        return result;
    }
    if (s.phase != UiFriendsPhase::ready || empty_state(s))
        return result;
    float top = 0;
    const float end = scroll + height(s) + row_height;
    for (u32 i = 0; i < s.sections->count; ++i) {
        const auto& section = s.sections->sections[i];
        if (top + section_height > scroll && top < end) {
            result.items[result.count++] = {0, top, static_cast<u8>(i), true};
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
                    result.items[result.count++] = {section.first + j, top + static_cast<float>(j) * row_height,
                                                    static_cast<u8>(i), false};
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
        if ((i && section.kind <= sections->sections[i - 1].kind) || section.first != end ||
            section.count > 16384 - end || section.total_count < section.count ||
            section.total_count > 16384 - total || section.kind > UiFriendsSectionKind::outgoing)
            return false;
        end += section.count;
        total += section.total_count;
    }
    return end == (source ? source->count : 0);
}
Utf8View section_label(UiFriendsSectionKind kind) noexcept {
    return kind == UiFriendsSectionKind::incoming ? utf8_literal("Received") : utf8_literal("Sent");
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
    used += decimal_write(e.section_label + used, section.count);
    e.section_label[used++] = ' ';
    e.section_label[used++] = section.collapsed ? '+' : '-';
    e.section_label_bytes = static_cast<u8>(used);
}
Utf8View tab_label(UiFriendsTab t) noexcept {
    return kTabLabels[static_cast<u32>(t)];
}
Utf8View action_label(UiFriendsAction a) noexcept {
    return kActionLabels[static_cast<u32>(a)];
}
Utf8View status_label(const UiFriendsScene& s) noexcept {
    if (s.status.bytes)
        return s.status;
    if (s.phase == UiFriendsPhase::loading)
        return utf8_literal("Loading friends…");
    if (s.phase == UiFriendsPhase::error)
        return utf8_literal("Friends could not be loaded. Try again.");
    // Add Friend explains where its note shows up until a result replaces it.
    if (adding(s))
        return utf8_literal("What you write here will also show up in your DM if you become friends.");
    return {s.count_heading, s.count_heading_bytes};
}
// The centred message of a list tab without rows (by tab; Add Friend has none), then a search without results.
constexpr Labels<7> kEmptyLabels{
    "There are no friends online at this time. Check back later!\0"
    "There are no current friends. Click \xE2\x80\x9C" "Add Friend\xE2\x80\x9D to send friend requests.\0"
    "There are no pending friend requests. Click \xE2\x80\x9C" "Add Friend\xE2\x80\x9D to send friend requests.\0"
    "\0"
    "There are no pending message requests.\0"
    "There are no spam requests.\0"
    "No results found"};
Utf8View empty_label(const UiFriendsScene& s) noexcept {
    return kEmptyLabels[searching(s) && s.content->search.text.bytes ? 6 : static_cast<u32>(s.tab)];
}
// Online and All without rows offer the Add Friend tab.
bool empty_offers_add(const UiFriendsScene& s) noexcept {
    return empty_state(s) && (s.tab == UiFriendsTab::online || s.tab == UiFriendsTab::all) &&
           !(searching(s) && s.content->search.text.bytes);
}
// An online friend's activity line (custom status, else "Playing X") in place
// of the presence label; without storage only the glyph resolves.
bool activity_line(const UiFriendsScene& s, const UiFriendsRow& row, const Slice<char>& storage,
                   UiActivityStatus* out) noexcept {
    if (row.kind != UiFriendsKind::friend_user || !s.content || !s.content->users ||
        row.presence == UiPresence::offline || row.presence == UiPresence::unknown)
        return false;
    const auto& users = *s.content->users;
    return ui_user_activity_status(users, row.user_id, user_appearance_find_activity(users.appearance, row.user_id),
                                   storage, out);
}
// The row's second line, after the 44-DIP avatar column.
UiRect status_line(const UiRect& content, float actions_width) noexcept {
    return {content.left + 44, content.top + 23, maximum(content.left + 44, content.right - actions_width - 8),
            content.bottom};
}
bool valid_row(const UiFriendsRow& r, UiFriendsTab tab) noexcept {
    if (!r.user_id || r.kind == UiFriendsKind::blocked || r.kind > UiFriendsKind::message_request ||
        r.presence > UiPresence::offline || (!r.display_name.bytes && !r.username.bytes) ||
        r.display_name.bytes > 256 || r.username.bytes > 256 || r.status.bytes > 512 ||
        !utf8_validate(r.display_name).valid || !utf8_validate(r.username).valid || !utf8_validate(r.status).valid)
        return false;
    return ui_friends_tab_requests(tab) ? r.kind == UiFriendsKind::message_request
           : tab == UiFriendsTab::pending
               ? (r.kind == UiFriendsKind::incoming || r.kind == UiFriendsKind::outgoing)
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
    switch (a) {
    case UiFriendsAction::open_dm: return UiIcon::direct_message;
    case UiFriendsAction::more: return UiIcon::ellipsis_vertical;
    case UiFriendsAction::accept: return UiIcon::check;
    case UiFriendsAction::decline:
    case UiFriendsAction::cancel: return UiIcon::close;
    default: return UiIcon::count;
    }
}
// A message request's buttons are text pills sized to their labels.
float action_width(UiFriendsAction a) noexcept {
    return action_icon(a) != UiIcon::count   ? icon_action_extent
           : a == UiFriendsAction::accept_dm ? 88
           : a == UiFriendsAction::ignore_dm ? 64
                                             : text_action_width;
}
float action_step(UiFriendsAction a) noexcept {
    return action_width(a) + (action_icon(a) != UiIcon::count ? icon_action_gap
                              : a == UiFriendsAction::accept_dm || a == UiFriendsAction::ignore_dm ? 8
                                                                                                  : text_action_gap);
}
UiFriendsAction primary(const UiFriendsRow& r) noexcept {
    return r.kind == UiFriendsKind::friend_user || r.kind == UiFriendsKind::message_request ? UiFriendsAction::open_dm
                                                                                            : UiFriendsAction::profile;
}
u32 actions(const UiFriendsRow& r, UiFriendsAction* out, bool hidden_view = false) noexcept {
    u32 n = 0;
    if (r.kind == UiFriendsKind::message_request) {
        // Ignore, then Accept DM, as the official client orders them.
        out[n++] = UiFriendsAction::ignore_dm;
        out[n++] = UiFriendsAction::accept_dm;
        return n;
    }
    if (r.kind == UiFriendsKind::friend_user)
        out[n++] = UiFriendsAction::open_dm;
    else if (r.kind == UiFriendsKind::incoming) {
        out[n++] = UiFriendsAction::accept;
        out[n++] = UiFriendsAction::decline;
    } else
        out[n++] = UiFriendsAction::cancel;
    if (!hidden_view)
        out[n++] = UiFriendsAction::more;
    return n;
}
bool allowed(const UiFriendsRow& r, UiFriendsAction a) noexcept {
    if ((a == UiFriendsAction::more && r.kind != UiFriendsKind::message_request) || a == UiFriendsAction::profile ||
        (a == UiFriendsAction::open_dm && r.kind == UiFriendsKind::message_request))
        return true;
    if (r.busy && a != UiFriendsAction::open_dm)
        return false;
    if (a == UiFriendsAction::ignore || a == UiFriendsAction::unignore)
        return a == UiFriendsAction::unignore ? r.ignored : !r.ignored;
    if (r.kind == UiFriendsKind::friend_user &&
        (a == UiFriendsAction::remove || a == UiFriendsAction::block || a == UiFriendsAction::nickname ||
         a == UiFriendsAction::voice_call))
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
// "Online — 3", "All friends — 12": the list's heading. Pending heads its
// Received and Sent sections instead; an empty tab has none.
void count_heading(UiFriendsScene& s) noexcept {
    s.count_heading_bytes = 0;
    if (adding(s) || hidden(s) || sectioned(s) || empty_state(s))
        return;
    // By tab (Add Friend has no list).
    static constexpr Labels<6> prefixes{"Online \xE2\x80\x94 \0" "All friends \xE2\x80\x94 \0" "All friends \xE2\x80\x94 \0"
                                 "\0" "Pending Requests \xE2\x80\x94 \0" "Spam \xE2\x80\x94 "};
    const auto label = prefixes[static_cast<u32>(s.tab)];
    copy_bytes(s.count_heading, label.data, label.bytes);
    s.count_heading_bytes = static_cast<u8>(label.bytes + decimal_write(s.count_heading + label.bytes, logical_count(s)));
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
// One node's layout: fixed (or, when negative, filling) extent, flow, vertical
// padding (12 DIPs across) and gap, end/centre alignment, a right margin and
// the vertical scroll flag.
struct Box {
    float width, height;
    UiLayoutFlow flow;
    u8 pad_y, gap, bits, margin_right;
};
constexpr u8 box_end = 1, box_center = 2, box_scroll = 4, box_middle = 8;
constexpr UiFriendsRow empty_row{};
UiWidgetHandle emit(UiFriendsScene& s, UiWidgetHandle parent, UiWidgetKind kind, const Box& box,
                    const UiFriendsTarget& target, const UiFriendsRow& row, bool row_label,
                    bool enabled, Utf8View label = {}) noexcept {
    auto st = ui_layout_style();
    st.width = box.width < 0 ? ui_layout_fill() : ui_layout_fixed(box.width);
    st.height = box.height < 0 ? ui_layout_fill() : ui_layout_fixed(box.height);
    st.flags |= ui_layout_node_allow_zero_size | ui_layout_node_clip_children |
                (box.bits & box_scroll ? ui_layout_node_scroll_vertical : 0);
    st.flow = box.flow;
    if (box.pad_y)
        st.padding = {12, static_cast<float>(box.pad_y), 12, static_cast<float>(box.pad_y)};
    st.gap = box.gap;
    if (box.bits & box_end)
        st.horizontal_alignment = UiLayoutAlignment::end;
    if (box.bits & box_middle)
        st.horizontal_alignment = UiLayoutAlignment::center;
    if (box.bits & box_center)
        st.vertical_alignment = UiLayoutAlignment::center;
    st.margin.right = box.margin_right;
    const auto name = label.bytes ? label
                      : row_label ? (row.display_name.bytes ? row.display_name : row.username)
                      : target.action == UiFriendsAction::select_tab ? tab_label(target.tab)
                      : target.action == UiFriendsAction::search
                          ? (adding(s) ? utf8_literal("Enter a username") : utf8_literal("Search friends"))
                      : target.action == UiFriendsAction::add_friend && adding(s) ? utf8_literal("Send Friend Request")
                      : target.action == UiFriendsAction::none       ? utf8_literal("Friends")
                                                                     : action_label(target.action);
    const u32 id = s.widgets.count + 1;
    const auto result = ui_widget_manager_add_keyed(
        &s.widgets, parent, id, kind,
        static_cast<u16>(target.action != UiFriendsAction::none && enabled
                             ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable
                             : ui_widget_none),
        name.data, name.bytes, st);
    if (!result.ok()) {
        s.build_failed = true;
        return ui_widget_invalid_handle();
    }
    auto& entry = entries(s)[result.handle.slot];
    entry.target = target;
    entry.row = row;
    entry.row_label = row_label;
    entry.section_label_bytes = 0;
    return result.handle;
}
UiWidgetHandle add(UiFriendsScene& s, UiWidgetHandle parent, UiWidgetKind kind, const Box& box) noexcept {
    return emit(s, parent, kind, box, {}, empty_row, false, true);
}
UiWidgetHandle add(UiFriendsScene& s, UiWidgetHandle parent, UiWidgetKind kind, const Box& box,
                   UiFriendsAction action, UiFriendsTab tab, bool enabled = true) noexcept {
    return emit(s, parent, kind, box, {action, 0, tab}, empty_row, false, enabled);
}
UiWidgetHandle handle(const UiFriendsScene& s, const UiFriendsTarget& target) noexcept {
    for (u32 i = 0; i < s.widgets.count; ++i)
        if (ui_friends_target_equal(entries(s)[i].target, target))
            return {static_cast<u16>(i), s.widgets.generation};
    return ui_widget_invalid_handle();
}
// The scroll offset that brings the row of `id` into view; false when it has no visible row.
bool reveal(const UiFriendsScene& s, u64 id, float* offset) noexcept {
    u32 at = 0;
    UiFriendsRow row{};
    float top = 0;
    if (!find(s, id, &at, &row) || !row_top(s, at, &top))
        return false;
    const float bottom = top + row_height;
    *offset = s.scroll_dips;
    if (top < *offset)
        *offset = top;
    else if (bottom > *offset + height(s))
        *offset = bottom - height(s);
    return true;
}
bool rebuild(UiFriendsScene& s) noexcept {
    const auto visible_window = window(s, s.scroll_dips);
    const u32 first = visible_window.first, count = visible_window.rows;
    if (count > maximum_visible)
        return false;
    const u32 required = 28 + 4 * count;
    if (s.capacity < required) {
        ui_fixed_widget_storage_release(&s.storage);
        reset_value(&s.widgets);
        s.capacity = 0;
        const u32 capacity = (required + 15) & ~15u;
        if (!ui_scene_open(&s.storage, &s.widgets, capacity, capacity * static_cast<u32>(sizeof(Entry)), alignof(Entry)))
            return false;
        s.capacity = capacity;
    } else
        ui_widget_manager_reset(&s.widgets);
    s.build_failed = false;
    const auto root = add(s, ui_widget_invalid_handle(), UiWidgetKind::container, {-1, -1, UiLayoutFlow::vertical});
    // In the toolbar: the glyph (from the toolbar's left edge), "Friends", a divider, then the tabs; atop the page:
    // the heading over the tabs.
    const bool bar = in_toolbar(s);
    const float lead = bar ? maximum(0.f, s.toolbar.left - s.surface.left) + toolbar_glyph : 0;
    const auto header = add(s, root, UiWidgetKind::container,
                            bar ? Box{-1, top_height(s), UiLayoutFlow::horizontal}
                                : Box{-1, top_height(s), UiLayoutFlow::vertical, 8, 4});
    if (bar)
        add(s, header, UiWidgetKind::container, {lead, -1});
    s.heading = add(s, header, UiWidgetKind::text,
                    bar ? Box{ui_friends_tab_requests(s.tab) ? toolbar_requests_title : toolbar_title, -1} : Box{-1, 24});
    if (bar)
        add(s, header, UiWidgetKind::container, {toolbar_divider, -1});
    if (hidden(s))
        s.status_widget = add(s, header, UiWidgetKind::text, {-1, 48});
    if (!hidden(s)) {
        const auto tabs = add(s, header, UiWidgetKind::container,
                              {-1, 32, UiLayoutFlow::horizontal, 0, 4, static_cast<u8>(bar ? box_center : 0)});
        // Friends: Pending grows by its badge of incoming requests. Message Requests: Requests and Spam, which
        // counts its spam ("Spam (2)").
        const bool requests = ui_friends_tab_requests(s.tab);
        s.badge = s.content ? requests ? s.content->spam_count : s.content->incoming_count : 0;
        s.spam_label_bytes = 0;
        if (requests) {
            const auto spam = utf8_literal("Spam");
            copy_bytes(s.spam_label, spam.data, spam.bytes);
            u32 used = spam.bytes;
            if (s.badge) {
                s.spam_label[used++] = ' ';
                s.spam_label[used++] = '(';
                used += decimal_write(s.spam_label + used, minimum(s.badge, 9999u));
                s.spam_label[used++] = ')';
            }
            s.spam_label_bytes = static_cast<u8>(used);
        }
        const float widths[]{54, 32, 68 + (s.badge ? ui_mention_badge_width(s.badge) + 8 : 0.f), 92};
        const float request_widths[]{84, s.badge ? 92.f : 60.f};
        const float* sizes = requests ? request_widths : widths;
        const u32 first = requests ? static_cast<u32>(UiFriendsTab::requests) : 0, count = requests ? 2 : 4;
        float total = 4.f * (count - 1);
        for (u32 i = 0; i < count; ++i) total += sizes[i];
        const float room = bar ? s.toolbar.right - s.surface.left - lead -
                                     (requests ? toolbar_requests_title : toolbar_title) - toolbar_divider
                               : ui_rect_width(s.viewport) - 40;
        const float tab_scale = minimum(1.f, maximum(1.f, room) / total);
        for (u32 i = 0; i < count; ++i)
            add(s, tabs, UiWidgetKind::button, {sizes[i] * tab_scale, 32}, UiFriendsAction::select_tab,
                static_cast<UiFriendsTab>(first + i));
    }
    // Add Friend: the title and hint, then the username field with its Send button.
    s.intro = s.empty = ui_widget_invalid_handle();
    if (adding(s))
        s.intro = emit(s, root, UiWidgetKind::text, {-1, intro_height, UiLayoutFlow::vertical, 8}, {}, empty_row,
                       false, true, utf8_literal("You can add friends with their Discord username."));
    s.search_field = s.clear_search = s.note_field = ui_widget_invalid_handle();
    if (searching(s)) {
        const auto search_row = add(s, root, UiWidgetKind::container, {-1, search_height, UiLayoutFlow::overlay, 6});
        s.search_field = add(s, search_row, UiWidgetKind::text_field, {-1, -1}, UiFriendsAction::search, s.tab);
        s.clear_search = adding(s)
                             ? add(s, s.search_field, UiWidgetKind::button,
                                   {send_width, 28, UiLayoutFlow::overlay, 0, 0, box_end | box_center, 4},
                                   UiFriendsAction::add_friend, s.tab,
                                   s.content->search.text.bytes != 0 && s.phase == UiFriendsPhase::ready)
                             : add(s, s.search_field, UiWidgetKind::button,
                                   {28, 28, UiLayoutFlow::overlay, 0, 0, box_end | box_center, 4},
                                   UiFriendsAction::clear_search, s.tab, s.content->search.text.bytes != 0);
        // Add Friend's optional note under the username, as the official client asks for it.
        if (adding(s)) {
            const auto note_row = add(s, root, UiWidgetKind::container, {-1, note_height, UiLayoutFlow::overlay, 6});
            s.note_field = add(s, note_row, UiWidgetKind::text_field, {-1, -1}, UiFriendsAction::note, s.tab);
        }
    }
    const auto status_row = add(s, root, UiWidgetKind::container, {-1, status_height, UiLayoutFlow::horizontal, 2, 8});
    if (!hidden(s))
        s.status_widget = add(s, status_row, UiWidgetKind::text, {-1, -1});
    if (s.sections && (s.sections->hidden_available || s.sections->hidden_view)) {
        add(s, status_row, UiWidgetKind::button, {hidden(s) ? 64.f : 132.f, -1}, UiFriendsAction::toggle_hidden, s.tab);
    }
    if (hidden(s) && s.sections->hidden_count > 1)
        add(s, status_row, UiWidgetKind::button, {64, -1}, UiFriendsAction::clear_hidden, s.tab);
    count_heading(s);
    s.body = add(s, root, UiWidgetKind::scroll_view, {-1, -1, UiLayoutFlow::vertical, 0, 0, box_scroll});
    float cursor = 0;
    u32 row_slot = 0;
    u64 ids[maximum_visible]; // the user ids built so far: a repeated user rejects the source
    for (u32 i = 0; i < visible_window.count; ++i) {
        const auto& item = visible_window.items[i];
        if (item.top > cursor)
            add(s, s.body, UiWidgetKind::container, {-1, item.top - cursor});
        if (item.heading) {
            const auto& section = s.sections->sections[item.section];
            const auto h = emit(s, s.body, UiWidgetKind::button, {-1, section_height, UiLayoutFlow::overlay, 2},
                                {UiFriendsAction::toggle_section, 0, s.tab, section.kind}, empty_row, false, true);
            if (h.generation)
                section_heading(entries(s)[h.slot], section);
            if (section.kind == UiFriendsSectionKind::incoming && section.allow_clear &&
                section.count >= 5) {
                add(s, h, UiWidgetKind::button, {64, 28, UiLayoutFlow::overlay, 0, 0, box_end},
                    UiFriendsAction::clear_incoming, s.tab);
            }
            cursor = item.top + section_height;
            continue;
        }
        UiFriendsRow r{};
        bool resolved = resolve(s, item.index, &r);
        for (u32 j = 0; resolved && j < row_slot; ++j)
            resolved = ids[j] != r.user_id;
        if (!resolved) {
            s.active = false; // the source changed or repeats a user: nothing half-built is kept
            return false;
        }
        ids[row_slot] = r.user_id;
        UiFriendsAction available[4]{};
        const auto n = actions(r, available, hidden(s));
        const auto row_parent = emit(s, s.body, UiWidgetKind::button, {-1, row_height, UiLayoutFlow::overlay, 5},
                                     {primary(r), r.user_id, s.tab}, r, true, true);
        s.visible_rows[row_slot++] = row_parent;
        for (u32 j = 0; j < n; ++j) {
            float margin = 0;
            for (u32 k = j + 1; k < n; ++k)
                margin += action_step(available[k]);
            const bool icon = action_icon(available[j]) != UiIcon::count;
            emit(s, row_parent, UiWidgetKind::button,
                 {action_width(available[j]), icon ? icon_action_extent : 30, UiLayoutFlow::overlay, 0, 0,
                  box_end | box_center, static_cast<u8>(margin)},
                 {available[j], r.user_id, s.tab}, r, false, allowed(r, available[j]));
        }
        cursor = item.top + row_height;
    }
    add(s, s.body, UiWidgetKind::container, {-1, maximum(0.f, extent(s) - cursor)});
    if (empty_state(s)) {
        add(s, s.body, UiWidgetKind::container, {-1, 48});
        s.empty = emit(s, s.body, UiWidgetKind::text, {-1, 48}, {}, empty_row, false, true, empty_label(s));
        if (empty_offers_add(s))
            add(s, s.body, UiWidgetKind::button, {120, 36, UiLayoutFlow::overlay, 0, 0, box_middle},
                UiFriendsAction::add_friend, s.tab);
    }
    if (s.phase == UiFriendsPhase::error)
        add(s, s.body, UiWidgetKind::button, {96, 36}, UiFriendsAction::retry, s.tab);
    // The scroll offset indexes the body's slot, so a failed add stops here.
    if (s.build_failed ||
        !ui_widget_manager_set_scroll(&s.widgets, s.body, {0, s.scroll_dips}) ||
        !ui_widget_manager_layout_rectangle(&s.widgets, s.surface).ok())
        return false;
    auto focused = handle(s, s.focus);
    if (focused.generation)
        ui_widget_manager_focus(&s.widgets, focused);
    s.first = first;
    s.count = count;
    s.section_mask = visible_window.mask;
    ++s.revision;
    ++s.layout_revision;
    return true;
}
} // namespace
bool ui_friends_scene_update(UiFriendsScene* scene, const UiFriendsSource* source, UiFriendsTab tab,
                             UiFriendsPhase phase, const Utf8View& status, const UiRect& viewport,
                             const UiFriendsContent* content, const UiFriendsSections* sections,
                             const UiFriendsActiveNow* active_now, const UiRect& toolbar) noexcept {
    if (!scene || tab > UiFriendsTab::spam || phase > UiFriendsPhase::error ||
        !ui_rect_valid(viewport) || ui_rect_width(viewport) < 320 || ui_rect_height(viewport) < 200 ||
        ui_rect_height(viewport) > 8192 || status.bytes > 512 || !utf8_validate(status).valid ||
        (source && (source->count > 16384 || (source->count && !source->resolve))) ||
        (content && (content->total_count > 16384 || content->search.text.bytes > 256 ||
                     !utf8_validate(content->search.text).valid ||
                     content->search.anchor > content->search.text.bytes ||
                     content->search.caret > content->search.text.bytes || content->note.text.bytes > 480 ||
                     !utf8_validate(content->note.text).valid || content->note.anchor > content->note.text.bytes ||
                     content->note.caret > content->note.text.bytes)) ||
        !valid_sections(sections, source, tab) ||
        (sections && sections->hidden_view && content && content->search.active) ||
        (active_now && active_now->count && !active_now->cards))
        return false;
    const bool modal = sections && sections->hidden_view;
    const UiRect bar = !modal && ui_rect_valid(toolbar) && ui_rect_has_area(toolbar) ? toolbar : UiRect{};
    const bool active_visible = active_fits(active_now, viewport, modal);
    const bool active_collapsed = active_visible && active_now->collapsed;
    const bool same_column = scene->active_visible == active_visible && scene->active_collapsed == active_collapsed;
    if (current(*scene) && scene->source == source && scene->tab == tab && scene->phase == phase &&
        ui_rect_equal(scene->viewport, viewport) && ui_rect_equal(scene->toolbar, bar) &&
        scene->status.bytes == status.bytes && scene->status.data == status.data && scene->content == content &&
        scene->sections == sections && scene->active_now == active_now && same_column)
        return true;
    // Cards are painted from the borrowed projection: a new projection needs
    // no list rebuild unless the column appears, disappears or changes width.
    scene->active_now = active_now;
    scene->active_now_revision = active_now ? active_now->revision : 0;
    const bool same_rows =
        scene->active && scene->source == source &&
        scene->source_revision == (source ? source->revision : 0) &&
        scene->source_count == (source ? source->count : 0) && scene->tab == tab && scene->phase == phase &&
        ui_rect_equal(scene->viewport, viewport) && ui_rect_equal(scene->toolbar, bar) && scene->status.bytes == status.bytes &&
        scene->status.data == status.data && scene->search_active == (content && content->search.active) &&
        scene->sections == sections && scene->sections_revision == (sections ? sections->revision : 0) &&
        scene->badge == (content ? ui_friends_tab_requests(tab) ? content->spam_count : content->incoming_count : 0) &&
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
            if (!content->search.text.bytes && (scene->focus.action == UiFriendsAction::clear_search ||
                                                scene->focus.action == UiFriendsAction::add_friend)) {
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
    scene->toolbar = bar;
    scene->surface = surface(viewport, modal);
    if (ui_rect_has_area(bar))
        scene->surface.top = bar.top;
    scene->active_visible = active_visible;
    scene->active_collapsed = active_collapsed;
    reset_value(&scene->active_rect);
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
    const auto focus_fallback = [&]() noexcept {
        scene->focus = {hidden(*scene) ? UiFriendsAction::toggle_hidden : UiFriendsAction::select_tab, 0, tab};
    };
    if (!ui_friends_scene_activation(*scene, scene->focus, &accepted))
        focus_fallback();
    if (scene->focus.user_id) {
        float offset = 0;
        if (reveal(*scene, scene->focus.user_id, &offset))
            scene->scroll_dips = clamp(*scene, offset);
        else
            focus_fallback();
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
    const auto revision = s->revision + 1, layout_revision = s->layout_revision + 1;
    ui_fixed_widget_storage_release(&s->storage);
    reset_value(s);
    s->revision = revision;
    s->layout_revision = layout_revision;
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
    if (!ui_widget_manager_set_scroll(&s->widgets, s->body, {0, next}) ||
        !ui_widget_manager_layout_rectangle(&s->widgets, s->surface).ok())
        return false;
    ++s->revision;
    ++s->layout_revision;
    return true;
}
namespace {
// Whether the scene currently offers the control `t` names.
bool offered(const UiFriendsScene& s, const UiFriendsTarget& t) noexcept {
    if (!current(s) || t.action == UiFriendsAction::none || t.action > UiFriendsAction::ignore_dm)
        return false;
    if (t.action == UiFriendsAction::note)
        return !t.user_id && t.tab == s.tab && adding(s) && searching(s);
    if (t.action == UiFriendsAction::active_toggle || t.action == UiFriendsAction::active_card) {
        if (!s.active_visible || (t.action == UiFriendsAction::active_toggle) == (t.user_id != 0))
            return false;
        bool listed = t.action == UiFriendsAction::active_toggle;
        for (u32 i = 0; !listed && i < active_card_count(s); ++i)
            listed = s.active_now->cards[i].member_count && s.active_now->cards[i].members[0] == t.user_id;
        return listed;
    }
    if (t.action == UiFriendsAction::select_tab)
        return !t.user_id && ui_friends_tab_requests(t.tab) == ui_friends_tab_requests(s.tab) && !hidden(s);
    if (t.tab != s.tab)
        return false;
    if (t.action == UiFriendsAction::clear_incoming) {
        if (t.user_id || !sectioned(s) || s.phase != UiFriendsPhase::ready)
            return false;
        for (u32 i = 0; i < s.sections->count; ++i) {
            const auto& section = s.sections->sections[i];
            if (section.kind == UiFriendsSectionKind::incoming && section.count >= 5 && section.allow_clear)
                return true;
        }
        return false;
    }
    if (t.action == UiFriendsAction::clear_hidden)
        return !t.user_id && hidden(s) && s.phase == UiFriendsPhase::ready && s.sections->hidden_count > 1;
    if (t.action == UiFriendsAction::toggle_hidden)
        return !t.user_id && s.sections &&
               (s.sections->hidden_view || (s.phase == UiFriendsPhase::ready && s.sections->hidden_available));
    if (t.action == UiFriendsAction::toggle_section) {
        if (t.user_id || !s.sections || s.phase != UiFriendsPhase::ready || empty_state(s))
            return false;
        for (u32 i = 0; i < s.sections->count; ++i)
            if (s.sections->sections[i].kind == t.section)
                return true;
        return false;
    }
    if (t.action == UiFriendsAction::search || t.action == UiFriendsAction::clear_search)
        return !t.user_id && searching(s) &&
               (t.action != UiFriendsAction::clear_search || (s.content->search.text.bytes != 0 && !adding(s)));
    if (t.action == UiFriendsAction::add_friend)
        return !t.user_id && (adding(s) ? searching(s) && s.content->search.text.bytes && s.phase == UiFriendsPhase::ready
                                        : empty_offers_add(s));
    if (t.action == UiFriendsAction::retry)
        return !t.user_id && s.phase == UiFriendsPhase::error;
    u32 at = 0;
    UiFriendsRow row{};
    float top = 0;
    return find(s, t.user_id, &at, &row) && row_top(s, at, &top) && allowed(row, t.action);
}
} // namespace
bool ui_friends_scene_activation(const UiFriendsScene& s, const UiFriendsTarget& t,
                                 UiFriendsTarget* out) noexcept {
    if (!out)
        return false;
    reset_value(out);
    if (!offered(s, t))
        return false;
    *out = t;
    return true;
}
bool ui_friends_scene_focus(UiFriendsScene* s, const UiFriendsTarget& t) noexcept {
    UiFriendsTarget checked{};
    if (!s || !ui_friends_scene_activation(*s, t, &checked))
        return false;
    if (t.user_id) {
        float offset = 0;
        if (!reveal(*s, t.user_id, &offset) || !ui_friends_scene_scroll(s, offset - s->scroll_dips))
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
u64 ui_friends_scene_hovered_row(const UiFriendsScene& s) noexcept {
    if (!current(s))
        return 0;
    const auto h = s.widgets.hovered;
    if (!h.generation || h.generation != s.widgets.generation || h.slot >= s.widgets.count)
        return 0;
    // A row's own actions keep its plate.
    return entries(s)[h.slot].row.user_id;
}
bool ui_friends_scene_name_hover(UiFriendsScene* s) noexcept {
    const u64 row = s ? ui_friends_scene_hovered_row(*s) : 0;
    if (!s || row == s->name_row)
        return false;
    s->name_row = row;
    ++s->revision;
    return true;
}
bool ui_friends_scene_contains(const UiFriendsScene& s, float x, float y) noexcept {
    return ui_rect_contains(s.viewport, x, y) || (in_toolbar(s) && ui_rect_contains(s.toolbar, x, y));
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
        const UiWidgetHandle h{static_cast<u16>(i), s.widgets.generation};
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
    return {field->rectangle,
            ui_rect_inset(field->content_rectangle, {10, 6, adding(s) ? send_width + 12 : 36.f, 6}), clear->rectangle};
}
UiFriendsSearchGeometry ui_friends_scene_note_geometry(const UiFriendsScene& s) noexcept {
    const auto* field = current(s) ? ui_widget_manager_layout_node(s.widgets, s.note_field) : nullptr;
    if (!field)
        return {};
    const auto text = ui_rect_inset(field->content_rectangle, {10, 6, note_counter_width + 12, 6});
    return {field->rectangle, text, {text.right + 4, text.top, field->content_rectangle.right - 8, text.bottom}};
}
UiFriendsEditField ui_friends_scene_edit_field(const UiFriendsScene& s) noexcept {
    if (!current(s) || !searching(s))
        return {};
    if (s.content->note.focused && s.note_field.generation)
        return {&s.content->note, ui_friends_scene_note_geometry(s).text};
    return {&s.content->search, ui_friends_scene_search_geometry(s).text};
}
u32 ui_friends_scene_visible_count(const UiFriendsScene& s) noexcept {
    return current(s) ? s.count : 0;
}
bool ui_friends_scene_visible_row(const UiFriendsScene& s, u32 slot, UiFriendsVisibleRow* out) noexcept {
    if (!out)
        return false;
    reset_value(out);
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
    UiPaint p{paint, theme};
    p.clip(ui_rect_intersection(body->content_rectangle, body->clip_rectangle));
    for (u32 i = 0; p && i < s.count; ++i) {
        UiFriendsVisibleRow row{};
        if (!ui_friends_scene_visible_row(s, i, &row)) {
            p.fail();
            break;
        }
        // Message requests show the person's avatar alone.
        if (!ui_rect_intersects(row.presence, row.clip) || row.row.kind == UiFriendsKind::message_request)
            continue;
        // The cut-out takes the colour under the row: the content canvas, or the requests dialog's card.
        p.use<ui_presence_paint>(row.presence, row.row.presence, hidden(s) ? theme.base_lowest : theme.base_low, theme,
                                 row.row.mobile);
    }
    return p.end();
}
bool ui_friends_scene_paint(const UiFriendsScene& s, const Theme& theme, PaintStream* paint,
                            const UiEditGeometry* edit, u32 dpi, bool include_presence) noexcept {
    if (edit && edit->selection_count > 48)
        return false;
    if (!s.active)
        return true;
    if (!current(s))
        return false;
    UiPaint p{paint, theme};
    p.clip(bounds(s));
    // The toolbar's people glyph before the title, and the divider between the title and the tabs.
    if (in_toolbar(s)) {
        const auto& t = s.toolbar;
        const float middle = (t.top + t.bottom) * .5f;
        p.icon(ui_friends_tab_requests(s.tab) ? UiIcon::direct_message : UiIcon::members,
               {t.left, middle - 12, t.left + 24, middle + 12}, theme.text_secondary);
        if (const auto* title = ui_widget_manager_layout_node(s.widgets, s.heading)) {
            const float x = title->rectangle.right + toolbar_divider * .5f;
            p.fill({x, middle - 12, x + 1, middle + 12}, ui_separator_color(theme));
        }
    }
    // The page draws no plate of its own: the shell's content canvas (with its
    // effect and, on gradient themes, the contrast plate) shows through it as
    // through the message view. The requests dialog dims the page under its card.
    if (hidden(s))
        p.fill(s.viewport, {0, 0, 0, .65f})
            .fill(s.surface, ui_background_surface_color(theme, theme.base_lowest, .94f));
    const auto* body = ui_widget_manager_layout_node(s.widgets, s.body);
    if (!body) {
        p.fail();
        return p.end();
    }
    const UiRect body_clip = ui_rect_intersection(body->content_rectangle, body->clip_rectangle);
    bool clip_open = false;
    UiRect previous_clip{};
    for (u32 i = 0; p && i < s.widgets.count; ++i) {
        const auto& e = entries(s)[i];
        if (e.target.action == UiFriendsAction::none)
            continue;
        const UiWidgetHandle h{static_cast<u16>(i), s.widgets.generation};
        const auto* node = ui_widget_manager_layout_node(s.widgets, h);
        if (e.target.action == UiFriendsAction::search || e.target.action == UiFriendsAction::note ||
            (e.target.action == UiFriendsAction::clear_search && !s.content->search.text.bytes) ||
            (e.target.action == UiFriendsAction::add_friend && adding(s)))
            continue;
        if (!node || !ui_rect_intersects(node->rectangle, node->clip_rectangle))
            continue;
        const bool focused = s.focus_visible && ui_widget_handle_equal(s.widgets.focused, h);
        const bool hovered = ui_widget_handle_equal(s.widgets.hovered, h);
        const bool pressed = hovered && ui_widget_handle_equal(s.widgets.captured, h);
        UiRect clip = node->clip_rectangle;
        // Every row primitive stays inside its own rectangle, so rows and actions
        // share the body clip instead of using one global clip slot per row.
        if (e.row.user_id || e.target.action == UiFriendsAction::toggle_section ||
            e.target.action == UiFriendsAction::clear_incoming)
            clip = body_clip;
        if (!clip_open || !ui_rect_equal(previous_clip, clip)) {
            if (clip_open)
                p.unclip();
            p.clip(clip);
            previous_clip = clip;
            clip_open = true;
        }
        // The pills: the Add Friend tab and button (brand filled, a brand tint while the tab is selected) and a
        // message request's Accept DM (primary on Requests) and Ignore buttons.
        const bool select_tab = e.target.action == UiFriendsAction::select_tab;
        const bool add_tab = select_tab && e.target.tab == UiFriendsTab::add_friend;
        const bool request = e.target.action == UiFriendsAction::accept_dm || e.target.action == UiFriendsAction::ignore_dm;
        if (add_tab || request || e.target.action == UiFriendsAction::add_friend) {
            // Tabs and the Add Friend button are always enabled.
            const bool enabled = (s.widgets.storage[i].flags & ui_widget_enabled) != 0;
            Color fill = add_tab && e.target.tab == s.tab ? ui_theme_color_alpha(theme.tokens.accent.brand, .16f)
                         : request && (e.target.action != UiFriendsAction::accept_dm || s.tab != UiFriendsTab::requests)
                             ? ui_background_hover_color(theme, theme.interactive_hover, hovered ? .32f : .20f)
                         : hovered && enabled ? ui_background_hover_color(theme, theme.tokens.accent.brand, .85f)
                                              : theme.tokens.accent.brand;
            if (!enabled)
                fill = ui_theme_color_alpha(fill, .5f);
            p.rounded_border(node->rectangle, fill,
                             focused ? theme.tokens.border.focus : ui_theme_color_alpha(theme.tokens.border.focus, 0),
                             add_tab ? ui_rect_height(node->rectangle) * .5f : theme.scale.radius_sm,
                             focused ? 2.f : 0.f);
            if (!add_tab)
                continue;
        } else if (select_tab)
            // The tab row: the shared pill tab, a plate only while selected or hot.
            p.look<kUiTab>(node->rectangle, static_cast<u8>((hovered ? ui_state_hovered : 0) |
                                                            (focused ? ui_state_focused : 0) |
                                                            (e.target.tab == s.tab ? ui_state_selected : 0)));
        if (select_tab) {
            if (e.target.tab == UiFriendsTab::pending && s.badge) {
                const auto& r = node->rectangle;
                const float w = ui_mention_badge_width(s.badge), top = (r.top + r.bottom - kUiMentionBadgeHeight) * .5f;
                p.rounded({r.right - 8 - w, top, r.right - 8, top + kUiMentionBadgeHeight}, theme.tokens.accent.danger,
                          kUiMentionBadgeHeight * .5f);
            }
            continue;
        }
        const UiIcon icon = e.row.user_id && !e.row_label ? action_icon(e.target.action) : UiIcon::count;
        if (icon != UiIcon::count) {
            const auto& r = node->rectangle;
            const float cx = (r.left + r.right) * .5f, cy = (r.top + r.bottom) * .5f;
            const float half = icon_action_extent * .5f, glyph = icon_action_glyph * .5f;
            const bool enabled = (s.widgets.storage[i].flags & ui_widget_enabled) != 0;
            p.rounded_border({cx - half, cy - half, cx + half, cy + half},
                             ui_background_surface_color(theme, hovered || focused ? theme.surface : theme.base_lower,
                                                         pressed ? .5f : hovered ? .4f : .3f),
                             focused ? theme.tokens.border.focus : ui_theme_color_alpha(theme.tokens.border.focus, 0),
                             half, focused ? 2.f : 0.f)
                .icon(icon, {cx - glyph, cy - glyph, cx + glyph, cy + glyph},
                      !hovered || !enabled                          ? theme.text_secondary
                      : e.target.action == UiFriendsAction::accept  ? theme.tokens.text.positive
                      : e.target.action == UiFriendsAction::decline ||
                              e.target.action == UiFriendsAction::cancel
                          ? theme.tokens.text.danger
                          : theme.text_primary);
            continue;
        }
        // Idle row/action background and border quads are skipped: the labels
        // already show the affordance and the quads would exceed the shared native
        // paint budget at tall viewports. Button chrome and hover/focus feedback stay.
        // A row's plate fades in and out with the host's hover motion (the official client's background ease).
        const float hover_amount =
            e.row_label && s.row_hover && !focused ? ui_motion_hover_amount(*s.row_hover, e.row.user_id) : -1.f;
        const bool fade = hover_amount > 0;
        if (fade || (hover_amount < 0 &&
                     ((!e.row.user_id && e.target.action != UiFriendsAction::toggle_section) || hovered || focused))) {
            // A fading plate is drawn as hovered (it is never focused).
            const bool hot = hovered || fade;
            Color fill = (hot ? ui_background_hover_color : ui_background_surface_color)(
                theme,
                hot           ? theme.interactive_hover
                : e.row_label ? theme.base_lowest
                              : theme.surface_high,
                pressed ? .32f
                : hot   ? .20f
                        : .12f);
            Color border = focused ? theme.tokens.border.focus : ui_background_surface_color(theme, theme.interactive, .38f);
            // The plate's own alpha carries the fade: a flat theme's colour helpers ignore their opacity argument.
            if (fade) {
                fill.alpha *= hover_amount;
                border.alpha *= hover_amount;
            }
            p.rounded_border({node->rectangle.left + 1, node->rectangle.top + 1, node->rectangle.right - 1,
                              node->rectangle.bottom - 1},
                             fill, border, theme.scale.radius_sm, focused ? 2.f : 1.f);
        }
        if (e.row_label) {
            constexpr float avatar = 32;
            const auto r = node->content_rectangle;
            const float top = (r.top + r.bottom - avatar) * .5f;
            p.rounded({r.left, top, r.left + avatar, top + avatar}, theme.interactive, avatar * .5f);
            UiActivityStatus activity{};
            if (!e.row.ignored && activity_line(s, e.row, {}, &activity) && activity.glyph != UiActivityGlyph::none) {
                UiFriendsAction available[4]{};
                const u32 n = actions(e.row, available, hidden(s));
                float actions_width = 0;
                for (u32 j = 0; j < n; ++j)
                    actions_width += action_step(available[j]);
                p.icon(ui_activity_glyph_icon(activity.glyph),
                       ui_activity_glyph_rectangle(status_line(r, actions_width)), theme.text_secondary);
            }
        }
    }
    if (clip_open)
        p.unclip();
    p.scrollbar(ui_friends_scene_scrollbar(s));
    if (searching(s)) {
        const auto g = ui_friends_scene_search_geometry(s);
        const bool note_focused = s.content->note.focused && s.note_field.generation;
        p.use<ui_edit_field_paint>(g.field, g.text, s.content->search, note_focused ? nullptr : edit, theme.base_lowest,
                                   theme, dpi, kUiEditCornerRadius);
        if (s.note_field.generation) {
            const auto n = ui_friends_scene_note_geometry(s);
            p.use<ui_edit_field_paint>(n.field, n.text, s.content->note, note_focused ? edit : nullptr,
                                       theme.base_lowest, theme, dpi, kUiEditCornerRadius);
        }
        // Send Friend Request sits inside the field, primary while there is a name to send.
        if (adding(s)) {
            const auto& w = s.widgets.storage[s.clear_search.slot];
            const bool enabled = (w.flags & ui_widget_enabled) != 0;
            const bool hot = enabled && ui_widget_handle_equal(s.widgets.hovered, s.clear_search);
            const bool focused = s.focus_visible && ui_widget_handle_equal(s.widgets.focused, s.clear_search);
            p.rounded_border(g.clear,
                             enabled ? (hot ? ui_background_hover_color(theme, theme.tokens.accent.brand, .85f)
                                            : theme.tokens.accent.brand)
                                     : ui_theme_color_alpha(theme.tokens.accent.brand, .5f),
                             focused ? theme.tokens.border.focus : ui_theme_color_alpha(theme.tokens.border.focus, 0),
                             theme.scale.radius_sm, focused ? 2.f : 0.f);
        }
    }
    if (p && include_presence && !ui_friends_scene_presence_paint(s, theme, paint))
        p.fail();
    if (p && s.active_visible) {
        const auto& r = s.active_rect;
        // A hairline divides the column from the list.
        p.fill({r.left, r.top, r.left + 1, r.bottom}, ui_separator_color(theme));
        const auto toggle = active_toggle_rect(s);
        const bool toggle_hover = s.active_hover == ~0u;
        if (toggle_hover)
            p.rounded(toggle, ui_background_hover_color(theme, theme.interactive_hover, .20f), theme.scale.radius_sm);
        const float glyph = 9;
        const float cx = (toggle.left + toggle.right) * .5f, cy = (toggle.top + toggle.bottom) * .5f;
        p.icon(s.active_collapsed ? UiIcon::chevron_left : UiIcon::chevron_right,
               {cx - glyph, cy - glyph, cx + glyph, cy + glyph},
               toggle_hover ? theme.text_primary : theme.text_secondary);
        for (u32 i = 0; p && i < active_card_count(s); ++i) {
            const auto card = active_card_rect(s, i);
            const bool hovered = s.active_hover == i + 1;
            p.rounded_border(
                card,
                (hovered ? ui_background_hover_color : ui_background_surface_color)(
                    theme, hovered ? theme.interactive_hover : theme.surface_high, hovered ? .22f : .14f),
                ui_background_surface_color(theme, theme.interactive, .30f), theme.scale.radius_md, 1.f);
            const auto& data = s.active_now->cards[i];
            for (u32 m = 0; p && m < data.member_count && m < kUiFriendsActiveNowAvatars; ++m) {
                const auto a = active_avatar_rect(card, m);
                // A ring in the card colour separates overlapping avatars.
                p.rounded({a.left - 2, a.top - 2, a.right + 2, a.bottom + 2}, theme.base_lower,
                          active_avatar * .5f + 2)
                    .rounded(a, theme.interactive, active_avatar * .5f);
            }
            p.icon(data.kind == UiFriendsActiveKind::voice ? UiIcon::speaker : UiIcon::play,
                   {card.right - 30, card.top + 18, card.right - 14, card.top + 34}, theme.text_secondary);
            // The game's details under the title: art, lines and timer (the host draws the images).
            if (p && data.activity &&
                !ui_activity_card_paint(*data.activity, active_detail_origin(card, data), theme, paint))
                p.fail();
        }
    }
    return p.end();
}
bool ui_friends_scene_text_output(const UiFriendsScene& s, const Theme& theme, UiTextOutput* stream,
                                  const UiEditGeometry* edit) noexcept {
    if (edit && edit->selection_count > 48)
        return false;
    if (!s.active)
        return true;
    if (!current(s))
        return false;
    const auto* heading = ui_widget_manager_layout_node(s.widgets, s.heading);
    const auto* status = ui_widget_manager_layout_node(s.widgets, s.status_widget);
    if (!heading || !status)
        return false;
    const UiRect panel = bounds(s);
    UiTextPen text{stream, s.widgets, panel};
    static constexpr Labels<3> titles{"Friends\0" "Message Requests\0" "Other requests"};
    const bool requests_dialog = hidden(s);
    text.at(s.heading)(s.heading, titles[requests_dialog ? 2 : ui_friends_tab_requests(s.tab) ? 1 : 0],
                       heading->content_rectangle, theme.text_primary,
                       in_toolbar(s) ? text_style_bold : text_style_heading_2 | text_style_bold);
    const auto tone = s.content && !requests_dialog ? s.content->tone : UiFriendsTone::neutral;
    text.at(s.status_widget)(s.status_widget,
                             requests_dialog ? utf8_literal("Requests from people you've ignored and possible spam.")
                                             : status_label(s),
                             status->content_rectangle,
                             tone == UiFriendsTone::positive ? theme.tokens.text.positive
                             : tone == UiFriendsTone::danger ? theme.tokens.text.danger
                                                             : theme.text_secondary,
                             text_style_subtext, requests_dialog ? ui_text_wrap | ui_text_middle | ui_text_clip : 0);
    if (const auto* intro = ui_widget_manager_layout_node(s.widgets, s.intro)) {
        auto r = intro->content_rectangle;
        r.bottom = r.top + 24;
        text.at(s.intro)(s.intro, utf8_literal("Add Friend"), r, theme.text_primary, text_style_bold);
        r.top = r.bottom;
        r.bottom = intro->content_rectangle.bottom;
        text.at(s.intro)(s.intro, utf8_literal("You can add friends with their Discord username."), r,
                         theme.text_secondary, text_style_subtext);
    }
    if (const auto* empty = ui_widget_manager_layout_node(s.widgets, s.empty))
        text.at(s.empty)(s.empty, empty_label(s), empty->content_rectangle, theme.text_secondary, text_style_subtext,
                         ui_text_center | ui_text_wrap | ui_text_middle | ui_text_clip);
    if (searching(s)) {
        const auto g = ui_friends_scene_search_geometry(s);
        const bool note_focused = s.content->note.focused && s.note_field.generation;
        if (text.ok() && !ui_edit_field_text_output(stream, {&s.widgets, s.search_field}, g.text, s.content->search,
                                                    note_focused ? nullptr : edit,
                                                    adding(s) ? utf8_literal("Enter a username")
                                                              : utf8_literal("Search friends"),
                                                    theme))
            text.fail();
        if (adding(s))
            text.at(s.clear_search)(s.clear_search, utf8_literal("Send Friend Request"), g.clear,
                                    theme.tokens.text.on_accent, text_style_subtext | text_style_bold, ui_text_center);
        if (s.note_field.generation) {
            const auto n = ui_friends_scene_note_geometry(s);
            if (text.ok() && !ui_edit_field_text_output(stream, {&s.widgets, s.note_field}, n.text, s.content->note,
                                                        note_focused ? edit : nullptr,
                                                        utf8_literal("Personalize your request (Optional)"), theme))
                text.fail();
            // The characters left of the note's 120, red once none are.
            const u32 left = 120 - minimum(120u, utf8_validate(s.content->note.text).codepoints);
            char digits[4];
            u32 used = 0;
            if (left >= 100) digits[used++] = static_cast<char>('0' + left / 100);
            if (left >= 10) digits[used++] = static_cast<char>('0' + left / 10 % 10);
            digits[used++] = static_cast<char>('0' + left % 10);
            const UiRect clip = ui_rect_intersection(n.clear, panel);
            if (text.ok() && ui_rect_has_area(clip) &&
                !ui_text_output_push_owned(stream, {{digits, used}, {&s.widgets, s.note_field}, n.clear, clip,
                                                    left ? theme.text_secondary : theme.tokens.text.danger,
                                                    text_style_subtext, UiTextAlignment::end, UiTextAlignment::center,
                                                    TextWrapping::no_wrap, TextOverflow::clip, UiTextFace::normal}))
                text.fail();
        }
    }
    if (s.active_visible && !s.active_collapsed) {
        const auto& r = s.active_rect;
        auto line = text.in(r);
        const auto toggle = active_toggle_rect(s);
        line(s.heading, utf8_literal("Active Now"), {r.left + active_padding, r.top + 12, toggle.left - 8, r.top + 44},
             theme.text_primary, text_style_heading_2 | text_style_bold);
        const u32 cards = active_card_count(s);
        if (!cards) {
            const UiRect title{r.left + active_padding, r.top + active_heading + 24, r.right - active_padding,
                               r.top + active_heading + 48};
            const UiRect body{title.left, title.bottom + 4, title.right, title.bottom + 64};
            line(s.heading, utf8_literal("It's quiet for now..."), title, theme.text_primary, text_style_bold);
            line(s.heading,
                 utf8_literal("When a friend starts an activity\xE2\x80\x94like playing a game or hanging out on "
                              "voice\xE2\x80\x94we'll show it here!"),
                 body, theme.text_secondary, text_style_subtext, ui_text_wrap | ui_text_middle | ui_text_clip);
        }
        for (u32 i = 0; text.ok() && i < cards; ++i) {
            const auto card = active_card_rect(s, i);
            const auto& data = s.active_now->cards[i];
            const float left = card.left + 14, right = card.right - 36;
            const float avatars_right =
                active_avatar_rect(card, minimum(data.member_count, kUiFriendsActiveNowAvatars) - 1).right + 8;
            char more[16];
            u32 more_bytes = 0;
            if (data.total > data.member_count) {
                more[more_bytes++] = '+';
                more_bytes += decimal_write(more + more_bytes, data.total - data.member_count);
            }
            line(s.heading, {more, more_bytes}, {avatars_right, card.top + 14, right, card.top + 46},
                 theme.text_secondary, text_style_subtext);
            // One friend: name and activity beside the avatar; shared: the title and members under the avatars.
            const bool solo = active_solo(data);
            const float text_left = solo ? avatars_right + 2 : left, title_top = solo ? card.top + 12 : card.top + 52;
            line(s.heading, data.title, {text_left, title_top, right, title_top + 20}, theme.text_primary,
                 text_style_bold);
            line(s.heading, data.subtitle, {text_left, title_top + 20, card.right - 14, title_top + 38},
                 theme.text_secondary, text_style_subtext);
            if (data.activity && !ui_activity_card_text(*data.activity, active_detail_origin(card, data), theme, text,
                                                        s.heading, card))
                text.fail();
        }
    }
    for (u32 i = 0; text.ok() && i < s.widgets.count; ++i) {
        const auto& e = entries(s)[i];
        if (e.target.action == UiFriendsAction::none)
            continue;
        const UiWidgetHandle h{static_cast<u16>(i), s.widgets.generation};
        const auto* node = ui_widget_manager_layout_node(s.widgets, h);
        if (!node) {
            text.fail();
            break;
        }
        if (e.target.action == UiFriendsAction::search || e.target.action == UiFriendsAction::note ||
            (e.target.action == UiFriendsAction::clear_search && !s.content->search.text.bytes) ||
            (e.target.action == UiFriendsAction::add_friend && adding(s)))
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
            const Utf8View name = e.row.display_name.bytes ? e.row.display_name : e.row.username;
            if (e.row.user_id == s.name_row && e.row.username.bytes && !utf8_equal(name, e.row.username)) {
                // The pointer's row shows the username after the name, muted, as the official client's list does.
                char composed[2 * 128 + 1];
                const u32 name_bytes = utf8_copy_truncated(composed, sizeof(composed) - 1, name);
                composed[name_bytes] = ' ';
                const u32 used = name_bytes + 1 +
                    utf8_copy_truncated(composed + name_bytes + 1, sizeof(composed) - name_bytes - 1, e.row.username);
                s.name_spans[0] = {0, name_bytes, text_style_bold, 0, {}};
                s.name_spans[1] = {name_bytes + 1, used - name_bytes - 1, text_style_subtext, 0, {}};
                const UiRect clip = ui_rect_intersection(r, node->clip_rectangle);
                if (ui_rect_has_area(clip) &&
                    !ui_text_output_push_owned(stream, {{composed, used}, {&s.widgets, h}, r, clip, theme.text_primary, 0,
                                                        UiTextAlignment::start, UiTextAlignment::center,
                                                        TextWrapping::no_wrap, TextOverflow::ellipsis, UiTextFace::normal,
                                                        {}, {s.name_spans, 2}}))
                    text.fail();
            } else
                text.at(h)(h, name, r, theme.text_primary, text_style_bold);
            r.top += 23;
            r.bottom = node->content_rectangle.bottom;
            // An online friend's activity or custom status replaces the presence label.
            char storage[kAppearanceActivityBytes], composed[kAppearanceActivityBytes + 32];
            UiActivityStatus activity{};
            if (!e.row.ignored && activity_line(s, e.row, {storage, sizeof(storage)}, &activity)) {
                u32 used = 0;
                const auto put = [&](const Utf8View& value) noexcept {
                    used += utf8_copy_truncated(composed + used, sizeof(composed) - used, value);
                };
                put(activity.emoji);
                if (activity.emoji.bytes && activity.text.bytes)
                    put(utf8_literal(" "));
                put(activity.text);
                const auto line = ui_activity_glyph_text(r, activity.glyph);
                const UiRect clip = ui_rect_intersection(line, node->clip_rectangle);
                if (used && ui_rect_has_area(clip) && text.ok() &&
                    !ui_text_output_push_owned(stream, {{composed, used}, {&s.widgets, h}, line, clip,
                                                        theme.text_secondary, text_style_subtext,
                                                        UiTextAlignment::start, UiTextAlignment::center,
                                                        TextWrapping::no_wrap, TextOverflow::ellipsis,
                                                        UiTextFace::normal}))
                    text.fail();
                if (used)
                    continue;
            }
            // The row's state: by presence, then the request kinds and the requests dialog's reasons.
            static constexpr Labels<9> states{"Status unavailable\0" "Online\0" "Idle\0" "Do Not Disturb\0" "Offline\0"
                                              "Incoming Friend Request\0" "Outgoing Friend Request\0" "Ignored\0"
                                              "Potential spam"};
            const bool ignored_friend = e.row.ignored && e.row.kind == UiFriendsKind::friend_user;
            text.at(h)(h,
                       e.row.status.bytes && !ignored_friend ? e.row.status
                       : states[ignored_friend                         ? 7
                                : hidden(s)                             ? (e.row.ignored ? 7 : 8)
                                : e.row.kind == UiFriendsKind::incoming ? 5
                                : e.row.kind == UiFriendsKind::outgoing ? 6
                                                                        : static_cast<u32>(e.row.presence)],
                       r, theme.text_secondary, text_style_subtext);
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
            const bool tab = e.target.action == UiFriendsAction::select_tab;
            if (tab && e.target.tab == UiFriendsTab::pending && s.badge) {
                const float w = ui_mention_badge_width(s.badge);
                const float top = (node->rectangle.top + node->rectangle.bottom - kUiMentionBadgeHeight) * .5f;
                const UiRect badge{node->rectangle.right - 8 - w, top, node->rectangle.right - 8,
                                   top + kUiMentionBadgeHeight};
                if (!ui_mention_badge_text_output(s.badge, s.widgets, h, badge, node->clip_rectangle, stream))
                    text.fail();
                r.right = maximum(r.left, badge.left - 4);
            }
            // Primary pills (the Add Friend tab and button, a request's Accept DM) read on the brand fill.
            const bool primary_pill =
                (tab && e.target.tab == UiFriendsTab::add_friend) || e.target.action == UiFriendsAction::add_friend ||
                (e.target.action == UiFriendsAction::accept_dm && s.tab == UiFriendsTab::requests);
            text.at(h)(h,
                       e.target.action == UiFriendsAction::toggle_section
                           ? Utf8View{e.section_label, e.section_label_bytes}
                       : e.target.action == UiFriendsAction::toggle_hidden
                           ? (s.sections->hidden_view ? utf8_literal("Close") : utf8_literal("View other requests"))
                       : e.target.action == UiFriendsAction::clear_hidden ? utf8_literal("Clear all")
                       : tab && e.target.tab == UiFriendsTab::spam && s.spam_label_bytes
                           ? Utf8View{s.spam_label, s.spam_label_bytes}
                       : tab ? tab_label(e.target.tab)
                                                                          : action_label(e.target.action),
                       r,
                       primary_pill ? (tab && e.target.tab == s.tab ? theme.tokens.accent.brand
                                                                    : theme.tokens.text.on_accent)
                       : (s.widgets.storage[i].flags & ui_widget_enabled) && (!tab || e.target.tab == s.tab)
                           ? theme.text_primary
                           : theme.text_secondary,
                       text_style_subtext, e.target.action == UiFriendsAction::toggle_section ? 0 : ui_text_center);
        }
    }
    return text.ok();
}
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
bool ui_friends_scene_active_card(const UiFriendsScene& s, u32 index, UiRect* card, UiPoint* activity) noexcept {
    if (!card || !activity || !current(s) || index >= active_card_count(s))
        return false;
    *card = active_card_rect(s, index);
    *activity = active_detail_origin(*card, s.active_now->cards[index]);
    return true;
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
        const UiWidgetHandle h{static_cast<u16>(i), s.widgets.generation};
        const auto* node = ui_widget_manager_layout_node(s.widgets, h);
        const bool selected = keyboard ? ui_widget_handle_equal(s.widgets.focused, h)
                                       : node && ui_rect_contains(node->rectangle, x, y) &&
                                             ui_rect_contains(node->clip_rectangle, x, y);
        if (!selected)
            continue;
        *widget = h;
        *target = e.target;
        *tooltip = action_label(e.target.action);
        return true;
    }
    return false;
}
} // namespace maicord
