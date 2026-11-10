#include "maicord/model/guild/server_tag.hpp"
#include "maicord/core/color_hex.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/guild/detail_store.hpp"

namespace maicord {

// Names as the official picker reads them; the colours are each badge's own
// before a tint, from the official client's badge set.
const GuildTagBadge kGuildTagBadges[kGuildTagBadgeCount]{
    {utf8_literal("Leaf"), 0, 0x7fb134, 0, 6, 1},
    {utf8_literal("Sword"), 0, 0xffe361, 0xd1cdd5, 0, 2},
    {utf8_literal("Heart"), 0, 0xff7fc0, 0, 7, 1},
    {utf8_literal("Fire"), 0, 0xfd6214, 0, 8, 1},
    {utf8_literal("Water Drop"), 0, 0x0abbff, 0, 1, 1},
    {utf8_literal("Skull"), 0, 0x847d8b, 0, 2, 1},
    {utf8_literal("Moon"), 0, 0xb79cf8, 0, 4, 1},
    {utf8_literal("Lightning"), 0, 0xffe361, 0, 5, 1},
    {utf8_literal("Compass"), 0, 0xff2c52, 0x47baff, 9, 2},
    {utf8_literal("Toadstool"), 0, 0xff2c52, 0xffe361, 3, 2},
    {utf8_literal("Bunny"), guild_feature_badge_pack_pets, 0xd1cdd5, 0, 21, 1},
    {utf8_literal("Dog"), guild_feature_badge_pack_pets, 0xf6a830, 0xffe880, 22, 2},
    {utf8_literal("Frog"), guild_feature_badge_pack_pets, 0x6be473, 0xffe361, 23, 2},
    {utf8_literal("Goat"), guild_feature_badge_pack_pets, 0xb88166, 0, 24, 1},
    {utf8_literal("Cat"), guild_feature_badge_pack_pets, 0x74767f, 0, 25, 1},
    {utf8_literal("Diamond"), guild_feature_badge_pack_flex, 0x00bbff, 0, 26, 1},
    {utf8_literal("Crown"), guild_feature_badge_pack_flex, 0xffe361, 0xff7fc0, 27, 2},
    {utf8_literal("Trophy"), guild_feature_badge_pack_flex, 0xffb84b, 0, 28, 1},
    {utf8_literal("Money Bag"), guild_feature_badge_pack_flex, 0xffe361, 0, 29, 1},
    {utf8_literal("Dollar Sign"), guild_feature_badge_pack_flex, 0x6be473, 0, 30, 1},
    {utf8_literal("Clover"), guild_feature_badge_pack_plant, 0x29bb21, 0, 31, 1},
    {utf8_literal("Blossom"), guild_feature_badge_pack_plant, 0xff4cd2, 0, 32, 1},
    {utf8_literal("Potted Plant"), guild_feature_badge_pack_plant, 0x027d46, 0xffae49, 33, 2},
    {utf8_literal("Maple Leaf"), guild_feature_badge_pack_plant, 0x730e17, 0, 34, 1},
    {utf8_literal("Wilted Flower"), guild_feature_badge_pack_plant, 0x800e6f, 0x2dc92d, 35, 2},
    {utf8_literal("Butterfly"), guild_feature_badge_pack_creepy_crawlies, 0x9af2ff, 0x5c0084, 36, 2},
    {utf8_literal("Snail"), guild_feature_badge_pack_creepy_crawlies, 0xc7531a, 0xe6d23b, 37, 2},
    {utf8_literal("Caterpillar"), guild_feature_badge_pack_creepy_crawlies, 0x1daa5a, 0xe6cf1e, 38, 2},
    {utf8_literal("Spider"), guild_feature_badge_pack_creepy_crawlies, 0xda1846, 0x1f1f1f, 39, 2},
    {utf8_literal("Bee"), guild_feature_badge_pack_creepy_crawlies, 0xfae330, 0, 40, 1},
};

const GuildTagPalette kGuildTagPalettes[kGuildTagPaletteCount]{
    {0xff1c90, 0xff7fc0, utf8_literal("Hot pink and light pink")},
    {0xff8f1c, 0xffae77, utf8_literal("Bright orange and peach")},
    {0xeebe1a, 0xfffc7f, utf8_literal("Golden yellow and light yellow")},
    {0x32a070, 0x57b59e, utf8_literal("Teal and mint green")},
    {0x32839a, 0x71c2d9, utf8_literal("Blue teal and light blue")},
    {0x8a43ff, 0xbd95ff, utf8_literal("Purple and lavender")},
    {0x9b3fe5, 0xcc8dff, utf8_literal("Violet and mauve")},
    {0x942e8f, 0xd46cb5, utf8_literal("Deep purple and orchid")},
    {0xd14242, 0xff8989, utf8_literal("Red and salmon")},
    {0x814300, 0xa88a6c, utf8_literal("Brown and tan")},
    {0x717224, 0xc3c3c3, utf8_literal("Olive green and gray")},
    {0x5d1d47, 0xc58fbb, utf8_literal("Burgundy and rose")},
    {0x222222, 0xcccccc, utf8_literal("Dark gray and light gray")},
};

const GuildTagBadge* guild_tag_badge(u8 type) noexcept {
    for (const auto& badge : kGuildTagBadges)
        if (badge.type == type)
            return &badge;
    return nullptr;
}

bool guild_tag_badge_unlocked(const GuildTagBadge& badge, u64 features) noexcept {
    return !badge.pack || (features & badge.pack);
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
