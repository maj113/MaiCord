#include "maicord/ui/guild/card_scene.hpp"
#include "maicord/ui/theme/background_color_math.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/ui/text/style.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/ui/text/measure_cache.hpp"

namespace maicord {
namespace {
constexpr float kW = kUiGuildCardWidth, kP = kUiGuildCardPadding, kRadius = 16, kSlide = 8;
// Game icons: 26 DIP boxes 4 apart, a 1 DIP edge around the 24 DIP picture, 4 DIP corners; a lone game's name
// follows 8 after it.
constexpr float kGame = 26, kGameGap = 4, kGameEdge = 1, kGameRadius = 4, kGameNameGap = 8;
// The picture under "+N" shows through the official 72% black scrim.
constexpr float kGamesMoreShow = .28f;
// A busy game's marker: a 14 DIP disc around a 10 DIP flame.
constexpr float kGameMarker = 14, kGameFlame = 10;
// A trait's emoji: 16 DIPs, 4 before its label.
constexpr float kEmoji = 16, kEmojiGap = 4;
// The icon sits in a 70 DIP ring plate overlapping the banner by 34, as the official card's avatar container.
constexpr float kRing = 3, kIconTop = kUiGuildCardBanner - 34 + kRing;
constexpr float kNameHeight = 20, kLine = 18, kButton = 32;
// The server badge, and the tag chip (the author line's chiplet: 16 tall, 4 padding, a 14 DIP badge, at most 70
// DIPs of badge and label).
constexpr float kServerBadge = 18, kChip = 16, kChipPadding = 4, kChipBadge = 14, kChipContent = 70;
// Trait chips: 4 DIPs of padding over a 14/18 label, 8 at the sides, a 1 DIP border.
constexpr float kTrait = 28, kTraitInset = 9;
constexpr u32 kDescriptionLines = 5;
// heading-md/semibold name, text-sm details and traits, text-xs/semibold tag.
constexpr u32 kNameStyle = text_style_bold, kInfoStyle = kTextStyleSizeCodeSm,
              kTagStyle = kTextStyleSizeCodeXs | text_style_bold;
constexpr UiHoverCardMotion kEntrance{{kUiGuildCardEntranceMs, UiMotionCurve::spring}, 0x4743'4152'4400'0001ull};
constexpr u64 kMeasureSource = 0x4743415244ull; // "GCARD"

// Measured size of text in a box width wide (wrapping unless single).
UiSize measure(UiTextMeasureCache* cache, const Utf8View& text, u32 style, float width, bool single, u16 item) noexcept {
    const float estimate = static_cast<float>(text.bytes) * (style & text_style_subtext ? 6.5f : 7.5f);
    UiSize size{single ? minimum(width, estimate) : width,
                single ? kLine : kLine * static_cast<float>(static_cast<u32>(estimate / maximum(width, 1.f)) + 1)};
    if (!cache || !text.bytes)
        return text.bytes ? size : UiSize{};
    UiTextMeasureValue value{};
    const TextStyleSpan span{0, text.bytes, style};
    if (ui_text_measure_cache_measure(
            cache, {{kMeasureSource, hash64_bytes(kHash64Seed, text.data, text.bytes), 0, static_cast<u16>(style), item},
                    text,
                    {&span, 1},
                    {width, 4096},
                    single},
            &value)
            .ok())
        size = value.size;
    return size;
}
UiRect offset(UiRect r, float dx, float dy) noexcept {
    return ui_rect_has_area(r) ? UiRect{r.left + dx, r.top + dy, r.right + dx, r.bottom + dy} : UiRect{};
}
// "Est. Mar 2021" from the guild snowflake (UTC month).
u32 established(u64 guild_id, char* output) noexcept {
    if (!guild_id)
        return 0;
    const u64 unix_ms = (guild_id >> 22) + 1'420'070'400'000ull;
    const CivilDate date = civil_date_from_days(static_cast<i64>(unix_ms / 86'400'000ull));
    const u32 month = date.month;
    const u64 year = static_cast<u64>(date.year);
    static constexpr char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    copy_bytes(output, "Est. ", 5);
    copy_bytes(output + 5, months + (month - 1) * 3, 3);
    output[8] = ' ';
    return 9 + decimal_write(output + 9, year);
}
// Settled card beside the anchor (the tag chip): right of it when it fits,
// else left; top-aligned with it and kept inside the viewport.
UiRect place(UiRect anchor, UiRect viewport, float height) noexcept {
    float left = anchor.right + kUiGuildCardGap;
    if (left + kW > viewport.right - kUiGuildCardGap)
        left = anchor.left - kUiGuildCardGap - kW;
    left = maximum(viewport.left + kUiGuildCardGap, minimum(left, viewport.right - kUiGuildCardGap - kW));
    float top = anchor.top - 8;
    top = minimum(top, viewport.bottom - kUiGuildCardGap - height);
    top = maximum(viewport.top + kUiGuildCardGap, top);
    return {left, top, left + kW, top + height};
}
float whole(float width) noexcept {
    return static_cast<float>(static_cast<u32>(width + .999f));
}
bool trait_has_emoji(const UiGuildCardScene& s, u32 i) noexcept {
    return s.trait_emoji_bytes[i] || media_asset_key_valid(s.trait_emoji_keys[i]);
}
// A trait chip's emoji square at the start of its padding, panel-relative.
UiRect trait_emoji_square(const UiGuildCardScene& s, u32 i) noexcept {
    const auto& r = s.traits[i];
    const float cy = (r.top + r.bottom) * .5f;
    return {r.left + kTraitInset, cy - kEmoji * .5f, r.left + kTraitInset + kEmoji, cy + kEmoji * .5f};
}
// The name, then the server badge and the tag chip, centred on one line; the name ellipsizes to leave them room.
float layout_name_line(UiGuildCardScene& s, UiTextMeasureCache* cache, float y) noexcept {
    const float content = kW - 2 * kP, cy = y + kNameHeight * .5f;
    float chip = 0;
    if (s.tag_bytes) {
        const float label = measure(cache, {s.tag, s.tag_bytes}, kTagStyle, kChipContent, true, 1).width;
        chip = minimum(kChipContent, kChipBadge + 2 + whole(label)) + 2 * kChipPadding;
    }
    const bool badge = s.server_badge_kind != UiGuildCardBadge::none;
    const float trailing = (badge ? kServerBadge + 4 : 0) + (chip > 0 ? chip + 4 : 0);
    const float name = whole(measure(cache, {s.name_text, s.name_bytes}, kNameStyle, content, true, 10).width);
    s.name = {kP, y, kP + maximum(0.f, minimum(name, content - trailing)), y + kNameHeight};
    float x = s.name.right + 4;
    if (badge) {
        s.server_badge = {x, cy - kServerBadge * .5f, x + kServerBadge, cy + kServerBadge * .5f};
        x = s.server_badge.right + 4;
    }
    if (chip > 0) {
        s.tag_chip = {x, cy - kChip * .5f, x + chip, cy + kChip * .5f};
        s.tag_badge = {x + kChipPadding, cy - kChipBadge * .5f, x + kChipPadding + kChipBadge, cy + kChipBadge * .5f};
        s.tag_text = {s.tag_badge.right + 2, s.tag_chip.top, s.tag_chip.right - kChipPadding, s.tag_chip.bottom};
    }
    return y + kNameHeight;
}
// Trait chips wrap at most kUiGuildCardTraitLines lines; the traits that do not fit fold into "+ N more".
float layout_traits(UiGuildCardScene& s, UiTextMeasureCache* cache, float y) noexcept {
    const float content = kW - 2 * kP;
    float widths[kUiGuildCardTraits]{};
    // A chip holds its emoji (16 DIPs and 4 before the label) and its label.
    for (u32 i = 0; i < s.trait_count; ++i) {
        const float emoji = trait_has_emoji(s, i) ? kEmoji + kEmojiGap : 0;
        widths[i] = minimum(content, whole(measure(cache, {s.trait_text[i], s.trait_bytes[i]}, kInfoStyle,
                                                   content - 2 * kTraitInset - emoji, true, static_cast<u16>(4 + i))
                                               .width) +
                                         emoji + 2 * kTraitInset);
    }
    // The official list reserves room for the widest label it could show ("+ 1N more" for N traits).
    char reserve[16];
    u32 bytes = 0;
    reserve[bytes++] = '+';
    reserve[bytes++] = ' ';
    reserve[bytes++] = '1';
    bytes += decimal_write(reserve + bytes, s.trait_count);
    copy_bytes(reserve + bytes, " more", 5);
    const float reserved =
        whole(measure(cache, {reserve, bytes + 5}, kInfoStyle, content, true, 11).width) + 2 * kTraitInset;
    s.visible_traits = ui_guild_card_visible_traits(widths, s.trait_count, reserved, content, kUiGuildCardTraitLines);
    const u32 hidden = s.trait_count - s.visible_traits;
    if (hidden) {
        bytes = 0;
        s.overflow_text[bytes++] = '+';
        s.overflow_text[bytes++] = ' ';
        bytes += decimal_write(s.overflow_text + bytes, hidden);
        copy_bytes(s.overflow_text + bytes, " more", 5);
        s.overflow_bytes = bytes + 5;
        // The tooltip's chips: one line per folded trait, a Unicode emoji leading its label.
        for (u32 i = s.visible_traits; i < s.trait_count; ++i) {
            if (s.overflow_hint_bytes)
                s.overflow_hint[s.overflow_hint_bytes++] = '\n';
            if (s.trait_emoji_bytes[i]) {
                copy_bytes(s.overflow_hint + s.overflow_hint_bytes, s.trait_emoji[i], s.trait_emoji_bytes[i]);
                s.overflow_hint_bytes += s.trait_emoji_bytes[i];
                s.overflow_hint[s.overflow_hint_bytes++] = ' ';
            }
            copy_bytes(s.overflow_hint + s.overflow_hint_bytes, s.trait_text[i], s.trait_bytes[i]);
            s.overflow_hint_bytes += s.trait_bytes[i];
        }
    }
    // The shown chips and the overflow chip wrap like the official flex row.
    float x = kP;
    const auto place_chip = [&](float width) noexcept {
        if (x > kP && x + width > kW - kP) {
            x = kP;
            y += kTrait + kUiGuildCardTraitGap;
        }
        const UiRect r{x, y, x + width, y + kTrait};
        x += width + kUiGuildCardTraitGap;
        return r;
    };
    for (u32 i = 0; i < s.visible_traits; ++i)
        s.traits[i] = place_chip(widths[i]);
    if (hidden)
        s.overflow = place_chip(minimum(
            content, whole(measure(cache, {s.overflow_text, s.overflow_bytes}, kInfoStyle, content, true, 12).width) +
                         2 * kTraitInset));
    return y + kTrait;
}
// Panel-relative layout; returns the card height.
float layout_relative(UiGuildCardScene& s, UiTextMeasureCache* cache) noexcept {
    s.banner = {0, 0, kW, kUiGuildCardBanner};
    s.icon_ring = {kP, kIconTop - kRing, kP + kUiGuildCardIcon + 2 * kRing, kIconTop + kUiGuildCardIcon + kRing};
    s.icon = {kP + kRing, kIconTop, kP + kRing + kUiGuildCardIcon, kIconTop + kUiGuildCardIcon};
    s.name = s.server_badge = s.tag_chip = s.tag_badge = s.tag_text = s.online_dot = s.online = s.members_dot =
        s.members = {};
    s.established = s.description = s.overflow = s.button = s.status_icon = s.status_title = s.status_text = {};
    for (auto& trait : s.traits)
        reset_value(&trait);
    for (auto& game : s.games)
        reset_value(&game);
    reset_value(&s.games_more);
    reset_value(&s.game_name);
    s.visible_traits = s.overflow_bytes = s.overflow_hint_bytes = s.games_more_bytes = 0;
    const float content = kW - 2 * kP;
    // The header starts 8 below the icon's ring plate.
    float y = s.icon_ring.bottom + 8;
    if (s.phase == UiGuildCardPhase::loading) {
        s.status_icon = {kW * .5f - 14, y + 8, kW * .5f + 14, y + 36};
        return y + 52;
    }
    if (s.phase != UiGuildCardPhase::ready) {
        s.status_title = {kP, y, kW - kP, y + kNameHeight};
        y += kNameHeight + 2;
        const auto text = s.phase == UiGuildCardPhase::private_profile
                              ? utf8_literal("The server has limited who can see this profile.")
                              : utf8_literal("Unable to load server profile. Click to try again.");
        const float height = minimum(measure(cache, text, text_style_subtext, content - 22, false, 9).height, kLine * 3);
        s.status_icon = {kP, y + 1, kP + 16, y + 17};
        s.status_text = {kP + 22, y, kW - kP, y + maximum(kLine, height)};
        return s.status_text.bottom + kP;
    }
    y = layout_name_line(s, cache, y);
    // Online and member counts: 8 DIP dots 4 before their labels, the two groups 8 apart.
    const float online = whole(measure(cache, {s.online_text, s.online_bytes}, kInfoStyle, content, true, 2).width);
    s.online_dot = {kP, y + 5, kP + 8, y + 13};
    s.online = {kP + 12, y, minimum(kW - kP, kP + 12 + online), y + kLine};
    s.members_dot = {s.online.right + 8, y + 5, s.online.right + 16, y + 13};
    s.members = {s.members_dot.right + 4, y, kW - kP, y + kLine};
    y += kLine;
    s.established = {kP, y, kW - kP, y + kLine};
    // The header's 8 DIP margin, then the body (description, traits) and its 16, or an 8 DIP spacer.
    y += kLine + 8;
    if (s.description_bytes) {
        const float height =
            minimum(measure(cache, {s.description_text, s.description_bytes}, kInfoStyle, content, false, 3).height,
                    kLine * kDescriptionLines);
        s.description = {kP, y, kW - kP, y + maximum(kLine, height)};
        y = s.description.bottom;
        if (s.game_count || s.trait_count)
            y += 16;
    }
    if (s.game_count) {
        // The first games' icons; "+N" over the next one's counts the rest; a lone game is named beside its icon.
        float x = kP;
        for (u32 i = 0; i < s.shown_games; ++i, x += kGame + kGameGap)
            s.games[i] = {x, y, x + kGame, y + kGame};
        if (s.game_count > s.shown_games) {
            s.games_more_text[0] = '+';
            s.games_more_bytes = 1 + decimal_write(s.games_more_text + 1, s.game_count - s.shown_games);
            s.games_more = {x, y, x + kGame, y + kGame};
        } else if (s.game_count == 1 && s.game_name_bytes[0]) {
            s.game_name = {s.games[0].right + kGameNameGap, y, kW - kP, y + kGame};
        }
        y += kGame;
        if (s.trait_count)
            y += 16;
    }
    if (s.trait_count)
        y = layout_traits(s, cache, y);
    y += s.description_bytes || s.game_count || s.trait_count ? 16 : 8;
    if (s.action != UiGuildCardAction::none) {
        s.button = {kP, y, kW - kP, y + kButton};
        y += kButton + kP;
    }
    return y;
}
// Absolute rectangles for paint, text and hits: relative ones moved with the
// panel (which carries the entrance slide).
UiRect at(const UiGuildCardScene& s, UiRect r) noexcept {
    return offset(r, s.panel.left, s.panel.top);
}
void place_panel(UiGuildCardScene& s) noexcept {
    const float side = s.settled.left >= s.anchor.right ? -1.f : 1.f;
    const float dx = side * kSlide * (1 - clamp_unit(s.progress));
    s.panel = {s.settled.left + dx, s.settled.top, s.settled.right + dx, s.settled.bottom};
}
// The server name's initials stand in for a missing icon.
u32 acronym(const UiGuildCardScene& s, char* output) noexcept {
    u32 bytes = 0;
    bool start = true;
    for (u32 i = 0; i < s.name_bytes && bytes < 6; ++i) {
        const char c = s.name_text[i];
        if (c == ' ') {
            start = true;
            continue;
        }
        if (start && (static_cast<u8>(c) & 0x80) == 0)
            output[bytes++] = c;
        start = false;
    }
    return bytes;
}
Utf8View action_label(UiGuildCardAction action) noexcept {
    switch (action) {
    case UiGuildCardAction::go_to_server: return utf8_literal("Go to Server");
    case UiGuildCardAction::join: return utf8_literal("Join");
    case UiGuildCardAction::joining: return utf8_literal("Joining\xE2\x80\xA6");
    default: return {};
    }
}
// "Level 2" (or "No Level"), as the official boost tooltip names a level.
void boost_level(TextBank& bank, u8 tier) noexcept {
    if (tier) {
        bank.add("Level ");
        bank.number(minimum<u8>(tier, 3));
    } else {
        bank.add("No Level");
    }
}
// "14 Boosts" ("1 Boost").
void boost_count(TextBank& bank, u32 boosts) noexcept {
    char digits[32];
    bank.add({digits, decimal_write_grouped(digits, boosts)});
    bank.add(boosts == 1 ? Utf8View{utf8_literal(" Boost")} : Utf8View{utf8_literal(" Boosts")});
}
// The server badge's tooltip card (the official texts): a boost badge's title is its level, its line the boosts;
// the others' title is "Community Server", their lines the verification or partnership, who can join, then the
// level and boosts of a boosted one.
u32 badge_hint(const UiGuildCardServer& server, UiGuildCardBadge kind, char* output) noexcept {
    TextBank bank{output, 192, 0};
    if (kind == UiGuildCardBadge::none)
        return 0;
    if (kind == UiGuildCardBadge::boost) {
        boost_level(bank, server.premium_tier);
        bank.add("\n");
        boost_count(bank, server.boosts);
        return bank.used;
    }
    bank.add("Community Server");
    if (kind == UiGuildCardBadge::verified)
        bank.add(server.partnered ? Utf8View{utf8_literal("\nVerified & Partnered")}
                                  : Utf8View{utf8_literal("\nVerified")});
    else if (kind == UiGuildCardBadge::partnered)
        bank.add("\nDiscord Partner");
    const bool open = server.community && server.discoverable;
    bank.add(open                   ? Utf8View{utf8_literal("\nAnyone can join this server.")}
             : server.apply_to_join ? Utf8View{utf8_literal("\nPeople must submit an application to join.")}
                                    : Utf8View{utf8_literal("\nPeople with an invite link can join.")});
    if (server.boosts || server.premium_tier) {
        bank.add("\n");
        boost_level(bank, server.premium_tier);
        bank.add(" \xC2\xB7 ");
        boost_count(bank, server.boosts);
    }
    return bank.used;
}
} // namespace

bool ui_guild_card_show(UiGuildCardScene* s, u64 identity, const UiGuildCardContent& content, UiRect anchor,
                        UiRect viewport, u64 now_ms, bool reduced_motion, UiTextMeasureCache* cache) noexcept {
    if (!identity || !ui_rect_valid(viewport) || !ui_rect_has_area(viewport) ||
        !ui_hover_card_initialize(*s, utf8_literal("Server profile"))) {
        if (s->active)
            ui_guild_card_close(s);
        return false;
    }
    const bool fresh = !s->active || s->identity != identity;
    s->identity = identity;
    s->anchor = ui_rect_has_area(anchor) ? anchor : UiRect{viewport.left, viewport.top, viewport.left + 1, viewport.top + 1};
    s->viewport = viewport;
    s->phase = content.phase;
    s->action = content.phase == UiGuildCardPhase::ready ? content.action : UiGuildCardAction::none;
    s->name_bytes = utf8_copy_truncated(s->name_text, kUiGuildCardNameBytes, content.name);
    s->description_bytes = utf8_copy_truncated(s->description_text, kUiGuildCardDescriptionBytes, content.description);
    s->tag_bytes = utf8_copy_truncated(s->tag, kUiGuildCardTagBytes, content.tag);
    s->trait_count = minimum(content.trait_count, kUiGuildCardTraits);
    for (u32 i = 0; i < s->trait_count; ++i) {
        s->trait_bytes[i] = utf8_copy_truncated(s->trait_text[i], kUiGuildCardTraitBytes, content.traits[i]);
        s->trait_emoji_bytes[i] = utf8_copy_truncated(s->trait_emoji[i], kUiGuildCardEmojiBytes, content.trait_emoji[i]);
        s->trait_emoji_keys[i] = s->trait_emoji_bytes[i] ? MediaAssetKey{} : content.trait_emoji_media[i];
    }
    u32 bytes = decimal_write_grouped(s->online_text, content.online_count);
    copy_bytes(s->online_text + bytes, " Online", 7);
    s->online_bytes = bytes + 7;
    bytes = decimal_write_grouped(s->members_text, content.member_count);
    copy_bytes(s->members_text + bytes, " Members", 8);
    s->members_bytes = bytes + 8;
    s->established_bytes = established(content.guild_id, s->established_text);
    s->banner_color = content.banner_color;
    s->has_banner_color = content.has_banner_color;
    s->server_badge_kind = ui_guild_card_badge(content.server);
    s->boost_tier = minimum<u8>(content.server.premium_tier, 3);
    s->boosted = content.server.boosts || content.server.premium_tier;
    s->icon_key = content.icon;
    s->badge_key = content.badge;
    s->banner_key = content.phase == UiGuildCardPhase::ready ? content.banner : MediaAssetKey{};
    s->game_count = content.phase == UiGuildCardPhase::ready ? content.game_count : 0;
    s->shown_games = minimum(s->game_count, kUiGuildCardGames);
    for (u32 i = 0; i < kUiGuildCardGames; ++i) {
        s->game_keys[i] = i < s->shown_games ? content.games[i] : MediaAssetKey{};
        s->game_high[i] = i < s->shown_games && content.game_high[i];
        s->game_name_bytes[i] =
            i < s->shown_games ? utf8_copy_truncated(s->game_name_text[i], kUiGuildCardGameNameBytes, content.game_names[i])
                               : 0;
    }
    for (u32 i = 0; i < kUiGuildCardGamesMore; ++i)
        s->games_more_keys[i] = s->game_count > s->shown_games ? content.games_more_icons[i] : MediaAssetKey{};
    s->games_more_hint_bytes = s->game_count > s->shown_games
                                   ? utf8_copy_truncated(s->games_more_hint, sizeof(s->games_more_hint),
                                                         content.games_more_names)
                                   : 0;
    s->badge_hint_bytes = badge_hint(content.server, s->server_badge_kind, s->badge_hint);
    s->pinned = content.pinned;
    const float height = layout_relative(*s, cache);
    s->settled = s->pinned ? UiRect{s->anchor.left, s->anchor.top, s->anchor.left + kW, s->anchor.top + height}
                           : place(s->anchor, viewport, height);
    if (fresh && !s->pinned)
        ui_hover_card_restart(s, kEntrance, now_ms, reduced_motion);
    s->progress = s->pinned ? 1.f : s->entrance.value;
    place_panel(*s);
    if (!ui_widget_manager_layout_rectangle(&s->widgets, viewport).ok()) {
        ui_guild_card_close(s);
        return false;
    }
    u64 hash = hash64_mix(kHash64Seed, identity);
    hash = hash64_counted_bytes(hash, s->name_text, s->name_bytes);
    hash = hash64_counted_bytes(hash, s->description_text, s->description_bytes);
    hash = hash64_counted_bytes(hash, s->tag, s->tag_bytes);
    hash = hash64_counted_bytes(hash, s->online_text, s->online_bytes);
    hash = hash64_counted_bytes(hash, s->members_text, s->members_bytes);
    for (u32 i = 0; i < s->trait_count; ++i)
        hash = hash64_counted_bytes(hash, s->trait_text[i], s->trait_bytes[i]);
    hash = hash64_bytes(hash, &s->icon_key, sizeof(s->icon_key));
    hash = hash64_bytes(hash, &s->badge_key, sizeof(s->badge_key));
    hash = hash64_bytes(hash, &s->banner_key, sizeof(s->banner_key));
    hash = hash64_bytes(hash, s->game_keys, sizeof(s->game_keys));
    hash = hash64_bytes(hash, s->games_more_keys, sizeof(s->games_more_keys));
    hash = hash64_bytes(hash, s->game_high, sizeof(s->game_high));
    hash = hash64_bytes(hash, s->trait_emoji_keys, sizeof(s->trait_emoji_keys));
    for (u32 i = 0; i < s->trait_count; ++i)
        hash = hash64_counted_bytes(hash, s->trait_emoji[i], s->trait_emoji_bytes[i]);
    for (u32 i = 0; i < s->shown_games; ++i)
        hash = hash64_counted_bytes(hash, s->game_name_text[i], s->game_name_bytes[i]);
    hash = hash64_mix(hash, s->game_count);
    hash = hash64_bytes(hash, &s->panel, sizeof(s->panel));
    hash = hash64_mix(hash, (u64{s->banner_color} << 16) | (u64{static_cast<u8>(s->phase)} << 8) |
                                (u64{static_cast<u8>(s->action)} << 1) | (s->has_banner_color ? 1u : 0u) |
                                (u64{static_cast<u8>(s->server_badge_kind)} << 48) | (u64{s->boost_tier} << 56) |
                                (u64{s->boosted} << 60));
    ui_hover_card_publish(s, hash);
    return true;
}

void ui_guild_card_close(UiGuildCardScene* s) noexcept {
    ui_hover_card_close(s);
}

bool ui_guild_card_tick(UiGuildCardScene* s, u64 now_ms, bool reduced_motion) noexcept {
    if (!ui_hover_card_advance(s, kEntrance, now_ms, reduced_motion))
        return false;
    place_panel(*s);
    ++s->revision;
    return true;
}

u64 ui_guild_card_next_wake(const UiGuildCardScene& s, bool reduced_motion) noexcept {
    return ui_hover_card_next_wake(s, kEntrance, reduced_motion);
}

bool ui_guild_card_contains(const UiGuildCardScene& s, float x, float y) noexcept {
    return ui_hover_card_contains(s, x, y);
}

UiGuildCardControl ui_guild_card_hit(const UiGuildCardScene& s, float x, float y) noexcept {
    if (!s.active)
        return UiGuildCardControl::none;
    if (s.action == UiGuildCardAction::go_to_server || s.action == UiGuildCardAction::join)
        if (ui_rect_contains(at(s, s.button), x, y))
            return UiGuildCardControl::action;
    if (s.phase == UiGuildCardPhase::failed && ui_rect_contains(s.panel, x, y))
        return UiGuildCardControl::retry;
    return UiGuildCardControl::none;
}

bool ui_guild_card_hint(const UiGuildCardScene& s, float x, float y, UiGuildCardHint* output) noexcept {
    if (!s.active || s.phase != UiGuildCardPhase::ready)
        return false;
    const auto over = [&](const UiRect& r, UiGuildCardHintKind kind, Utf8View text, u32 index) noexcept {
        const auto anchor = ui_rect_has_area(r) ? ui_rect_intersection(at(s, r), s.panel) : UiRect{};
        if (!text.bytes || !ui_rect_has_area(anchor) || !ui_rect_contains(anchor, x, y))
            return false;
        *output = {kind, index, anchor, text, {}};
        return true;
    };
    if (over(s.server_badge, UiGuildCardHintKind::server_badge, {s.badge_hint, s.badge_hint_bytes}, 0) ||
        over(s.overflow, UiGuildCardHintKind::traits, {s.overflow_hint, s.overflow_hint_bytes}, 0))
        return true;
    if (over(s.games_more, UiGuildCardHintKind::games, {s.games_more_hint, s.games_more_hint_bytes}, 0)) {
        output->media = {s.games_more_keys, kUiGuildCardGamesMore};
        return true;
    }
    // A lone game's name stands beside it: its icon offers none.
    for (u32 i = 0; i < s.shown_games && !ui_rect_has_area(s.game_name); ++i)
        if (over(s.games[i], UiGuildCardHintKind::game, {s.game_name_text[i], s.game_name_bytes[i]}, i))
            return true;
    return false;
}

u32 ui_guild_card_visible_traits(const float* widths, u32 count, float overflow_width, float width, u32 lines) noexcept {
    if (!widths || !count || !lines)
        return 0;
    // Math.round of the non-negative sums the official list compares.
    const auto rounded = [](float value) noexcept { return static_cast<float>(static_cast<u32>(value + .5f)); };
    u32 line = 0, last = 0;
    float x = 0;
    for (u32 i = 0; i < count; ++i) {
        const float w = widths[i];
        if (!(w >= 0 && w <= width))
            continue;
        const bool last_line = line == lines - 1;
        const float reserve = last_line && i + 1 < count ? overflow_width + kUiGuildCardTraitGap : 0;
        if (!(rounded(x + w + reserve) < rounded(width))) {
            if (last_line)
                break;
            ++line;
            x = 0;
        }
        last = i;
        x += w + kUiGuildCardTraitGap;
    }
    return last + 1;
}

UiGuildCardBadge ui_guild_card_badge(const UiGuildCardServer& server) noexcept {
    if (server.verified)
        return UiGuildCardBadge::verified;
    if (server.partnered)
        return UiGuildCardBadge::partnered;
    if (server.community)
        return server.discoverable ? UiGuildCardBadge::discoverable : UiGuildCardBadge::community;
    return server.boosts || server.premium_tier ? UiGuildCardBadge::boost : UiGuildCardBadge::none;
}

bool ui_guild_card_set_hover(UiGuildCardScene* s, float x, float y) noexcept {
    return ui_hover_card_set_hovered(s, ui_guild_card_hit(*s, x, y) == UiGuildCardControl::action);
}

bool ui_guild_card_set_pressed(UiGuildCardScene* s, bool pressed) noexcept {
    return ui_hover_card_set_pressed(s, pressed);
}

bool ui_guild_card_spinner_visible(const UiGuildCardScene& s) noexcept {
    return s.active && s.phase == UiGuildCardPhase::loading;
}

bool ui_guild_card_media(const UiGuildCardScene& s, u32 index, UiGuildCardMedia* output) noexcept {
    if (!s.active)
        return false;
    if (index == 0 && media_asset_key_valid(s.icon_key) && s.phase != UiGuildCardPhase::loading) {
        const auto r = at(s, s.icon);
        *output = {s.icon_key, r, s.panel, 18, clamp_unit(s.progress)};
        return true;
    }
    if (index == 1 && media_asset_key_valid(s.badge_key) && ui_rect_has_area(s.tag_badge)) {
        const auto r = at(s, s.tag_badge);
        *output = {s.badge_key, r, s.panel, 0, clamp_unit(s.progress)};
        return true;
    }
    // Game pictures sit inside their 1 DIP edge; the one under "+N" shows through the official 72% scrim.
    const auto inset = [](UiRect r) noexcept {
        return UiRect{r.left + kGameEdge, r.top + kGameEdge, r.right - kGameEdge, r.bottom - kGameEdge};
    };
    if (index >= kUiGuildCardGameMedia && index - kUiGuildCardGameMedia < s.shown_games &&
        media_asset_key_valid(s.game_keys[index - kUiGuildCardGameMedia])) {
        const auto r = inset(at(s, s.games[index - kUiGuildCardGameMedia]));
        *output = {s.game_keys[index - kUiGuildCardGameMedia], r, s.panel, kGameRadius - kGameEdge,
                   clamp_unit(s.progress)};
        return true;
    }
    if (index == kUiGuildCardGamesMoreMedia && media_asset_key_valid(s.games_more_keys[0]) &&
        ui_rect_has_area(s.games_more)) {
        *output = {s.games_more_keys[0], inset(at(s, s.games_more)), s.panel, kGameRadius - kGameEdge,
                   clamp_unit(s.progress) * kGamesMoreShow};
        return true;
    }
    if (index >= kUiGuildCardTraitEmojiMedia && index - kUiGuildCardTraitEmojiMedia < s.visible_traits &&
        media_asset_key_valid(s.trait_emoji_keys[index - kUiGuildCardTraitEmojiMedia])) {
        const u32 i = index - kUiGuildCardTraitEmojiMedia;
        *output = {s.trait_emoji_keys[i], at(s, trait_emoji_square(s, i)), s.panel, 0, clamp_unit(s.progress)};
        return true;
    }
    if (index == kUiGuildCardBannerMedia && media_asset_key_valid(s.banner_key)) {
        // Centred cover fit; the card rounds its top corners.
        const auto r = at(s, s.banner);
        *output = {s.banner_key, r, ui_rect_intersection(r, s.panel), 0, clamp_unit(s.progress),
                   {r.left, r.top, r.right, r.bottom + kRadius}, kRadius, true};
        return true;
    }
    return false;
}

Color ui_guild_card_banner_highlight(Color base) noexcept {
    // chroma.brighten(1.75): CIELAB L* + 31.5, about +0.27 Oklab lightness.
    auto lab = background_color_oklab(base);
    lab.lightness = lab.lightness + .27 < 1 ? lab.lightness + .27 : 1.;
    return background_color_from_oklab(lab, base.alpha);
}

u32 ui_guild_card_desaturate(u32 rgb, float saturation) noexcept {
    if (!(saturation < 1))
        return rgb;
    auto hsl = background_color_hsl(color_from_rgb24(rgb));
    hsl.saturation *= saturation > 0 ? saturation : 0;
    const Color c = background_color_from_hsl(hsl);
    // tinycolor's hex: each channel rounded to a byte.
    const auto byte = [](float v) noexcept {
        return static_cast<u32>((v < 0 ? 0 : v > 1 ? 1 : v) * 255 + .5f);
    };
    return byte(c.red) << 16 | byte(c.green) << 8 | byte(c.blue);
}

bool ui_guild_profile_banner_paint(PaintStream* paint, const UiRect& rectangle, Color base, float radius,
                                   float gradient_height) noexcept {
    return paint_stream_push_banner_glow(paint, rectangle, base, ui_guild_card_banner_highlight(base), radius,
                                         gradient_height > 0 ? gradient_height : ui_rect_height(rectangle));
}

bool ui_guild_card_paint(const UiGuildCardScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!s.active)
        return true;
    UiPaint p{paint, theme};
    const float a = clamp_unit(s.progress);
    p.use<ui_hover_card_paint_surface>(s, theme, kRadius).clip(s.panel);
    // Banner: the glow of the brand (else icon) colour, else of the theme's
    // server banner colour, its top corners rounded with the card and its
    // bottom edge square. A custom banner picture (media) covers it.
    const auto banner = at(s, s.banner);
    const Color base = s.has_banner_color ? color_from_rgb24(s.banner_color, a)
                                          : ui_theme_color_alpha(theme.tokens.profile.server_banner, a);
    const UiRect cover{banner.left, banner.top, banner.right, banner.bottom + kRadius};
    return p.clip(banner).use<ui_guild_profile_banner_paint>(cover, base, kRadius, kUiGuildCardBanner).unclip().end();
}

bool ui_guild_card_paint_foreground(const UiGuildCardScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!s.active)
        return true;
    UiPaint p{paint, theme};
    const float a = clamp_unit(s.progress);
    const Color fill = ui_theme_color_alpha(theme.tokens.background.floating, a);
    p.clip(s.panel);
    // Icon: a ring in the card colour, then the plate the host's icon covers.
    p.rounded(at(s, s.icon_ring), fill, theme.scale.radius_xl + kRing)
        .rounded(at(s, s.icon), ui_theme_color_alpha(theme.surface_high, a), theme.scale.radius_xl);
    if (s.phase == UiGuildCardPhase::loading)
        p.spinner(at(s, s.status_icon), ui_theme_color_alpha(theme.tokens.text.normal, a), s.spinner_phase);
    if (s.phase == UiGuildCardPhase::private_profile || s.phase == UiGuildCardPhase::failed)
        p.icon(s.phase == UiGuildCardPhase::private_profile ? UiIcon::lock : UiIcon::warning, at(s, s.status_icon),
               ui_theme_color_alpha(theme.tokens.text.muted, a));
    if (ui_rect_has_area(s.server_badge)) {
        // A filled scalloped badge with its mark: the boost gem by level, a check, a star, or the community house
        // (a public server's compass), in the boosting pink while the server has boosts.
        const Color on = theme.tokens.text.on_accent;
        const bool lit = s.boosted && (s.server_badge_kind == UiGuildCardBadge::community ||
                                       s.server_badge_kind == UiGuildCardBadge::discoverable);
        Color shape = theme.tokens.accent.boost_badge, mark = on;
        UiIcon glyph = UiIcon::boost;
        float glyph_size = 14;
        switch (s.server_badge_kind) {
        case UiGuildCardBadge::boost:
            mark = s.boost_tier == 3 ? theme.tokens.accent.boost : on;
            if (!s.boost_tier)
                mark.alpha *= .2f;
            break;
        case UiGuildCardBadge::verified:
            shape = theme.tokens.accent.success;
            glyph = UiIcon::check;
            break;
        case UiGuildCardBadge::partnered:
            shape = theme.tokens.accent.app_badge;
            glyph = UiIcon::favorite;
            break;
        default:
            shape = lit ? theme.tokens.accent.boost : theme.tokens.text.normal;
            mark = lit ? on : theme.tokens.background.floating;
            glyph = s.server_badge_kind == UiGuildCardBadge::discoverable ? UiIcon::compass : UiIcon::home;
            glyph_size = 12;
            break;
        }
        shape = ui_theme_color_alpha(shape, a);
        const auto r = at(s, s.server_badge);
        const float cx = (r.left + r.right) * .5f, cy = (r.top + r.bottom) * .5f;
        // The outline spans 18.2 of its 24 unit box: a 22 DIP box draws an 18 DIP badge with its stroke.
        p.use<ui_icon_paint_styled>(UiIcon::badge_check, UiRect{cx - 11, cy - 11, cx + 11, cy + 11},
                                    UiIconStyle{shape, shape, 0})
            .icon(glyph, {cx - glyph_size * .5f, cy - glyph_size * .5f, cx + glyph_size * .5f, cy + glyph_size * .5f},
                  ui_theme_color_alpha(mark, a));
    }
    // The tag chip as beside an author's name: the muted fill, no border.
    if (ui_rect_has_area(s.tag_chip))
        p.rounded(at(s, s.tag_chip), ui_theme_color_alpha(theme.tokens.fill.muted, a), theme.scale.radius_sm);
    if (ui_rect_has_area(s.online_dot))
        p.rounded(at(s, s.online_dot), ui_theme_color_alpha(theme.tokens.status.online, a), theme.scale.radius_full)
            .rounded(at(s, s.members_dot), ui_theme_color_alpha(theme.tokens.status.offline, a),
                     theme.scale.radius_full);
    // Trait chips (and "+ N more") are outlined pills.
    const Color outline = ui_theme_color_alpha(theme.tokens.border.outline, a);
    for (u32 i = 0; p && i < s.visible_traits; ++i)
        p.rounded_border(at(s, s.traits[i]), Color{}, outline, theme.scale.radius_full, 1);
    if (ui_rect_has_area(s.overflow))
        p.rounded_border(at(s, s.overflow), Color{}, outline, theme.scale.radius_full, 1);
    // Game pictures (the host's media) sit inside a faint 1 DIP edge; "+N" darkens the next one.
    const Color edge = ui_theme_color_alpha(theme.tokens.border.muted, a);
    for (u32 i = 0; p && i < s.shown_games; ++i)
        p.rounded_border(at(s, s.games[i]), ui_theme_color_alpha(theme.surface_high, a), edge, kGameRadius,
                         kGameEdge);
    if (ui_rect_has_area(s.games_more))
        p.rounded_border(at(s, s.games_more), ui_theme_color_alpha(theme.tokens.overlay.scrim_strong, a), edge,
                         kGameRadius, kGameEdge);
    if (ui_rect_has_area(s.button)) {
        p.rounded(at(s, s.button),
                  ui_theme_color_alpha(ui_hover_card_button_color(s, theme),
                                       s.action == UiGuildCardAction::joining ? a * .5f : a),
                  theme.scale.radius_md);
    }
    return p.end();
}

bool ui_guild_card_paint_overlay(const UiGuildCardScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!s.active)
        return true;
    UiPaint p{paint, theme};
    const float a = clamp_unit(s.progress);
    p.clip(s.panel);
    // A busy game's marker over its picture's top right corner (4 DIPs past it): a 14 DIP disc in the highest
    // surface around a filled 10 DIP flame.
    for (u32 i = 0; p && i < s.shown_games; ++i) {
        if (!s.game_high[i])
            continue;
        const auto g = at(s, s.games[i]);
        const UiRect disc{g.right + 4 - kGameMarker, g.top - 4, g.right + 4, g.top - 4 + kGameMarker};
        const float cx = (disc.left + disc.right) * .5f, cy = (disc.top + disc.bottom) * .5f, half = kGameFlame * .5f;
        const Color flame = ui_theme_color_alpha(theme.tokens.accent.game_activity, a);
        p.rounded(disc, ui_theme_color_alpha(theme.surface_high, a), theme.scale.radius_full)
            .use<ui_icon_paint_styled>(UiIcon::flame, UiRect{cx - half, cy - half, cx + half, cy + half},
                                       UiIconStyle{flame, flame, 0});
    }
    return p.end();
}

// The ready card's plain lines: where each one's text, byte count and panel-relative rectangle sit in the scene,
// its colour (strong, muted, the "+N" overlay's), its sideways inset, text style and text mode.
struct TextLine {
    u16 text, bytes, rect;
    u8 color, inset;
    u32 style, mode;
};
enum : u8 { line_strong, line_muted, line_overlay };
#define MAICORD_CARD_AT(member) static_cast<u16>(__builtin_offsetof(UiGuildCardScene, member))
#define MAICORD_CARD_LINE(text, bytes, rect, color, inset, style, mode) \
    {MAICORD_CARD_AT(text), MAICORD_CARD_AT(bytes), MAICORD_CARD_AT(rect), color, inset, style, mode}
// Before the trait chips (the first six) and after them.
constexpr TextLine kTextLines[]{
    MAICORD_CARD_LINE(name_text, name_bytes, name, line_strong, 0, kNameStyle, 0),
    MAICORD_CARD_LINE(tag, tag_bytes, tag_text, line_strong, 0, kTagStyle, 0),
    MAICORD_CARD_LINE(online_text, online_bytes, online, line_muted, 0, kInfoStyle, 0),
    MAICORD_CARD_LINE(members_text, members_bytes, members, line_muted, 0, kInfoStyle, 0),
    MAICORD_CARD_LINE(established_text, established_bytes, established, line_muted, 0, kInfoStyle, 0),
    MAICORD_CARD_LINE(description_text, description_bytes, description, line_muted, 0, kInfoStyle, ui_text_wrap),
    MAICORD_CARD_LINE(game_name_text, game_name_bytes, game_name, line_strong, 0, kInfoStyle, 0),
    MAICORD_CARD_LINE(games_more_text, games_more_bytes, games_more, line_overlay, 0, kTextStyleSizeCodeXs,
                      ui_text_center),
    MAICORD_CARD_LINE(overflow_text, overflow_bytes, overflow, line_strong, static_cast<u8>(kTraitInset), kInfoStyle, 0),
};
#undef MAICORD_CARD_LINE
#undef MAICORD_CARD_AT
constexpr u32 kLinesBeforeTraits = 6;

bool ui_guild_card_text_output(const UiGuildCardScene& s, const Theme& theme, UiTextOutput* out) noexcept {
    if (!s.active)
        return true;
    // A pinned preview scrolls with its page: its text stays inside the page's visible area.
    const UiRect clip = s.pinned ? ui_rect_intersection(s.panel, s.viewport) : s.panel;
    UiTextPen<UiTextClip::region> text{out, s.widgets, clip};
    const auto owner = s.owner.handle;
    const float a = clamp_unit(s.progress);
    const Color strong = ui_theme_color_alpha(theme.tokens.text.normal, a), muted = ui_theme_color_alpha(theme.tokens.text.muted, a);
    if (!media_asset_key_valid(s.icon_key) && s.phase == UiGuildCardPhase::ready) {
        char initials[8];
        const u32 bytes = acronym(s, initials);
        if (bytes && !ui_text_output_push_owned(out, {{initials, bytes}, s.owner, at(s, s.icon), clip, strong,
                                                      text_style_bold, UiTextAlignment::center,
                                                      UiTextAlignment::center, TextWrapping::no_wrap,
                                                      TextOverflow::ellipsis}))
            text.fail();
    }
    if (s.phase == UiGuildCardPhase::private_profile || s.phase == UiGuildCardPhase::failed) {
        const bool closed = s.phase == UiGuildCardPhase::private_profile;
        text(owner, closed ? utf8_literal("Private Server") : utf8_literal("Unknown Server"), at(s, s.status_title),
             strong, text_style_bold);
        return text(owner,
                    closed ? utf8_literal("The server has limited who can see this profile.")
                           : utf8_literal("Unable to load server profile. Click to try again."),
                    at(s, s.status_text), muted, text_style_subtext, ui_text_wrap)
            .ok();
    }
    if (s.phase != UiGuildCardPhase::ready)
        return text.ok();
    const Color colors[]{strong, muted, ui_theme_color_alpha(theme.tokens.overlay.text, a)};
    const auto* base = reinterpret_cast<const u8*>(&s);
    const auto line = [&](const TextLine& row) noexcept {
        const UiRect r = at(s, *reinterpret_cast<const UiRect*>(base + row.rect));
        text(owner, {reinterpret_cast<const char*>(base + row.text), *reinterpret_cast<const u32*>(base + row.bytes)},
             {r.left + row.inset, r.top, r.right - row.inset, r.bottom}, colors[row.color], row.style, row.mode);
    };
    for (u32 i = 0; i < kLinesBeforeTraits; ++i)
        line(kTextLines[i]);
    for (u32 i = 0; i < s.visible_traits; ++i) {
        const auto r = at(s, s.traits[i]);
        const float lead = trait_has_emoji(s, i) ? kEmoji + kEmojiGap : 0;
        // A Unicode emoji is text in its square; a custom one is the host's media.
        if (s.trait_emoji_bytes[i])
            text(owner, {s.trait_emoji[i], s.trait_emoji_bytes[i]}, at(s, trait_emoji_square(s, i)), strong, kInfoStyle,
                 ui_text_center);
        text(owner, {s.trait_text[i], s.trait_bytes[i]},
             {r.left + kTraitInset + lead, r.top, r.right - kTraitInset, r.bottom}, strong, kInfoStyle);
    }
    for (u32 i = kLinesBeforeTraits; i < sizeof(kTextLines) / sizeof(kTextLines[0]); ++i)
        line(kTextLines[i]);
    return text(owner, action_label(s.action), at(s, s.button), ui_theme_color_alpha(theme.tokens.text.on_accent, a),
                text_style_bold, ui_text_center)
        .ok();
}
} // namespace maicord
