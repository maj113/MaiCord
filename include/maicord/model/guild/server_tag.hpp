#pragma once

#include "maicord/protocol/guild/admin.hpp"

namespace maicord {

// Server Tag page: the badges a server may pick, the preset colour pairs and
// the page's draft over the tag fields of GET /guilds/{g}/profile
// (Userdoccers: Discovery, Guild Profile Structure and Guild Badge Type).
// Setting any of them needs the GUILD_TAGS feature and Manage Server.

// One badge type that may still be set. `tints` is how many colours it
// takes (1: primary; 2: primary and secondary); `primary` and `secondary` are
// its own colours (0xRRGGBB), shown while the profile's colours are null.
// A pack badge needs its pack feature (guild_feature_badge_pack_*).
struct GuildTagBadge {
    Utf8View name;
    u64 pack;
    u32 primary, secondary;
    u8 type, tints;
};
// The base set in the official picker's order, then the four packs (pets,
// flex, plant, creepy crawlies) in type order.
inline constexpr u32 kGuildTagBadgeCount = 30;
inline constexpr u32 kGuildTagBaseBadgeCount = 10;
extern const GuildTagBadge kGuildTagBadges[kGuildTagBadgeCount];
// The picker's first badge (Leaf): what a profile without a badge shows.
inline constexpr u8 kGuildTagDefaultBadge = 6;
// Null for a type that can no longer be set.
const GuildTagBadge* guild_tag_badge(u8 type) noexcept;
// A base badge, or one whose pack the server has (`features`: guild_feature_* bits).
bool guild_tag_badge_unlocked(const GuildTagBadge& badge, u64 features) noexcept;

// "Choose Color": the preset pairs, then "Reset to default colors" (both
// null) and the custom pair.
struct GuildTagPalette {
    u32 primary, secondary;
    Utf8View name;
};
inline constexpr u32 kGuildTagPaletteCount = 13;
inline constexpr u32 kGuildTagPaletteDefault = kGuildTagPaletteCount;
inline constexpr u32 kGuildTagPaletteCustom = kGuildTagPaletteCount + 1;
extern const GuildTagPalette kGuildTagPalettes[kGuildTagPaletteCount];

// The page's draft. The tag text survives turning the tag off, so turning it
// back on restores it; an unset colour is null (the badge's own).
struct GuildTagDraft {
    char tag[kGuildAdminTagBytes];
    u8 tag_bytes;
    bool enabled;
    u8 badge;
    bool primary_set, secondary_set;
    u32 primary, secondary;
};
// A null or empty tag is off; a missing badge or one that can no longer be
// set reads as the default badge; a colour that is not #RRGGBB reads as null.
GuildTagDraft guild_tag_draft(const GuildAdminProfile& profile) noexcept;
bool guild_tag_draft_equal(const GuildTagDraft&, const GuildTagDraft&) noexcept;
// Replaces the text (cut at kGuildAdminTagBytes on a character boundary).
void guild_tag_set_text(GuildTagDraft*, Utf8View text) noexcept;
// Whether the tag the draft would save differs from the stored one: the
// official page then warns that members must adopt the tag again.
bool guild_tag_renamed(const GuildTagDraft& draft, const GuildAdminProfile& stored) noexcept;
// The selected Choose Color tile: a preset index, kGuildTagPaletteDefault or
// kGuildTagPaletteCustom.
u32 guild_tag_palette_choice(const GuildTagDraft&) noexcept;
// Sets both colours to a preset, or clears both for kGuildTagPaletteDefault.
void guild_tag_set_palette(GuildTagDraft*, u32 choice) noexcept;
// The colours the badge shows: the draft's, else the badge's own.
u32 guild_tag_primary(const GuildTagDraft&) noexcept;
u32 guild_tag_secondary(const GuildTagDraft&) noexcept;
// Whether a badge drawn as (type, primary, secondary) is the one `saved` names, in its colours: the server's own image
// of its badge (the CDN's) then shows exactly what the page would draw. A one-colour badge has no secondary to match.
bool guild_tag_badge_saved(const GuildTagDraft& saved, u8 type, u32 primary, u32 secondary) noexcept;
// Whether the draft saves: off, or on with a valid tag.
bool guild_tag_draft_valid(const GuildTagDraft&) noexcept;
// The PATCH body of a draft; `colors` holds the "#rrggbb" texts the write
// borrows, as it borrows the draft's tag. False for an invalid draft.
struct GuildTagColorText {
    char primary[8], secondary[8];
};
bool guild_tag_write(const GuildTagDraft& draft, GuildTagColorText* colors, GuildTagWrite* write) noexcept;

} // namespace maicord
