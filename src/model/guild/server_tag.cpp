#include "maicord/model/guild/server_tag.hpp"
#include "maicord/core/color_hex.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/guild/detail_store.hpp"

namespace maicord {

// Names as the official picker reads them; the colours are each badge's own before a tint, from the official client's
// badge set.
constexpr char kGuildTagBadgeNames[] =
    "Leaf\0Sword\0Heart\0Fire\0Water Drop\0Skull\0Moon\0Lightning\0Compass\0Toadstool\0"
    "Bunny\0Dog\0Frog\0Goat\0Cat\0Diamond\0Crown\0Trophy\0Money Bag\0Dollar Sign\0"
    "Clover\0Blossom\0Potted Plant\0Maple Leaf\0Wilted Flower\0Butterfly\0Snail\0Caterpillar\0Spider\0Bee\0";
static_assert(name_list_count(kGuildTagBadgeNames) == kGuildTagBadgeCount);
constexpr u8 kPets = 33, kFlex = 34, kPlant = 35, kCrawlies = 36;
static_assert(1ull << kPets == guild_feature_badge_pack_pets && 1ull << kFlex == guild_feature_badge_pack_flex &&
              1ull << kPlant == guild_feature_badge_pack_plant &&
              1ull << kCrawlies == guild_feature_badge_pack_creepy_crawlies);
const GuildTagBadge kGuildTagBadges[kGuildTagBadgeCount]{
    {0x7fb134, 0, 6, 1, 0},
    {0xffe361, 0xd1cdd5, 0, 2, 0},
    {0xff7fc0, 0, 7, 1, 0},
    {0xfd6214, 0, 8, 1, 0},
    {0x0abbff, 0, 1, 1, 0},
    {0x847d8b, 0, 2, 1, 0},
    {0xb79cf8, 0, 4, 1, 0},
    {0xffe361, 0, 5, 1, 0},
    {0xff2c52, 0x47baff, 9, 2, 0},
    {0xff2c52, 0xffe361, 3, 2, 0},
    {0xd1cdd5, 0, 21, 1, kPets},
    {0xf6a830, 0xffe880, 22, 2, kPets},
    {0x6be473, 0xffe361, 23, 2, kPets},
    {0xb88166, 0, 24, 1, kPets},
    {0x74767f, 0, 25, 1, kPets},
    {0x00bbff, 0, 26, 1, kFlex},
    {0xffe361, 0xff7fc0, 27, 2, kFlex},
    {0xffb84b, 0, 28, 1, kFlex},
    {0xffe361, 0, 29, 1, kFlex},
    {0x6be473, 0, 30, 1, kFlex},
    {0x29bb21, 0, 31, 1, kPlant},
    {0xff4cd2, 0, 32, 1, kPlant},
    {0x027d46, 0xffae49, 33, 2, kPlant},
    {0x730e17, 0, 34, 1, kPlant},
    {0x800e6f, 0x2dc92d, 35, 2, kPlant},
    {0x9af2ff, 0x5c0084, 36, 2, kCrawlies},
    {0xc7531a, 0xe6d23b, 37, 2, kCrawlies},
    {0x1daa5a, 0xe6cf1e, 38, 2, kCrawlies},
    {0xda1846, 0x1f1f1f, 39, 2, kCrawlies},
    {0xfae330, 0, 40, 1, kCrawlies},
};

const GuildTagPalette kGuildTagPalettes[kGuildTagPaletteCount]{
    {0xff1c90, 0xff7fc0}, {0xff8f1c, 0xffae77}, {0xeebe1a, 0xfffc7f}, {0x32a070, 0x57b59e}, {0x32839a, 0x71c2d9},
    {0x8a43ff, 0xbd95ff}, {0x9b3fe5, 0xcc8dff}, {0x942e8f, 0xd46cb5}, {0xd14242, 0xff8989}, {0x814300, 0xa88a6c},
    {0x717224, 0xc3c3c3}, {0x5d1d47, 0xc58fbb}, {0x222222, 0xcccccc},
};
constexpr char kGuildTagPaletteNames[] =
    "Hot pink and light pink\0Bright orange and peach\0Golden yellow and light yellow\0Teal and mint green\0"
    "Blue teal and light blue\0Purple and lavender\0Violet and mauve\0Deep purple and orchid\0Red and salmon\0"
    "Brown and tan\0Olive green and gray\0Burgundy and rose\0Dark gray and light gray\0";
static_assert(name_list_count(kGuildTagPaletteNames) == kGuildTagPaletteCount);

const GuildTagBadge* guild_tag_badge(u8 type) noexcept {
    for (const auto& badge : kGuildTagBadges)
        if (badge.type == type)
            return &badge;
    return nullptr;
}

bool guild_tag_badge_unlocked(const GuildTagBadge& badge, u64 features) noexcept {
    return !badge.pack || (features >> badge.pack & 1);
}

namespace {

bool read_color(const char* text, u8 bytes, u32* color) noexcept {
    // Only "#RRGGBB": the API's form, never the shorthand.
    return bytes == 7 && text[0] == '#' && color_hex_parse_rgb24({text, bytes}, color);
}

void format_color(u32 color, char* output) noexcept {
    output[0] = '#';
    for (u32 i = 0; i < 6; ++i)
        output[1 + i] = ascii_hex_digit(color >> ((5 - i) * 4));
}

const GuildTagBadge& badge_of(const GuildTagDraft& draft) noexcept {
    const auto* badge = guild_tag_badge(draft.badge);
    return badge ? *badge : kGuildTagBadges[0];
}

} // namespace

GuildTagDraft guild_tag_draft(const GuildAdminProfile& profile) noexcept {
    GuildTagDraft draft{};
    guild_tag_set_text(&draft, {profile.tag, profile.tag_bytes});
    draft.enabled = draft.tag_bytes != 0;
    draft.badge =
        profile.badge_known && guild_tag_badge(profile.badge) ? profile.badge : kGuildTagDefaultBadge;
    draft.primary_set = read_color(profile.badge_color_primary, profile.badge_color_primary_bytes, &draft.primary);
    draft.secondary_set =
        read_color(profile.badge_color_secondary, profile.badge_color_secondary_bytes, &draft.secondary);
    if (!draft.primary_set)
        draft.primary = 0;
    if (!draft.secondary_set)
        draft.secondary = 0;
    return draft;
}

bool guild_tag_draft_equal(const GuildTagDraft& a, const GuildTagDraft& b) noexcept {
    const bool same_tag = !a.enabled || (a.tag_bytes == b.tag_bytes && bytes_equal(a.tag, b.tag, a.tag_bytes));
    return a.enabled == b.enabled && same_tag && a.badge == b.badge && a.primary_set == b.primary_set &&
           a.secondary_set == b.secondary_set && (!a.primary_set || a.primary == b.primary) &&
           (!a.secondary_set || a.secondary == b.secondary);
}

void guild_tag_set_text(GuildTagDraft* draft, Utf8View text) noexcept {
    if (!draft || (!text.data && text.bytes))
        return;
    const u32 bytes = utf8_truncated_length(text, kGuildAdminTagBytes);
    copy_bytes(draft->tag, text.data, bytes);
    draft->tag_bytes = static_cast<u8>(bytes);
}

bool guild_tag_renamed(const GuildTagDraft& draft, const GuildAdminProfile& stored) noexcept {
    const u32 bytes = draft.enabled ? draft.tag_bytes : 0;
    return bytes != stored.tag_bytes || !bytes_equal(draft.tag, stored.tag, bytes);
}

u32 guild_tag_palette_choice(const GuildTagDraft& draft) noexcept {
    if (!draft.primary_set && !draft.secondary_set)
        return kGuildTagPaletteDefault;
    for (u32 i = 0; i < kGuildTagPaletteCount; ++i)
        if (draft.primary_set && draft.secondary_set && draft.primary == kGuildTagPalettes[i].primary &&
            draft.secondary == kGuildTagPalettes[i].secondary)
            return i;
    return kGuildTagPaletteCustom;
}

void guild_tag_set_palette(GuildTagDraft* draft, u32 choice) noexcept {
    if (!draft || choice > kGuildTagPaletteDefault)
        return;
    const bool preset = choice < kGuildTagPaletteCount;
    draft->primary_set = draft->secondary_set = preset;
    draft->primary = preset ? kGuildTagPalettes[choice].primary : 0;
    draft->secondary = preset ? kGuildTagPalettes[choice].secondary : 0;
}

u32 guild_tag_primary(const GuildTagDraft& draft) noexcept {
    return draft.primary_set ? draft.primary : badge_of(draft).primary;
}

u32 guild_tag_secondary(const GuildTagDraft& draft) noexcept {
    const auto& badge = badge_of(draft);
    return draft.secondary_set ? draft.secondary : badge.tints > 1 ? badge.secondary : badge.primary;
}

bool guild_tag_badge_saved(const GuildTagDraft& saved, u8 type, u32 primary, u32 secondary) noexcept {
    const auto* badge = guild_tag_badge(type);
    return badge && type == saved.badge && primary == guild_tag_primary(saved) &&
           (badge->tints < 2 || secondary == guild_tag_secondary(saved));
}

bool guild_tag_draft_valid(const GuildTagDraft& draft) noexcept {
    return guild_tag_badge(draft.badge) && (!draft.enabled || guild_tag_text_valid({draft.tag, draft.tag_bytes}));
}

bool guild_tag_write(const GuildTagDraft& draft, GuildTagColorText* colors, GuildTagWrite* write) noexcept {
    if (!colors || !write || !guild_tag_draft_valid(draft))
        return false;
    GuildTagWrite value{};
    if (draft.enabled)
        value.tag = {draft.tag, draft.tag_bytes};
    value.badge = draft.badge;
    if (draft.primary_set) {
        format_color(draft.primary, colors->primary);
        value.badge_color_primary = {colors->primary, 7};
    }
    if (draft.secondary_set) {
        format_color(draft.secondary, colors->secondary);
        value.badge_color_secondary = {colors->secondary, 7};
    }
    *write = value;
    return true;
}

} // namespace maicord
