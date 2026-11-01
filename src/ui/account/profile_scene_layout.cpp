#include "profile_scene_internal.hpp"

namespace maicord {
using namespace profile_scene_detail;

namespace profile_scene_detail {
bool panel(const ProfileScene& s) noexcept {
    return s.presentation == ProfilePresentation::panel;
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
    // Only the first card; more peek 8 DIPs below it ("View More").
    if (cards) {
        const bool peek = cards > 1;
        const float h = activity_card(s, s->body, x0, y, x0 + content, 0, order[0], activity_now(*s), ProfileAction::card);
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
    else if (footer)
        s->message_field = text_field(s, blank(s, s->panel, {x0, footer_top, x0 + content, footer_top + footer_height},
                                               ProfileAction::message, ProfileLook::input));
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
    header_parts(s, s->panel, {0, 0, 0, 0}, ProfileAction::expand);
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
        s->edit_content = ui_rect_inset(s->edit_rectangle, {12, 2, 12, 2});
        reset_value(&s->message_placeholder);
        append(&s->message_placeholder, utf8_literal("Message @"));
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
Utf8View platform_name(const Utf8View& type) noexcept {
    // "type", "Display Name" pairs, each NUL-terminated.
    static constexpr char names[] =
        "battlenet\0Battle.net\0bluesky\0Bluesky\0bungie\0Bungie.net\0crunchyroll\0Crunchyroll\0domain\0Domain\0"
        "ebay\0eBay\0epicgames\0Epic Games\0facebook\0Facebook\0github\0GitHub\0instagram\0Instagram\0"
        "leagueoflegends\0League of Legends\0mastodon\0Mastodon\0paypal\0PayPal\0playstation\0PlayStation\0"
        "reddit\0Reddit\0riotgames\0Riot Games\0roblox\0Roblox\0skype\0Skype\0spotify\0Spotify\0steam\0Steam\0"
        "tiktok\0TikTok\0twitch\0Twitch\0twitter\0X\0xbox\0Xbox\0youtube\0YouTube";
    for (const char* at = names; at < names + sizeof(names) - 1;) {
        const u32 type_bytes = c_string_length(at);
        const char* name = at + type_bytes + 1;
        const u32 name_bytes = c_string_length(name);
        if (utf8_equal({at, type_bytes}, type))
            return {name, name_bytes};
        at = name + name_bytes + 1;
    }
    return type;
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
        icon(s, owner, {x, y + 1, x + 16, y + 17}, UiIcon::calendar, ProfileAction::none);
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
        blank(s, owner, {x, y + 1, x + 16, y + 17})->media = ProfileMediaKind::member_guild;
        x += 20;
        l.member_since[1].bytes = 0;
        append_date(&l.member_since[1], joined / 1000);
        if (left + width > x)
            caption(s, owner, {x, y, left + width, y + 18}, l.member_since[1].view(), kSm);
    }
    return h + 18;
}
float connections(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept {
    const auto& p = *s->profile;
    if (!p.connection_count)
        return 0;
    float y = top + heading(s, owner, left, top, width, utf8_literal("Connections"));
    auto& l = labels(s);
    for (u32 i = 0; i < p.connection_count && i < 16; ++i) {
        const auto& c = p.connections[i];
        if (i)
            y += 16;
        // No platform icons are shipped: the box carries the platform initial.
        blank(s, owner, {left, y, left + 24, y + 24}, ProfileAction::none, ProfileLook::box);
        const auto platform = platform_name(c.type.view());
        l.connection_initial[i][0] = platform.bytes ? platform.data[0] : '?';
        if (l.connection_initial[i][0] >= 'a' && l.connection_initial[i][0] <= 'z')
            l.connection_initial[i][0] = static_cast<char>(l.connection_initial[i][0] - 32);
        caption(s, owner, {left, y, left + 24, y + 24}, {l.connection_initial[i], 1}, kXs | text_style_bold)
            ->centered = true;
        const float info = left + 32;
        const float name = text_width(s, c.name.view(), width - 32 - (c.verified ? 20.f : 0.f), kSm);
        caption(s, owner, {info, y, info + name, y + 18}, c.name.view(), kSm);
        if (c.verified)
            icon(s, owner, {info + name + 4, y + 1, info + name + 20, y + 17}, UiIcon::badge_check, ProfileAction::none);
        auto& line = l.connection_line[i];
        line.bytes = 0;
        u64 created = 0;
        if (c.created_at.bytes && parse_iso_timestamp({c.created_at.view(), 0, false}, &created)) {
            append(&line, utf8_literal("Member since "));
            append_date(&line, created / 1000);
        } else
            append(&line, platform);
        caption(s, owner, {info, y + 22, left + width, y + 38}, line.view(), kXs, ProfileTone::subtle);
        y += 38;
    }
    return y - top;
}
float note_section(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept {
    if (!s->note_visible || self(*s))
        return 0;
    const float h = heading(s, owner, left, top, width, utf8_literal("Note (only visible to you)"));
    s->note_offset = top;
    const float y = top + h;
    if (s->note_editing) {
        s->note_field = text_field(s, blank(s, owner, {left - 4, y, left + width, y + 34}, ProfileAction::note,
                                            ProfileLook::input));
        return h + 34;
    }
    const Utf8View text = s->note_text.bytes ? s->note_text
                          : s->note_loading ? utf8_literal("Loading Note")
                                            : utf8_literal("Click to add a note");
    const float height = maximum(18.f, measure(s, text, width, true, kSm).height);
    add(s, owner, {left, y, left + width, y + height}, text, s->note_loading ? ProfileAction::none : ProfileAction::note,
        kSm, ProfileLook::plain, s->note_text.bytes ? ProfileTone::normal : ProfileTone::muted)
        ->wrap = true;
    return h + height;
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
// The tab panel.
void tab_panel(ProfileScene* s, const UiRect& panel, float bottom_pad, const u8* order, u32 cards) noexcept {
    const auto& p = *s->profile;
    auto& l = labels(s);
    // Activity, then the mutual lists: a bot has servers only, the signed-in user none.
    Utf8View titles[3]{utf8_literal("Activity")};
    ProfileAction actions[3]{ProfileAction::activity};
    u32 count = 1;
    for (u32 i = bot(*s) ? 1 : 0; !self(*s) && i < 2; ++i) {
        auto& text = l.tab_text[i];
        const u32 n = i ? p.mutual_guild_count : p.mutual_friend_count;
        if (n)
            mutual_count(&text, n, i != 0);
        else {
            text.bytes = 0;
            append(&text, i ? utf8_literal("No Mutual Servers") : utf8_literal("No Mutual Friends"));
        }
        actions[count] = i ? ProfileAction::servers : ProfileAction::friends;
        titles[count++] = text.view();
    }
    if (s->tab >= count)
        s->tab = 0;
    float x = panel.left;
    const float tab_top = panel.top + 20;
    for (u32 i = 0; i < count; ++i) {
        const float w = text_width(s, titles[i], panel.right - x, kSm | text_style_bold);
        if (w <= 0)
            break;
        auto* tab = blank(s, s->panel, {x, tab_top, x + w, tab_top + 30}, actions[i], ProfileLook::tab);
        caption(s, s->panel, {x, tab_top, x + w + 1, tab_top + 14}, titles[i], kSm | text_style_bold,
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
    const float width = ui_rect_width(list) - 16;
    if (!s->tab) {
        float y = 0;
        // Cards 8 apart.
        for (u32 i = 0; i < cards; ++i) {
            const float used = activity_card(s, container->widget, 0, y, width, i, order[i], activity_now(*s),
                                             ProfileAction::none);
            if (used > 0)
                y += used + 8;
        }
        if (y > 0)
            y -= 8;
        else {
            caption(s, container->widget, {0, 0, width, 18}, utf8_literal("No current activity"), kSm,
                    ProfileTone::muted);
            y = 18;
        }
        s->secondary_height = y;
    } else {
        const bool friends = actions[s->tab] == ProfileAction::friends;
        const u32 n = friends ? p.mutual_friends_count : p.mutual_guild_count;
        s->secondary_height = n * 50.f;
        s->secondary_scroll =
            maximum(0.f, minimum(s->secondary_scroll, maximum(0.f, s->secondary_height - ui_rect_height(list))));
        const u32 first = static_cast<u32>(s->secondary_scroll / 50),
                  last = minimum(n, first + static_cast<u32>(ui_rect_height(list) / 50) + 2);
        for (u32 i = first; i < last; ++i) {
            const auto& peer = friends ? p.mutual_friends[i] : p.mutual_guilds[i];
            const auto name = peer_name(*s, friends, peer);
            const float y = i * 50.f + 1;
            peer_row(s, container->widget, friends, i, {0, y, width, y + 48}, {4, y + 4, 44, y + 44});
            const auto* status =
                friends && s->users ? user_appearance_find_activity(s->users->appearance, peer.id) : nullptr;
            const bool second = status && status->activity.bytes;
            const float name_top = second ? y + 6 : y + 14;
            if (name.bytes)
                caption(s, container->widget, {54, name_top, width - 4, name_top + 20}, name, kMd);
            if (second)
                caption(s, container->widget, {54, y + 26, width - 4, y + 42}, status->activity.view(), kXs,
                        ProfileTone::subtle);
        }
        if (!n)
            caption(s, container->widget, {0, 0, width, 18},
                    friends ? utf8_literal("No Mutual Friends") : utf8_literal("No Mutual Servers"), kSm,
                    ProfileTone::muted);
    }
    s->secondary_scroll =
        maximum(0.f, minimum(s->secondary_scroll, maximum(0.f, s->secondary_height - ui_rect_height(list))));
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
    header_parts(s, s->panel, {0, 0, 0, 0}, ProfileAction::none);
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
    section(biography(s, s->body, cp, y, content, false));
    // Bot info: the bio, its Commands, then Created On.
    section(commands_section(s, s->body, cp, y, content));
    section(member_since(s, s->body, cp, y, content));
    if (s->profile->guild_id && s->state) {
        const float before = y;
        const float h = heading(s, s->body, cp, y, content, utf8_literal("Roles"));
        const float r = roles(s, s->body, cp, y + h, content, false);
        if (r == 0) {
            // No roles to show: drop the heading again.
            --s->part_count;
            y = before;
        } else
            y += h + r + 24;
    }
    section(plugin_section(s, s->body, cp, y, content));
    section(connections(s, s->body, cp, y, content));
    section(note_section(s, s->body, cp, y, content));
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
        append(&label, utf8_literal("/"));
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
    header_parts(s, s->panel, {0, 0, 0, 0}, ProfileAction::expand);
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
    // The bio without a heading (three lines; "View Full Profile" below is the
    // way to the rest), the account date, then the cards (a bot's commands).
    section(biography(s, s->body, x0, y, content, true, false));
    section(member_since(s, s->body, x0, y, content, ProfileTone::strong));
    section(commands_section(s, s->body, x0, y, content));
    section(plugin_section(s, s->body, x0, y, content));
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
