#include "profile_scene_internal.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/model/chat/chat_state.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/model/voice/state_store.hpp"
#include "maicord/ui/widgets/icon.hpp"
#include "maicord/ui/widgets/presence.hpp"

namespace maicord {
using namespace profile_scene_detail;

namespace profile_scene_detail {
CardMetrics card_metrics(bool large) noexcept {
    return large ? CardMetrics{12, 100, 32, 12, 4} : CardMetrics{8, 60, 24, 8, 2};
}
namespace {
using Role = UiActivityCardRole;
// The action a part takes only to show its hint: the popout card's own click,
// or none in the full profile.
ProfileAction hint_action(const ProfileScene& s) noexcept {
    return modal(s) ? ProfileAction::hint : ProfileAction::card;
}
u16 part_index(const ProfileScene& s, const ProfilePart* part) noexcept {
    return part && part >= s.parts && part < s.parts + s.part_count ? static_cast<u16>(part - s.parts) : kNoPart;
}
bool application_icon(const ProfileScene& s, u64 application, bool* known) noexcept {
    *known = false;
    for (u32 i = 0; i < s.applications.count; ++i)
        if (s.applications[i].id == application) {
            *known = true;
            return s.applications[i].icon.bytes != 0;
        }
    return false;
}
UiSize measure_text(void* context, Utf8View text, u32 flags, float width, bool wrap) noexcept {
    return measure(static_cast<ProfileScene*>(context), text, width, wrap, flags);
}
ProfileAction card_action(UiActivityCardAction action) noexcept {
    switch (action) {
    case UiActivityCardAction::button:
        return ProfileAction::activity_button;
    case UiActivityCardAction::watch:
        return ProfileAction::activity_watch;
    case UiActivityCardAction::join:
        return ProfileAction::activity_join;
    case UiActivityCardAction::spotify_track:
    case UiActivityCardAction::spotify_album:
        return ProfileAction::activity_spotify;
    default:
        return ProfileAction::none;
    }
}
ProfileTone card_tone(const UiActivityCardItem& item) noexcept {
    switch (item.tone) {
    case UiActivityCardTone::strong:
        return ProfileTone::strong;
    case UiActivityCardTone::positive:
        return ProfileTone::positive;
    case UiActivityCardTone::subtle:
        // A disabled button's glyph and label read muted.
        return item.role == Role::button_glyph || item.role == Role::button_label ? ProfileTone::muted
                                                                                  : ProfileTone::subtle;
    default:
        return ProfileTone::normal;
    }
}
} // namespace

// One card from the shared layout: each item becomes a part, offset to the
// card's place. Text parts borrow the card's strings, which the tick rewrites.
float activity_card(ProfileScene* s, UiWidgetHandle owner, float left, float top, float right, u32 card, u32 item,
                    u64 now_ms, ProfileAction action) noexcept {
    auto& l = labels(s);
    const auto& a = l.activity_set.items[item];
    auto& c = l.activity_card[card];
    bool known = false;
    const bool icon_ready = application_icon(*s, a.application_id, &known);
    const UiActivityCardInput input{modal(*s) ? UiActivityCardSize::full : UiActivityCardSize::popout,
                                    right - left,
                                    now_ms,
                                    bot(*s),
                                    self(*s),
                                    interactive(*s),
                                    icon_ready,
                                    measure_text,
                                    s};
    if (!ui_activity_card_layout(a, input, &c))
        return 0;
    if (a.application_id && !known && c.content.artwork && c.art.large != UserActivityArt::large &&
        s->missing_icon_count < kUserActivityMaximum)
        s->missing_icons[s->missing_icon_count++] = a.application_id;
    auto* background = blank(s, owner, {left, top, right, top + 1}, action, ProfileLook::card);
    background->media = ProfileMediaKind::activity_card;
    background->source = static_cast<u16>(item);
    const u16 source = static_cast<u16>(item);
    ActivityTimer timer{kNoPart, kNoPart, kNoPart, kNoPart, kNoPart, static_cast<u8>(item), static_cast<u8>(card)};
    for (u32 i = 0; i < c.count; ++i) {
        const auto& it = c.items[i];
        const UiRect r{left + it.rect.left, top + it.rect.top, left + it.rect.right, top + it.rect.bottom};
        const auto tone = card_tone(it);
        const auto act = card_action(it.action);
        ProfilePart* part = nullptr;
        switch (it.role) {
        case Role::heading:
            part = caption(s, owner, r, it.text, it.flags, tone);
            break;
        case Role::title:
        case Role::line:
            part = add(s, owner, r, it.text, act != ProfileAction::none ? act : it.hint.bytes ? hint_action(*s)
                                                                                               : ProfileAction::none,
                       it.flags, ProfileLook::plain, tone);
            part->wrap = it.wrap;
            part->media = ProfileMediaKind::activity_text;
            if (act == ProfileAction::none && it.hint.bytes)
                unfocusable(s, part);
            break;
        case Role::art:
        case Role::badge:
            part = blank(s, owner, r,
                         act != ProfileAction::none ? act : it.hint.bytes ? hint_action(*s) : ProfileAction::none);
            part->media = it.role == Role::art ? ProfileMediaKind::activity_large : ProfileMediaKind::activity_small;
            if (act == ProfileAction::none)
                unfocusable(s, part);
            break;
        case Role::glyph:
        case Role::clock_glyph:
        case Role::button_glyph:
            part = icon(s, owner, r, it.icon, ProfileAction::none, tone);
            if (it.role == Role::clock_glyph)
                timer.glyph = part_index(*s, part);
            break;
        case Role::chip:
        case Role::clock:
        case Role::elapsed:
        case Role::total:
        case Role::button_label:
            part = caption(s, owner, r, it.text, it.flags, tone);
            if (it.role != Role::button_label)
                part->media = it.role == Role::total ? ProfileMediaKind::activity_clock : ProfileMediaKind::activity_text;
            if (it.role == Role::clock)
                timer.clock = part_index(*s, part);
            else if (it.role == Role::elapsed)
                timer.elapsed = part_index(*s, part);
            else if (it.role == Role::total)
                timer.total = part_index(*s, part);
            break;
        case Role::bar:
            part = blank(s, owner, r);
            part->media = ProfileMediaKind::activity_progress;
            part->color = {c.fraction, 0, 0, 1};
            timer.bar = part_index(*s, part);
            break;
        case Role::button:
            part = blank(s, owner, r, act, ProfileLook::secondary);
            part->media = ProfileMediaKind::activity_action;
            break;
        }
        part->source = source;
        part->hint = it.hint;
        // The art's image kind (Spotify's album art opens the album); a button's index.
        part->target = it.role == Role::art ? static_cast<u64>(c.art.large) : it.index;
    }
    place(s, background->widget, {left, top, right, top + c.height});
    if ((timer.clock != kNoPart || timer.bar != kNoPart) && l.timer_count < kUserActivityMaximum)
        l.timers[l.timer_count++] = timer;
    return c.height;
}
// The profile user's cards in the official order; the set is copied into the
// scene first. Offline users show none.
u32 activity_cards(ProfileScene* s, u8* order) noexcept {
    auto& l = labels(s);
    s->activity_set = nullptr;
    if (!s->users || !s->profile || s->presence == UiPresence::offline ||
        !user_activity_find(s->users->activities, s->profile->user_id, &l.activity_set) || !l.activity_set.count)
        return 0;
    s->activity_set = &l.activity_set;
    return profile_activity_order(l.activity_set, {order, kUserActivityMaximum});
}
namespace {
// The voice card's member collage: per member count, each avatar's top, left
// and size in the 60 DIP (popout) and 100 DIP (full profile) square.
struct Slot {
    u8 top, left, size;
};
constexpr Slot kPopoutCollage[4][4]{{{15, 15, 28}},
                                    {{7, 7, 20}, {25, 25, 26}},
                                    {{5, 10, 18}, {15, 31, 22}, {29, 5, 24}},
                                    {{5, 5, 22}, {7, 33, 18}, {33, 7, 18}, {31, 31, 22}}};
constexpr Slot kFullCollage[4][4]{{{23, 23, 52}},
                                  {{13, 13, 32}, {41, 41, 44}},
                                  {{9, 14, 31}, {27, 53, 34}, {52, 11, 36}},
                                  {{10, 10, 36}, {10, 57, 31}, {57, 10, 31}, {52, 52, 36}}};
// The voice card's parts share this source (activity_set indices are below it).
constexpr u16 kVoiceSource = 0xFFFE;
constexpr u8 kStageChannel = 13;
// The profile user's voice state: in the profile's server, else in any
// channel (the official client's lookup without a server).
const VoiceState* user_voice(const ProfileScene& s) noexcept {
    const u64 user = s.profile->user_id;
    if (s.profile->guild_id)
        if (const auto* v = voice_state_store_find(*s.voice_states, s.profile->guild_id, user))
            return v;
    for (u32 i = 0; i < s.voice_states->count; ++i)
        if (s.voice_states->records[i].user_id == user && s.voice_states->records[i].channel_id)
            return &s.voice_states->records[i];
    return nullptr;
}
// The voice state the card shows and its channel (null for a call the store
// does not hold), or null: the user is in no channel this account can see, or
// an activity of theirs runs in it (an embedded one has a card of its own).
const VoiceState* shown_voice(const ProfileScene& s, const ChannelRecord** channel) noexcept {
    *channel = nullptr;
    if (!s.voice_states || !s.state || !s.users || !s.profile || s.presence == UiPresence::offline)
        return nullptr;
    const auto* v = user_voice(s);
    if (!v)
        return nullptr;
    for (u32 i = 0; s.activity_set && i < s.activity_set->count; ++i)
        if (s.activity_set->items[i].flags & user_activity_flag_embedded)
            return nullptr;
    *channel = chat_state_store_find_channel(*s.state, v->channel_id);
    const bool call = *channel ? channel_is_private((*channel)->type) : !v->guild_id;
    if (!call && (!*channel || !(*channel)->permissions_resolved || !((*channel)->actions & channel_action_view)))
        return nullptr;
    return v;
}
template <u32 N> Utf8View keep(ProfileString<N>* out, Utf8View prefix, Utf8View value) noexcept {
    out->bytes = 0;
    append(out, prefix);
    append(out, value);
    return out->view();
}
} // namespace
bool voice_card_shown(const ProfileScene& s) noexcept {
    const ChannelRecord* channel = nullptr;
    return shown_voice(s, &channel) != nullptr;
}
float voice_card(ProfileScene* s, UiWidgetHandle owner, float left, float top, float right,
                 ProfileAction action) noexcept {
    const ChannelRecord* channel = nullptr;
    const auto* v = shown_voice(*s, &channel);
    if (!v)
        return 0;
    const bool call = channel ? channel_is_private(channel->type) : !v->guild_id;
    const bool stage = channel && channel->type == kStageChannel;
    const bool large = modal(*s);
    const auto m = card_metrics(large);
    auto& l = labels(s);
    // The channel's name; a one-to-one call reads as its other member.
    Utf8View name = channel ? chat_state_store_channel_name(*s->state, *channel) : Utf8View{};
    const auto* record = user_store_find(*s->users, s->profile->user_id);
    if (!name.bytes && channel && channel->type == 1 && record)
        name = user_store_name(*s->users, *record);
    const auto channel_name = keep(&l.voice_channel, {}, name);
    const auto* guild = call ? nullptr : chat_state_store_find_guild(*s->state, v->guild_id);
    const auto guild_line =
        guild ? keep(&l.voice_guild, utf8_literal("in "), chat_state_store_guild_name(*s->state, *guild)) : Utf8View{};
    const bool connect = interactive(*s) && (call || (channel->actions & channel_action_connect));
    const bool open = s->connected_channel == v->channel_id;

    auto* background = blank(s, owner, {left, top, right, top + 1}, action, ProfileLook::card);
    background->media = ProfileMediaKind::activity_card;
    background->source = kVoiceSource;
    float y = top + m.pad;
    caption(s, owner, {left + m.pad, y, right - m.pad, y + 16},
            call ? utf8_literal("In a call") : stage ? utf8_literal("In a stage") : utf8_literal("In voice"),
            kXs | text_style_bold, ProfileTone::strong);
    y += 16 + 8;
    // The members' collage; past four, the second slot (the fourth from ten
    // on) shows how many more there are.
    const u32 members = voice_state_store_channel_count(*s->voice_states, v->channel_id);
    const u32 shown = minimum<u32>(members, 4);
    const u32 more = members > 4 ? members - 4 + 1 : 0, more_slot = more >= 10 ? 3 : 1;
    const auto& collage = (large ? kFullCollage : kPopoutCollage)[shown ? shown - 1 : 0];
    const float x0 = left + m.pad;
    for (u32 i = 0; i < shown; ++i) {
        const auto& slot = collage[i];
        const UiRect r{x0 + slot.left, y + slot.top, x0 + slot.left + slot.size, y + slot.top + slot.size};
        blank(s, owner, r, ProfileAction::none, ProfileLook::avatar_plate)->source = kVoiceSource;
        if (more && i == more_slot) {
            char digits[8];
            auto& label = l.voice_more;
            label.bytes = 0;
            if (more > 99)
                append(&label, utf8_literal(">99"));
            else {
                append(&label, utf8_literal("+"));
                append(&label, {digits, decimal_write(digits, more)});
            }
            caption(s, owner, r, label.view(), kXs | text_style_bold, ProfileTone::strong)->centered = true;
            continue;
        }
        const auto* state = voice_state_store_channel_at(*s->voice_states, v->channel_id, i);
        const auto* member = state ? user_store_find(*s->users, state->user_id) : nullptr;
        auto* face = blank(s, owner, r, member ? hint_action(*s) : ProfileAction::none);
        face->media = ProfileMediaKind::voice_avatar;
        face->source = kVoiceSource;
        face->target = state ? state->user_id : 0;
        face->hint = member ? keep(&l.voice_names[i], {}, user_store_name(*s->users, *member)) : Utf8View{};
        unfocusable(s, face);
    }
    // The channel (a link that joins it) and its server, centred on the
    // collage; the full profile's button follows them in the column.
    const float text_left = x0 + m.image + m.gap, column = maximum(1.f, right - m.pad - text_left);
    const Utf8View label = large  ? (open ? utf8_literal("Open") : utf8_literal("Join"))
                           : call ? (open ? utf8_literal("Open Call") : utf8_literal("Join Call"))
                           : stage ? (open ? utf8_literal("Open Stage") : utf8_literal("Join Stage"))
                                   : (open ? utf8_literal("Open Voice") : utf8_literal("Join Voice"));
    const float lines = (channel_name.bytes ? 18.f : 0.f) + (guild_line.bytes ? 16.f : 0.f);
    const float details = lines + (large && connect ? (lines > 0 ? 8.f : 0.f) + 32 : 0.f);
    float cy = y + maximum(0.f, (m.image - details) * .5f);
    if (channel_name.bytes) {
        icon(s, owner, {text_left, cy + 3, text_left + 12, cy + 15},
             call ? UiIcon::phone : stage ? UiIcon::podcast : UiIcon::speaker, ProfileAction::none, ProfileTone::normal);
        auto* link = add(s, owner, {text_left + 16, cy, text_left + column, cy + 18}, channel_name,
                         connect ? ProfileAction::voice_join : ProfileAction::none, kSm | text_style_bold,
                         ProfileLook::plain, ProfileTone::strong);
        link->media = ProfileMediaKind::activity_text;
        link->source = kVoiceSource;
        link->target = v->channel_id;
        cy += 18;
    }
    if (guild_line.bytes) {
        auto* link = add(s, owner, {text_left, cy, text_left + column, cy + 16}, guild_line,
                         interactive(*s) ? ProfileAction::voice_guild : ProfileAction::none, kXs, ProfileLook::plain,
                         ProfileTone::subtle);
        link->media = ProfileMediaKind::activity_text;
        link->source = kVoiceSource;
        link->target = v->guild_id;
        cy += 16;
    }
    float bottom = y + maximum(m.image, details);
    if (connect) {
        const float label_width = text_width(s, label, 400, kSm | text_style_bold);
        // Full width under the body in the popout; its natural width in the column.
        const float bx = large ? text_left : x0, width = large ? minimum(column, label_width + 32) : right - left - 2 * m.pad;
        const float by = large ? cy + (cy > y ? 8.f : 0.f) : bottom + 12;
        if (!large)
            bottom = by + 32;
        auto* button = blank(s, owner, {bx, by, bx + width, by + 32}, ProfileAction::voice_join, ProfileLook::secondary);
        button->media = ProfileMediaKind::activity_action;
        button->source = kVoiceSource;
        button->target = v->channel_id;
        const float lx = bx + maximum(16.f, (width - label_width) * .5f);
        caption(s, owner, {lx, by, maximum(lx + 1, minimum(lx + label_width + 1, bx + width - 16)), by + 32}, label,
                kSm | text_style_bold);
    }
    bottom += m.pad;
    place(s, background->widget, {left, top, right, bottom});
    return bottom - top;
}
} // namespace profile_scene_detail
} // namespace maicord
