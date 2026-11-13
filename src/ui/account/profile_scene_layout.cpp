#include "profile_scene_internal.hpp"
#include "maicord/ui/text/timestamp_format.hpp"
#include "maicord/protocol/gateway/message_events.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/model/chat/chat_state.hpp"
#include "maicord/model/chat/game_store.hpp"

namespace maicord {
using namespace profile_scene_detail;

namespace profile_scene_detail {
bool panel(const ProfileScene& s) noexcept {
    return s.presentation == ProfilePresentation::panel;
}
bool append_truncated(char* data, u16* used, u32 capacity, const char* text, u32 bytes) noexcept {
    const u32 kept = utf8_copy_truncated(data + *used, capacity - *used, {text, bytes});
    *used = static_cast<u16>(*used + kept);
    return kept == bytes;
}
namespace {
// A clipping container for scrolled or tabbed content.
ProfilePart* clipped(ProfileScene* s, UiWidgetHandle parent, const UiRect& r) noexcept {
    return add(s, parent, r, {}, ProfileAction::none, 0, ProfileLook::plain, ProfileTone::normal, true);
}
// The "could not load" notice with its retry action: its height.
float failed_notice(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept {
    add(s, owner, {left, top, left + width, top + 18}, utf8_literal("Unable to load parts of profile"),
        ProfileAction::retry, kSm, ProfileLook::plain, ProfileTone::muted);
    return 18.f;
}
void scroll_container(ProfileScene* scene, UiWidgetHandle handle, float offset) noexcept {
    auto* node = ui_widget_manager_layout_node(&scene->widgets, handle);
    if (node)
        node->spec.style.flags |= ui_layout_node_scroll_vertical;
    if (!node || !ui_widget_manager_set_scroll(&scene->widgets, handle, {0, offset}))
        labels(scene).failed = true;
}
template <u32 N> bool append_date(ProfileString<N>* out, u64 unix_seconds) noexcept {
    char text[96];
    const auto formatted = format_profile_date(unix_seconds, {text, 96});
    return formatted.ok && append(out, {text, formatted.bytes});
}
} // namespace
u64 activity_now(ProfileScene& s) noexcept {
    const u64 tick = labels(&s).tick_ms;
    return tick ? tick : clock_unix_milliseconds();
}
// The popout (and the editor preview, which is the same card).
bool layout_popout(ProfileScene* s, const UiRect& viewport) noexcept {
    const auto& p = *s->profile;
    const bool live = interactive(*s);
    const float W = kPopoutWidth, pad = s->themed ? 2.f : 0.f;
    const float inner = W - 2 * pad, content = inner - 32, x0 = pad + 16;
    s->panel = blank(s, s->root, {0, 0, W, 1})->widget;
    // Header (relative to the card).
    header_geometry(s, {pad, pad, W - pad, pad + kPopoutBanner}, 16, 61, 80);
    u8 order[kUserActivityMaximum];
    const u32 cards = activity_cards(s, order);
    const float bubble_max = W - pad - 12 - (pad + kPopoutBanner);
    const float bubble =
        status_bubble(s, s->panel, pad + kPopoutBanner, pad + kPopoutBanner - 7, minimum(181.f, bubble_max));
    const float header_bottom = maximum(140.f, bubble ? pad + kPopoutBanner - 10 + bubble : 0.f);
    // Body (scroller): padding 4 16 8, sections 12 apart.
    const float body_top = header_bottom + 8;
    s->body = clipped(s, s->panel, {0, body_top, W, body_top + 1})->widget;
    float y = 4;
    const auto section = [&](float height) noexcept {
        if (height > 0)
            y += height + 12;
    };
    section(name_block(s, s->body, x0, y, content, false));
    if (s->phase == ProfilePhase::failed)
        section(failed_notice(s, s->body, x0, y, content));
    section(notices(s, s->body, x0, y, content, false));
    section(mutuals(s, s->body, x0, y, content));
    section(biography(s, s->body, x0, y, content, true));
    section(game_collection(s, s->body, x0, y, content));
    // Only the first card (the activities, then the voice channel); more peek
    // 8 DIPs below it ("View More").
    const bool voice = voice_card_shown(*s);
    if (cards || voice) {
        const bool peek = cards + voice > 1;
        const float h = cards ? activity_card(s, s->body, x0, y, x0 + content, 0, order[0], activity_now(*s),
                                              ProfileAction::card)
                              : voice_card(s, s->body, x0, y, x0 + content, ProfileAction::card);
        if (peek && h > 0)
            blank(s, s->body, {x0 + 8, y + h - 8, x0 + content - 8, y + h + 8}, ProfileAction::activity,
                  ProfileLook::peek);
        // The peek paints only below the first card, so its order is free.
        section(h + (peek && h > 0 ? 8.f : 0.f));
    }
    // A bot popout lists its Commands after the bio, before roles.
    section(commands_section(s, s->body, x0, y, content));
    section(roles(s, s->body, x0, y, content, true));
    section(plugin_section(s, s->body, x0, y, content));
    section(nameplate_row(s, s->body, x0, y, content));
    const float body_content = y > 4 ? y - 12 + 8 : 12;
    // Footer: Message @name for others, Edit Profile for self.
    const bool self_card = self(*s);
    const bool footer = live && (self_card || can_message(*s));
    const float footer_height = !footer ? 0.f : self_card ? 32.f : 40.f;
    const float natural = body_top + body_content + (footer ? 8 + footer_height + 12 : 0.f) + 4 + pad;
    const float limit = s->presentation == ProfilePresentation::preview ? natural
                                                                        : maximum(200.f, ui_rect_height(viewport) - 20);
    const float H = minimum(natural, limit);
    const float footer_top = H - pad - 4 - 12 - footer_height;
    const float body_bottom = footer ? footer_top - 8 : H - pad - 4;
    if (footer && self_card)
        button(s, s->panel, x0, footer_top, utf8_literal("Edit Profile"), UiIcon::edit, ProfileAction::edit, true, content);
    else if (footer) {
        s->message_field = text_field(s, blank(s, s->panel, {x0, footer_top, x0 + content, footer_top + footer_height},
                                               ProfileAction::message, ProfileLook::input));
        // The field's emoji button (the client's profile message input takes emoji, not GIFs or stickers).
        const float right = x0 + content - 8;
        icon(s, s->panel, {right - 24, footer_top + 8, right, footer_top + 32}, UiIcon::emoji, ProfileAction::emoji);
    }
    if (s->message_failed && footer)
        caption(s, s->panel, {x0, footer_top - 26, x0 + content, footer_top - 6},
                utf8_literal("Could not send. Press Enter to retry."), kXs, ProfileTone::muted);
    // Placement: beside the anchor with 8 DIPs spacing,
    // inverted when it does not fit and nudged into the viewport.
    float x = s->anchor.left, top = s->anchor.top;
    const float margin = 10;
    if (s->presentation != ProfilePresentation::preview) {
        if (s->placement == ProfilePlacement::above) {
            top = s->anchor.top - 8 - H;
            if (top < viewport.top + margin)
                top = s->anchor.bottom + 8;
        } else {
            const float spacing = s->placement == ProfilePlacement::left ? 16.f : 8.f;
            const bool right_fits = s->anchor.right + spacing + W <= viewport.right - margin;
            const bool left_fits = s->anchor.left - spacing - W >= viewport.left + margin;
            const bool right = s->placement == ProfilePlacement::right ? right_fits || !left_fits : !left_fits && right_fits;
            x = right ? s->anchor.right + spacing : s->anchor.left - spacing - W;
        }
        x = maximum(viewport.left + margin, minimum(x, viewport.right - margin - W));
        top = maximum(viewport.top + margin, minimum(top, viewport.bottom - margin - H));
        const float shift = s->intro_offset;
        if (s->placement == ProfilePlacement::above)
            top -= shift;
        else
            x += s->placement == ProfilePlacement::left ? -shift : shift;
    }
    s->rectangle = {x, top, x + W, top + H};
    s->preview_height = H;
    s->card = s->rectangle;
    s->effect_area = ui_rect_inset(s->rectangle, {pad, pad, pad, pad});
    const auto absolute = [&](UiRect r) noexcept {
        return UiRect{x + r.left, top + r.top, x + r.right, top + r.bottom};
    };
    // Header media and controls (relative to the card).
    header_parts(s, ProfileAction::expand);
    header_to_screen(s, x, top);
    if (live && !self_card)
        header_buttons(s, W - pad - 12 - 32, pad + 8);
    place(s, s->panel, {x - viewport.left, top - viewport.top, x - viewport.left + W, top - viewport.top + H});
    place(s, s->body, {0, body_top, W, maximum(body_top + 1, body_bottom)});
    s->scroll_viewport = absolute({0, body_top, W, body_bottom});
    s->content_height = body_content;
    s->scroll = minimum(s->scroll, maximum(0.f, s->content_height - ui_rect_height(s->scroll_viewport)));
    scroll_container(s, s->body, s->scroll);
    if (s->edit.active && !s->note_editing && s->message_field.generation) {
        s->edit_rectangle = absolute({x0, footer_top, x0 + content, footer_top + footer_height});
        s->edit_content = ui_rect_inset(s->edit_rectangle, {12, 2, 12 + 32, 2});
        reset_value(&s->message_placeholder);
        append(&s->message_placeholder, "Message @");
        append(&s->message_placeholder, p.nickname.bytes ? p.nickname.view() : p.display_name.view());
        if (s->edit.focused)
            ui_widget_manager_focus(&s->widgets, s->message_field);
    }
    return finish(s, viewport);
}

namespace {
// Body sections: a text-xs semibold heading, 8 above its content.
float heading(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, const Utf8View& text,
              ProfileTone tone = ProfileTone::subtle) noexcept {
    caption(s, owner, {left, top, left + width, top + 16}, text, kXs | text_style_bold, tone);
    return 24.f;
}
// The account's creation date under "Member Since" ("Created On" for a bot);
// in a server the Discord date gets a mark and the server's join date follows.
float member_since(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width,
                   ProfileTone tone = ProfileTone::subtle) noexcept {
    const auto& p = *s->profile;
    const float h = heading(s, owner, left, top, width, bot(*s) ? utf8_literal("Created On") : utf8_literal("Member Since"),
                            tone);
    auto& l = labels(s);
    const float y = top + h;
    float x = left;
    u64 joined = 0;
    const auto* guild = s->state && p.guild_id ? chat_state_store_find_guild(*s->state, p.guild_id) : nullptr;
    const bool member = guild && p.joined_at.bytes && parse_iso_timestamp({p.joined_at.view(), 0, false}, &joined);
    // A generic calendar icon marks the account date; no brand asset is shipped.
    if (member) {
        icon(s, owner, {x, y + 1, x + 16, y + 17}, UiIcon::calendar, ProfileAction::hint)->hint =
            utf8_literal("Discord");
        x += 20;
    }
    l.member_since[0].bytes = 0;
    append_date(&l.member_since[0], ((p.user_id >> 22) + 1420070400000ull) / 1000);
    const float first = text_width(s, l.member_since[0].view(), width - (x - left), kSm);
    caption(s, owner, {x, y, x + first, y + 18}, l.member_since[0].view(), kSm);
    x += first;
    if (member) {
        x += 8;
        blank(s, owner, {x, y + 7, x + 4, y + 11}, ProfileAction::none, ProfileLook::dot);
        x += 12;
        auto* mark = blank(s, owner, {x, y + 1, x + 16, y + 17}, ProfileAction::hint);
        mark->media = ProfileMediaKind::member_guild;
        mark->hint = chat_state_store_guild_name(*s->state, *guild);
        x += 20;
        l.member_since[1].bytes = 0;
        append_date(&l.member_since[1], joined / 1000);
        if (left + width > x)
            caption(s, owner, {x, y, left + width, y + 18}, l.member_since[1].view(), kSm);
    }
    return h + 18;
}
// "Friends Since" and the day the friendship began (friends only).
float friends_since(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept {
    if (s->relation != 1 || !s->friends_since_ms)
        return 0;
    const float h = heading(s, owner, left, top, width, utf8_literal("Friends Since"));
    auto& line = labels(s).friends_since;
    line.bytes = 0;
    append_date(&line, s->friends_since_ms / 1000);
    caption(s, owner, {left, top + h, left + width, top + h + 18}, line.view(), kSm);
    return h + 18;
}
// Parts one connected account's row can add at most: the tile, the name, its arrow and mark, the date line and
// four statistics with their pictures, with room for wrapped lines.
constexpr u32 kConnectionParts = 24;
// A connected account as the official client's rows: its logo on a white tile, the name (it opens the account's
// page) with an arrow and the verified mark, then "Member since <date>" and the service's statistics, which wrap.
// Returns the row's height.
float connection_row(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, u32 index) noexcept {
    const auto& c = s->profile->connections[index];
    auto& label = labels(s).connection[index];
    const auto kind = profile_connection_kind(c.type.view());
    const Utf8View service = kind == ProfileConnectionKind::unknown ? c.type.view() : profile_connection_name(kind);
    char url[512];
    const bool linked = profile_connection_url(c, {url, sizeof(url)}) != 0;
    u64 created = 0;
    label.since.bytes = 0;
    if (c.created_at.bytes && parse_iso_timestamp({c.created_at.view(), 0, false}, &created)) {
        append(&label.since, "Member since ");
        append_date(&label.since, created / 1000);
    }
    const u32 chip_count = profile_connection_chips(c, label.chips);
    // The tile is 32 DIPs; a name alone is centred on it, else the lines start at its top.
    auto* tile = blank(s, owner, {left, top, left + 32, top + 32}, ProfileAction::hint, ProfileLook::connection_tile);
    tile->media = ProfileMediaKind::connection_logo;
    tile->source = static_cast<u16>(index);
    tile->hint = service;
    unfocusable(s, tile);
    const float text_left = left + 44, right = left + width;
    const float y = top + (label.since.bytes || chip_count ? 0.f : 6.f);
    const float room = maximum(0.f, right - text_left - (linked ? 20.f : 0.f) - (c.verified ? 20.f : 0.f));
    const float name_width = text_width(s, c.name.view(), room, kSm);
    auto* name = add(s, owner, {text_left, y, text_left + name_width + (linked ? 20.f : 0.f), y + 20}, c.name.view(),
                     linked ? ProfileAction::connection : ProfileAction::none, kSm, ProfileLook::plain,
                     ProfileTone::strong);
    name->source = static_cast<u16>(index);
    if (linked)
        icon(s, owner, {text_left + name_width + 4, y + 4, text_left + name_width + 16, y + 16},
             UiIcon::external_link, ProfileAction::none, ProfileTone::strong)
            ->source = 12;
    if (c.verified) {
        const bool twitter = kind == ProfileConnectionKind::twitter && (c.marks & profile_connection_verified);
        const float x = text_left + name_width + (linked ? 24.f : 4.f);
        auto* mark = icon(s, owner, {x, y + 2, x + 16, y + 18}, UiIcon::badge_check, ProfileAction::hint,
                          twitter ? ProfileTone::link : ProfileTone::subtle);
        mark->hint = twitter ? utf8_literal("Verified on Twitter") : utf8_literal("Verified Connection");
        unfocusable(s, mark);
    }
    // What a screen reader reads for the link: the service and the account, and where it leads.
    label.access.bytes = 0;
    append(&label.access, service);
    append(&label.access, ": ");
    append(&label.access, c.name.view());
    if (linked)
        append(&label.access, ", opens in browser");
    float bottom = y + 20;
    if (label.since.bytes) {
        caption(s, owner, {text_left, bottom, right, bottom + 16}, label.since.view(), kXs);
        bottom += 16;
    }
    // Statistics: a game's picture, the figure in bold and its label, 16 apart, wrapping at the row's end.
    float x = text_left, line = bottom + 4;
    for (u32 i = 0; i < chip_count; ++i) {
        const auto& chip = label.chips[i];
        const Utf8View text = chip.text.view();
        const Utf8View figure{text.data, chip.figure_bytes}, rest{text.data + chip.figure_bytes, text.bytes - chip.figure_bytes};
        const float picture = chip.icon != ProfileConnectionChipIcon::none ? 24.f : 0.f;
        const float figure_width = text_width(s, figure, 4096, kXs | text_style_bold);
        const float rest_width = text_width(s, rest, 4096, kXs);
        if (x > text_left && x + picture + figure_width + rest_width > right) {
            x = text_left;
            line += 24;
        }
        if (picture > 0) {
            auto* game = blank(s, owner, {x, line, x + 20, line + 20}, ProfileAction::hint);
            game->media = ProfileMediaKind::connection_chip;
            game->source = static_cast<u16>(chip.icon);
            game->hint = chip.icon == ProfileConnectionChipIcon::dota_2 ? utf8_literal("Dota 2")
                                                                          : utf8_literal("Team Fortress 2");
            unfocusable(s, game);
        }
        const float figure_left = x + picture, rest_left = figure_left + figure_width;
        if (figure.bytes)
            caption(s, owner, {figure_left, line + 2, minimum(rest_left, right), line + 18}, figure,
                    kXs | text_style_bold);
        if (rest.bytes)
            caption(s, owner, {rest_left, line + 2, minimum(rest_left + rest_width, right), line + 18}, rest, kXs);
        x = rest_left + rest_width + 16;
        bottom = line + 20;
    }
    return maximum(32.f, bottom - top);
}
float connections(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept {
    const auto& p = *s->profile;
    if (!p.connection_count)
        return 0;
    float y = top + heading(s, owner, left, top, width, utf8_literal("Connections"));
    // Rows the card has no room for are left out, not the card.
    for (u32 i = 0; i < p.connection_count && i < kConnectionMaximum && s->part_count + kConnectionParts < capacity;
         ++i) {
        if (i)
            y += 16;
        y += connection_row(s, owner, left, y, width, i);
    }
    return y - top;
}
float note_section(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept {
    if (!s->note_visible || self(*s))
        return 0;
    const float h = heading(s, owner, left, top, width, utf8_literal("Note (only visible to you)"));
    s->note_offset = top;
    float y = top + h;
    if (s->note_editing) {
        s->note_field = text_field(s, blank(s, owner, {left - 4, y, left + width, y + 34}, ProfileAction::note,
                                            ProfileLook::input));
        y += 34;
    } else {
        const Utf8View text = s->note_text.bytes ? s->note_text
                              : s->note_loading ? utf8_literal("Loading Note")
                                                : utf8_literal("Click to add a note");
        const float height = maximum(18.f, measure(s, text, width, true, kSm).height);
        add(s, owner, {left, y, left + width, y + height}, text,
            s->note_loading ? ProfileAction::none : ProfileAction::note, kSm, ProfileLook::plain,
            s->note_text.bytes ? ProfileTone::normal : ProfileTone::muted)
            ->wrap = true;
        y += height;
    }
    // A refused save (the note store keeps the draft and tries again on the next edit).
    if (s->note_failed) {
        caption(s, owner, {left, y + 4, left + width, y + 20},
                utf8_literal("Uh-oh, something went wrong. Please try again later."), kXs, ProfileTone::danger);
        y += 20;
    }
    return y - top;
}
// Buttons row by relationship.
float modal_buttons(ProfileScene* s, UiWidgetHandle owner, float left, float top) noexcept {
    if (s->relation == 4)
        return 0;
    float x = left;
    const auto next = [&](float w) noexcept { x += w + 8; };
    if (self(*s)) {
        next(button(s, owner, x, top, utf8_literal("Edit Profile"), UiIcon::edit, ProfileAction::edit, true));
    } else if (!bot(*s) && s->relation == 0) {
        next(button(s, owner, x, top, utf8_literal("Add Friend"), UiIcon::user_plus, ProfileAction::add_friend, true));
        next(button(s, owner, x, top, {}, UiIcon::direct_message, ProfileAction::message, false));
    } else {
        next(button(s, owner, x, top, utf8_literal("Message"), UiIcon::direct_message, ProfileAction::message, true));
        if (!bot(*s) && (s->relation == 1 || s->relation == 3))
            next(button(s, owner, x, top, {}, s->relation == 1 ? UiIcon::user_check : UiIcon::recent,
                        ProfileAction::relationship, false));
    }
    next(button(s, owner, x, top, {}, UiIcon::ellipsis, ProfileAction::more, false));
    return 32;
}
// The Board lists the game widgets that hold games (application widgets are not drawn).
bool board_shown(const UserProfile& p) noexcept {
    for (u32 i = 0; i < p.widget_count; ++i)
        if (p.widgets[i].kind != ProfileWidgetKind::application && p.widgets[i].game_count)
            return true;
    return false;
}
// The tabs shown, in the official order.
u32 tab_list(const ProfileScene& s, ProfileTab* tabs) noexcept {
    u32 count = 0;
    if (board_shown(*s.profile))
        tabs[count++] = ProfileTab::widgets;
    tabs[count++] = ProfileTab::activity;
    if (!self(s)) {
        if (!bot(s))
            tabs[count++] = ProfileTab::friends;
        tabs[count++] = ProfileTab::servers;
    }
    return count;
}
// The empty tabs' lines (the official client picks one at random; here the user picks it).
constexpr char kNoFriendsLines[] =
    "No mutual friends yet – perfect time to make a new connection\0"
    "Join the same servers to meet more mutual friends 🤝\0"
    "Your friend networks are like separate galaxies – time for them to collide!\0"
    "Friendship multiverse detected – time for a crossover episode! 🎬\0"
    "Your friends haven’t met their friends yet – time for a legendary crossover! ⚡️";
constexpr char kNoServersLines[] =
    "Time to impress them with your taste...drop an invite link to your favourite server\0"
    "No shared server = no shared emoji options...time to create a server for all your favourite memes together\0"
    "Join the same servers to unlock shared emotes and more VC channels to hang in\0"
    "Ask them to invite you to their favourite server so you can bond\0"
    "Shared server inventory: empty but full of potential ✨";
constexpr char kNoActivityLines[] =
    "Looks like this profile is still a work in progress – send them a message to say hi!\0"
    "Profile under construction 🚧 – ask them about their plans for decorating\0"
    "Still waiting to see what they’ll share next – why not break the ice?\0"
    "No activity shared yet – drop them a friendly hello to start chatting\0"
    "Gaming console offline – they must be achievement-hunting in secret\0"
    "Their gaming console isn’t linked 🎯 ask them what their gaming setup looks like\0"
    "They must be touching grass...ask them what that’s like\0"
    "They’re shy 👉 👈...say hey to break the ice\0"
    "So mysterious! Ask them to tell you a secret\0"
    "They’re going for that minimalist aesthetic ✨ ask them for tips\0"
    "They left their profile blank to keep you guessing 🎭 tell them you think they’re super mysterious\0"
    "They believe less is more... way less in this case 😅 ask them how they do it\0"
    "They appear to be practising the art of digital zen 🧘‍♀️ take a deep breath and say hey/namaste\0"
    "They’re in stealth mode 🥷 ask them about their ninja training techniques\0"
    "Guess they’re cosplaying as a ghost 👻 leave some cookies to lure them out";
Utf8View quip(const ProfileScene& s, const char* lines, u32 count) noexcept {
    return c_string_list_entry(lines, static_cast<u32>((s.profile->user_id >> 22) % count));
}
struct EmptyButton {
    Utf8View label;
    UiIcon glyph;
    ProfileAction action;
};
// An empty tab: its heading and line centred (at most 320 wide, 8 apart), its
// primary buttons 24 below, the block centred in the panel. Returns its bottom.
float empty_tab(ProfileScene* s, UiWidgetHandle owner, float width, float height, const Utf8View& title,
                const Utf8View& body, const EmptyButton* buttons = nullptr, u32 count = 0) noexcept {
    const float text_w = minimum(width, 320.f), left = (width - text_w) * .5f;
    const float title_h = maximum(20.f, measure(s, title, text_w, true, kMd | text_style_bold).height);
    const float body_h = maximum(18.f, measure(s, body, text_w, true, kSm).height);
    float widths[2]{}, row = 0;
    for (u32 i = 0; i < count; ++i) {
        widths[i] = text_width(s, buttons[i].label, 400, kSm | text_style_bold) + 32 +
                    (buttons[i].glyph != UiIcon::count ? 24.f : 0.f);
        row += widths[i] + (i ? 8.f : 0.f);
    }
    float y = maximum(0.f, (height - (title_h + 8 + body_h + (count ? 56.f : 0.f))) * .5f);
    auto* heading_part = add(s, owner, {left, y, left + text_w, y + title_h}, title, ProfileAction::none,
                             kMd | text_style_bold, ProfileLook::plain, ProfileTone::strong);
    heading_part->wrap = heading_part->centered = true;
    y += title_h + 8;
    auto* line = add(s, owner, {left, y, left + text_w, y + body_h}, body, ProfileAction::none, kSm);
    line->wrap = line->centered = true;
    y += body_h;
    if (!count)
        return y;
    y += 24;
    float x = (width - row) * .5f;
    for (u32 i = 0; i < count; ++i) {
        button(s, owner, x, y, buttons[i].label, buttons[i].glyph, buttons[i].action, true, widths[i]);
        x += widths[i] + 8;
    }
    return y + 32;
}
// Activity: "Current activity" over the cards 8 apart (the voice channel
// last), else the empty state.
float activity_tab(ProfileScene* s, UiWidgetHandle owner, float width, float height, const u8* order,
                   u32 cards) noexcept {
    if (cards || voice_card_shown(*s)) {
        float y = heading(s, owner, 0, 0, width, utf8_literal("Current activity"));
        for (u32 i = 0; i < cards; ++i) {
            const float used = activity_card(s, owner, 0, y, width, i, order[i], activity_now(*s), ProfileAction::none);
            if (used > 0)
                y += used + 8;
        }
        const float voice = voice_card(s, owner, 0, y, width, ProfileAction::none);
        return voice > 0 ? y + voice : y - 8;
    }
    if (self(*s)) {
        // Registered Games only: linking an account is not offered.
        const EmptyButton add_game{utf8_literal("Add Game"), UiIcon::count, ProfileAction::add_game};
        return empty_tab(s, owner, width, height, utf8_literal("You don't have any activity here"),
                         utf8_literal("Connect your accounts to share your in-game status and activity. You're in "
                                      "control of what you share."),
                         &add_game, interactive(*s) ? 1u : 0u);
    }
    const auto& p = *s->profile;
    auto& title = labels(s).empty_heading;
    title.bytes = 0;
    append(&title, p.nickname.bytes       ? p.nickname.view()
                   : p.display_name.bytes ? p.display_name.view()
                                          : p.username.view());
    append(&title, " doesn’t have any activity to share here");
    const EmptyButton message{utf8_literal("Message"), UiIcon::direct_message, ProfileAction::message};
    return empty_tab(s, owner, width, height, title.view(), quip(*s, kNoActivityLines, 15), &message,
                     interactive(*s) && can_message(*s) ? 1u : 0u);
}
// The Board's widget titles and the game tags it shows (the others stay hidden), by tag bit.
Utf8View widget_title(ProfileWidgetKind kind) noexcept {
    switch (kind) {
    case ProfileWidgetKind::favorite_games:
        return utf8_literal("Favorite Game");
    case ProfileWidgetKind::current_games:
        return utf8_literal("Games in Rotation");
    case ProfileWidgetKind::played_games:
        return utf8_literal("Games I Like");
    default:
        return utf8_literal("Want to Play");
    }
}
constexpr char kTagNames[] = "\0\0Casual\0\0Intermediate\0Expert\0Better than you\0Obsessed\0Love it\0Kind of love it\0"
                             "Kind of hate it\0Rage quitting\0\0\0\0Looking for group\0Open to play\0Looking for tips\0"
                             "Open to teach\0Looking to discuss";
Utf8View game_name(const ProfileScene& s, u64 id) noexcept {
    return s.games ? game_store_name(*s.games, id) : Utf8View{};
}
// A game's cover (its icon without one), named by the game in its tooltip.
void game_cover(ProfileScene* s, UiWidgetHandle owner, const UiRect& r, u64 id) noexcept {
    auto* cover = blank(s, owner, r, ProfileAction::game, ProfileLook::box);
    cover->target = id;
    cover->media = ProfileMediaKind::widget_game;
    cover->hint = game_name(*s, id);
}
// A game's tags on one row of pills, "+N" for those that do not fit.
void tag_row(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, u32 tags,
             ProfileString<8>* more) noexcept {
    float x = left;
    u32 hidden = 0;
    for (u32 t = 0; t < profile_widget_tag_count; ++t) {
        const auto text = c_string_list_entry(kTagNames, t);
        if (!(tags & (1u << t)) || !text.bytes)
            continue;
        const float w = text_width(s, text, width, kXs) + 16;
        if (hidden || x + w > left + width) {
            ++hidden;
            continue;
        }
        add(s, owner, {x, top, x + w, top + 20}, text, ProfileAction::none, kXs, ProfileLook::pill);
        x += w + 4;
    }
    if (hidden) {
        const auto text = plus_count(more, hidden);
        const float w = text_width(s, text, width, kXs) + 16;
        add(s, owner, {x, top, x + w, top + 20}, text, ProfileAction::none, kXs, ProfileLook::pill);
    }
}
// A detailed game: the cover in the grid's first column, then its name (its
// game profile), the comment and the tags, centred beside the cover.
float game_card(ProfileScene* s, UiWidgetHandle owner, float top, float width, float cell, float gap,
                const ProfileWidgetGame& game, u32 index) noexcept {
    const float cover_h = cell * 4 / 3, x = cell + gap, w = width - x;
    game_cover(s, owner, {0, top, cell, top + cover_h}, game.game_id);
    const auto name = game_name(*s, game.game_id);
    const float comment_h =
        game.comment.bytes ? maximum(18.f, measure(s, game.comment.view(), w, true, kSm).height) + 4 : 0.f;
    bool tagged = false;
    for (u32 t = 0; t < profile_widget_tag_count; ++t)
        tagged |= ((game.tags >> t) & 1) && c_string_list_entry(kTagNames, t).bytes;
    const float details = (name.bytes ? 24.f : 0.f) + comment_h + (tagged ? 28.f : 0.f) - 4;
    float y = top + maximum(0.f, (cover_h - details) * .5f);
    if (name.bytes) {
        const float name_w = text_width(s, name, w, kMd | text_style_bold);
        add(s, owner, {x, y, x + name_w, y + 20}, name, ProfileAction::game, kMd | text_style_bold, ProfileLook::plain,
            ProfileTone::strong)
            ->target = game.game_id;
        y += 24;
    }
    if (comment_h > 0) {
        caption(s, owner, {x, y, x + w, y + comment_h - 4}, game.comment.view(), kSm, ProfileTone::muted)->wrap = true;
        y += comment_h;
    }
    if (tagged)
        tag_row(s, owner, x, y + 4, w, game.tags, &labels(s).tags_more[index]);
    return maximum(cover_h, details);
}
// "Show more" / "Show less" under a widget's games.
float more_toggle(ProfileScene* s, UiWidgetHandle owner, float top, float width, u32 widget, bool expanded) noexcept {
    const auto label = expanded ? utf8_literal("Show less") : utf8_literal("Show more");
    const float w = text_width(s, label, width, kSm | text_style_bold) + 16;
    auto* toggle = add(s, owner, {(width - w) * .5f, top, (width + w) * .5f, top + 24}, label,
                       ProfileAction::widget_more, kSm | text_style_bold, ProfileLook::plain, ProfileTone::subtle);
    toggle->centered = true;
    toggle->source = static_cast<u16>(widget);
    return 24;
}
// The Board: each game widget's title, then its favorite (one detailed game),
// its games in rotation (two detailed, the rest behind Show more) or a grid
// of five covers a row (eight, the rest behind Show more); widgets 24 apart.
float board_tab(ProfileScene* s, UiWidgetHandle owner, float width) noexcept {
    const auto& p = *s->profile;
    const float gap = width >= 266 ? 12.f : 8.f, cell = (width - 4 * gap) / 5, cover_h = cell * 4 / 3;
    float y = 0;
    for (u32 w = 0; w < p.widget_count; ++w) {
        const auto& widget = p.widgets[w];
        const u32 count = widget.first_game < p.widget_game_count
                              ? minimum<u32>(widget.game_count, p.widget_game_count - widget.first_game)
                              : 0;
        if (widget.kind == ProfileWidgetKind::application || !count)
            continue;
        if (y > 0)
            y += 24;
        caption(s, owner, {0, y, width, y + 20}, widget_title(widget.kind), kMd | text_style_bold,
                ProfileTone::strong);
        y += 32;
        const auto* games = p.widget_games + widget.first_game;
        const bool favorite = widget.kind == ProfileWidgetKind::favorite_games;
        const bool detailed = favorite || widget.kind == ProfileWidgetKind::current_games;
        const bool expanded = (s->widgets_expanded >> w) & 1;
        const u32 folded = favorite ? 1u : detailed ? 2u : 8u;
        const u32 shown = favorite ? 1u : expanded ? count : minimum(count, folded);
        for (u32 i = 0; i < shown; ++i) {
            if (detailed) {
                y += game_card(s, owner, y, width, cell, gap, games[i], widget.first_game + i) + 12;
                continue;
            }
            const float cx = (i % 5) * (cell + gap), cy = y + (i / 5) * (cover_h + gap);
            game_cover(s, owner, {cx, cy, cx + cell, cy + cover_h}, games[i].game_id);
        }
        y = detailed ? y - 12 : y + ((shown + 4) / 5) * (cover_h + gap) - gap;
        if (!favorite && count > folded)
            y += 12 + more_toggle(s, owner, y + 12, width, w, expanded);
    }
    return y;
}
// The tab panel: the tab row, a rule, then the selected tab's scrolled content.
void tab_panel(ProfileScene* s, const UiRect& panel, float bottom_pad, const u8* order, u32 cards) noexcept {
    const auto& p = *s->profile;
    auto& l = labels(s);
    ProfileTab tabs[4];
    const u32 count = tab_list(*s, tabs);
    s->tab = 0;
    while (s->tab < count && tabs[s->tab] != s->section)
        ++s->tab;
    if (s->tab == count) {
        s->tab = 0;
        if (s->phase != ProfilePhase::loading)
            s->section = tabs[0];
    }
    float x = panel.left;
    const float tab_top = panel.top + 20;
    for (u32 i = 0; i < count; ++i) {
        Utf8View title = utf8_literal("Board");
        ProfileAction action = ProfileAction::widgets;
        if (tabs[i] == ProfileTab::activity) {
            title = utf8_literal("Activity");
            action = ProfileAction::activity;
        } else if (tabs[i] != ProfileTab::widgets) {
            const bool servers = tabs[i] == ProfileTab::servers;
            auto& text = l.tab_text[servers];
            const u32 n = servers ? p.mutual_guild_count : p.mutual_friend_count;
            if (n)
                mutual_count(&text, n, servers);
            else {
                text.bytes = 0;
                append(&text, servers ? utf8_literal("No Mutual Servers") : utf8_literal("No Mutual Friends"));
            }
            title = text.view();
            action = servers ? ProfileAction::servers : ProfileAction::friends;
        }
        const float w = text_width(s, title, panel.right - x, kSm | text_style_bold);
        if (w <= 0)
            break;
        auto* tab = blank(s, s->panel, {x, tab_top, x + w, tab_top + 30}, action, ProfileLook::tab);
        caption(s, s->panel, {x, tab_top, x + w + 1, tab_top + 14}, title, kSm | text_style_bold,
                i == s->tab ? (s->themed ? ProfileTone::strong : ProfileTone::link) : ProfileTone::subtle);
        tab->source = static_cast<u16>(i);
        x += w + 24;
    }
    blank(s, s->panel, {panel.left, tab_top + 30, panel.right, tab_top + 31}, ProfileAction::none, ProfileLook::rule);
    const UiRect list{panel.left, tab_top + 31 + 20, panel.right, panel.bottom - bottom_pad};
    if (!ui_rect_has_area(list))
        return;
    s->secondary_viewport = {s->rectangle.left + list.left, s->rectangle.top + list.top,
                             s->rectangle.left + list.right, s->rectangle.top + list.bottom};
    auto* container = clipped(s, s->panel, list);
    const float width = ui_rect_width(list) - 16, height = ui_rect_height(list);
    const auto tab = tabs[s->tab];
    if (tab == ProfileTab::widgets)
        s->secondary_height = board_tab(s, container->widget, width);
    else if (tab == ProfileTab::activity)
        s->secondary_height = activity_tab(s, container->widget, width, height, order, cards);
    else {
        const bool friends = tab == ProfileTab::friends;
        const u32 n = friends ? p.mutual_friends_count : p.mutual_guild_count;
        s->secondary_height = n * 50.f;
        s->secondary_scroll = maximum(0.f, minimum(s->secondary_scroll, maximum(0.f, s->secondary_height - height)));
        const u32 first = static_cast<u32>(s->secondary_scroll / 50),
                  last = minimum(n, first + static_cast<u32>(height / 50) + 2);
        for (u32 i = first; i < last; ++i) {
            const auto& peer = friends ? p.mutual_friends[i] : p.mutual_guilds[i];
            const auto name = peer_name(*s, friends, peer);
            const float y = i * 50.f + 1;
            peer_row(s, container->widget, friends, i, {0, y, width, y + 48}, {4, y + 4, 44, y + 44});
            // A friend's activity line; a server's line is their nickname there.
            const auto* status =
                friends && s->users ? user_appearance_find_activity(s->users->appearance, peer.id) : nullptr;
            const Utf8View second = status ? status->activity.view() : friends ? Utf8View{} : peer.nick.view();
            const float name_top = second.bytes ? y + 6 : y + 14;
            if (name.bytes)
                caption(s, container->widget, {54, name_top, width - 4, name_top + 20}, name, kMd);
            if (second.bytes)
                caption(s, container->widget, {54, y + 26, width - 4, y + 42}, second, kXs, ProfileTone::subtle);
        }
        // Empty once the profile has loaded (it carries the lists).
        if (!n && s->phase != ProfilePhase::loading)
            s->secondary_height =
                friends ? empty_tab(s, container->widget, width, height,
                                    utf8_literal("You don’t have any friends in common"),
                                    quip(*s, kNoFriendsLines, 5))
                        : empty_tab(s, container->widget, width, height,
                                    utf8_literal("You don’t have any servers in common"),
                                    quip(*s, kNoServersLines, 5));
    }
    s->secondary_scroll = maximum(0.f, minimum(s->secondary_scroll, maximum(0.f, s->secondary_height - height)));
    scroll_container(s, container->widget, s->secondary_scroll);
}

} // namespace
// The full profile modal.
bool layout_modal(ProfileScene* s, const UiRect& viewport) noexcept {
    const float vw = ui_rect_width(viewport), vh = ui_rect_height(viewport);
    const bool wide = vw >= 929, tall = vh >= 785;
    const float mw = wide ? minimum(962.f, vw - 96) : vw;
    float mh = tall ? minimum(800.f, vh - 96) : vh;
    mh = maximum(mh, minimum(550.f, vh));
    s->modal_rounded = wide && tall;
    const float x = viewport.left + (vw - mw) * .5f, top = viewport.top + (vh - mh) * .5f;
    s->rectangle = {x, top, x + mw, top + mh};
    const float pad = vw >= 1025 ? 48.f : 32.f;
    const float card_w = minimum(maximum(300.f, .455f * vw - (wide ? .91f * 48 : 0.f)), 400.f);
    const float cp = card_w <= 380 ? 24.f : 32.f;
    const float size = card_w <= 350 ? 96.f : 120.f;
    s->panel = clipped(s, s->root, {x - viewport.left, top - viewport.top, x - viewport.left + mw, top - viewport.top + mh})
                   ->widget;
    const UiRect card{pad, pad, pad + card_w, mh};
    s->card = {x + card.left, top + card.top, x + card.right, top + card.bottom};
    s->effect_area = s->card;
    header_geometry(s, {card.left, card.top, card.right, card.top + kModalBanner}, card.left + cp,
                    card.top + kModalBanner - size * .5f, size);
    // The card's header: banner, avatar, decoration, then the status bubble.
    header_parts(s, ProfileAction::none);
    const float bubble_left = card.left + cp + size + 20, bubble_top = card.top + kModalBanner - 8;
    u8 order[kUserActivityMaximum];
    const u32 cards = activity_cards(s, order);
    const float bubble = status_bubble(s, s->panel, bubble_left, bubble_top, card.right - 16 - bubble_left);
    const float header_bottom = maximum(s->avatar.bottom + 12, bubble ? bubble_top + bubble + 8 : 0.f);
    // Card body: gap 24, padding 4 cp cp (end cp - 8), scrolled with fade.
    s->body = clipped(s, s->panel, {card.left, header_bottom, card.right, mh})->widget;
    const float content = card_w - cp - (cp - 8);
    float y = 0;
    const auto section = [&](float height) noexcept {
        if (height > 0)
            y += height + 24;
    };
    section(name_block(s, s->body, cp, y, content, true));
    if (s->phase == ProfilePhase::failed)
        section(failed_notice(s, s->body, cp, y, content));
    section(notices(s, s->body, cp, y, content, true));
    section(interactive(*s) ? modal_buttons(s, s->body, cp, y) : 0.f);
    // Streamer Mode hides the bio, the connections and the note.
    section(s->hide_personal ? 0.f : biography(s, s->body, cp, y, content, false));
    // Bot info: the bio, its Commands, then Created On.
    section(commands_section(s, s->body, cp, y, content));
    section(member_since(s, s->body, cp, y, content));
    section(friends_since(s, s->body, cp, y, content));
    if (roles_shown(*s)) {
        const float h = heading(s, s->body, cp, y, content, utf8_literal("Roles"));
        y += h + roles(s, s->body, cp, y + h, content, false) + 24;
    }
    section(plugin_section(s, s->body, cp, y, content));
    section(s->hide_personal ? 0.f : connections(s, s->body, cp, y, content));
    section(s->hide_personal ? 0.f : note_section(s, s->body, cp, y, content));
    s->content_height = (y > 0 ? y - 24 : 0) + cp;
    s->scroll_viewport = {s->card.left, top + header_bottom, s->card.right, s->card.bottom};
    if (s->scroll_to_note) {
        s->scroll_to_note = false;
        s->scroll = s->note_offset - 8;
    }
    s->scroll = maximum(0.f, minimum(s->scroll, maximum(0.f, s->content_height - ui_rect_height(s->scroll_viewport))));
    scroll_container(s, s->body, s->scroll);
    // Right: the tab panel beside the card (gutter 32).
    reset_value(&s->secondary_viewport);
    s->secondary_height = 0;
    const UiRect panel_rect{card.right + 32, pad, mw - (pad - 16), mh};
    if (ui_rect_width(panel_rect) > 120)
        tab_panel(s, panel_rect, wide ? 16.f : 32.f, order, cards);
    // Close (X): top 8, right 8.
    if (interactive(*s))
        icon(s, s->panel, {mw - 40, 8, mw - 8, 40}, UiIcon::close, ProfileAction::close, ProfileTone::normal,
             ProfileLook::header_button);
    header_to_screen(s, x, top);
    s->preview_height = mh;
    return finish(s, viewport);
}
// A bot's popular commands (resolved by the host): "Commands", then small
// secondary "/name" chips 6 apart that wrap, each hinting its description.
float commands_section(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept {
    const u32 count = minimum<u32>(s->commands.count, kProfileCommandMaximum);
    if (!count || !bot(*s))
        return 0;
    // The Commands section keeps the default heading colour.
    const float h = heading(s, owner, left, top, width, utf8_literal("Commands"));
    auto& l = labels(s);
    float x = left, y = top + h;
    for (u32 i = 0; i < count; ++i) {
        const auto& command = s->commands[i];
        auto& label = l.command_label[i];
        label.bytes = 0;
        append(&label, "/");
        append(&label, command.name);
        const float text = text_width(s, label.view(), width - 24, kSm);
        const float w = minimum(width, maximum(60.f, text + 24));
        if (x > left && x + w > left + width) {
            x = left;
            y += 32 + 6;
        }
        auto* chip = blank(s, owner, {x, y, x + w, y + 32}, ProfileAction::command, ProfileLook::secondary);
        chip->source = static_cast<u16>(i);
        chip->hint = command.description;
        caption(s, owner, {x + (w - text) * .5f, y, x + (w + text) * .5f, y + 32}, label.view(), kSm);
        x += w + 6;
    }
    return y + 32 - top;
}
// The DM profile panel: a card
// inset in its slot (anchor), blocks 20 apart 16 from the sides, the body
// scrolling between the header and the "View Full Profile" bar. Geometry
// below is relative to the card's inside (within its border).
bool layout_panel(ProfileScene* s, const UiRect& viewport) noexcept {
    const UiRect outer = ui_rect_inset(s->anchor, {kPanelInset, kPanelInset, kPanelInset, kPanelInset});
    const UiRect slot = ui_rect_inset(outer, {kPanelBorder, kPanelBorder, kPanelBorder, kPanelBorder});
    const float W = ui_rect_width(slot), H = ui_rect_height(slot);
    if (W < 120 || H < kPanelBanner + kPanelFooter)
        return false;
    const float x0 = 16, content = W - 32;
    s->rectangle = outer;
    s->card = s->effect_area = slot;
    s->panel = blank(s, s->root, {slot.left - viewport.left, slot.top - viewport.top, slot.right - viewport.left,
                                  slot.bottom - viewport.top})
                   ->widget;
    header_geometry(s, {0, 0, W, kPanelBanner}, 16 - kPanelBorder, kPanelAvatarTop, 80);
    header_parts(s, ProfileAction::expand);
    u8 order[kUserActivityMaximum];
    const u32 cards = activity_cards(s, order);
    const float bubble_left = x0 + 80 + 12;
    const float bubble = status_bubble(s, s->panel, bubble_left, kPanelBanner - 7, minimum(181.f, W - 12 - bubble_left));
    // Header: banner plus 39 (the avatar's lower half), 8 below.
    const float header_bottom = maximum(kPanelBanner + 39, bubble ? kPanelBanner - 7 + bubble : 0.f) + 8;
    const float footer_top = H - kPanelFooter;
    s->body = clipped(s, s->panel, {0, header_bottom, W, maximum(header_bottom + 1, footer_top)})->widget;
    float y = 0;
    const auto section = [&](float height) noexcept {
        if (height > 0)
            y += height + 20;
    };
    if (s->phase == ProfilePhase::failed)
        section(failed_notice(s, s->body, x0, y, content));
    // Name and username, then the mutuals row 8 below.
    const float name = name_block(s, s->body, x0, y, content, false);
    const float shared = mutuals(s, s->body, x0, y + name + 8, content);
    section(name + (shared ? 8 + shared : 0));
    section(notices(s, s->body, x0, y, content, false));
    for (u32 i = 0; i < cards; ++i)
        section(activity_card(s, s->body, x0, y, x0 + content, i, order[i], activity_now(*s), ProfileAction::card));
    section(voice_card(s, s->body, x0, y, x0 + content, ProfileAction::card));
    // The bio without a heading (three lines; "View Full Profile" below is the
    // way to the rest), the account date, then the cards (a bot's commands).
    section(biography(s, s->body, x0, y, content, true, false));
    section(game_collection(s, s->body, x0, y, content));
    section(member_since(s, s->body, x0, y, content, ProfileTone::strong));
    section(commands_section(s, s->body, x0, y, content));
    section(plugin_section(s, s->body, x0, y, content));
    // Streamer Mode hides the connections here as in the full profile.
    section(s->hide_personal ? 0.f : connections(s, s->body, x0, y, content));
    const float body_content = y > 0 ? y - 20 + 16 : 16;
    // "View Full Profile": a full-width bar, no rule above it.
    blank(s, s->panel, {8, footer_top + 6, W - 8, H - 6}, ProfileAction::expand, ProfileLook::row);
    constexpr auto label = utf8_literal("View Full Profile");
    const float label_width = text_width(s, label, W - 32, kSm | text_style_bold) + 1;
    caption(s, s->panel, {(W - label_width) * .5f, footer_top + 1, (W + label_width) * .5f, H}, label,
            kSm | text_style_bold);
    if (interactive(*s) && !self(*s))
        header_buttons(s, W - 12 - 32, 12);
    const auto absolute = [&](UiRect r) noexcept {
        return UiRect{slot.left + r.left, slot.top + r.top, slot.left + r.right, slot.top + r.bottom};
    };
    header_to_screen(s, slot.left, slot.top);
    s->scroll_viewport = absolute({0, header_bottom, W, footer_top});
    s->content_height = body_content;
    s->scroll = minimum(s->scroll, maximum(0.f, s->content_height - ui_rect_height(s->scroll_viewport)));
    s->preview_height = H;
    scroll_container(s, s->body, s->scroll);
    return finish(s, viewport);
}
} // namespace profile_scene_detail
} // namespace maicord
