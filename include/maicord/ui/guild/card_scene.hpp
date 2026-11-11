#pragma once
#include "maicord/media/core/asset_key.hpp"
#include "maicord/ui/widgets/hover_card.hpp"
#include "maicord/ui/core/paint.hpp"
#include "maicord/ui/text/presentation.hpp"

namespace maicord {
struct UiTextMeasureCache;

// Server tag popout, opened from a server tag chip: a card with the server's
// banner (custom picture, else a radial glow of its brand colour, else of its
// icon's colour), icon, name with the server badge and tag chip, online/member
// counts, founding month, description, traits and one action. Pure geometry
// and paint: the host fetches GET /guilds/{id}/profile, draws the banner, icon
// and tag badge into ui_guild_card_media and runs the action.
inline constexpr float kUiGuildCardWidth = 300, kUiGuildCardPadding = 16, kUiGuildCardGap = 8;
inline constexpr float kUiGuildCardBanner = 120, kUiGuildCardIcon = 64;
inline constexpr u32 kUiGuildCardNameBytes = 256, kUiGuildCardDescriptionBytes = 1024, kUiGuildCardTagBytes = 32;
inline constexpr u32 kUiGuildCardTraits = 5, kUiGuildCardTraitBytes = 96;
inline constexpr u32 kUiGuildCardEntranceMs = 220;
// Trait chips wrap onto at most this many lines; the rest fold into "+ N more".
inline constexpr u32 kUiGuildCardTraitLines = 2;
inline constexpr float kUiGuildCardTraitGap = 4;
// The games row: up to this many 26 DIP game icons (a 1 DIP edge around 24), then the next game's icon dimmed
// under "+N" for the rest; a single game shows its name beside its icon.
inline constexpr u32 kUiGuildCardGames = 5;
// "+N"'s tooltip lists at most this many of the other games, each beside its picture.
inline constexpr u32 kUiGuildCardGamesMore = 8;
inline constexpr u32 kUiGuildCardGameNameBytes = 64;
inline constexpr u32 kUiGuildCardEmojiBytes = 32;

enum class UiGuildCardPhase : u8 {
    loading,         // GET /guilds/{id}/profile in flight: spinner.
    ready,           // The profile.
    private_profile, // 50001: "Private Server".
    failed,          // Anything else: "Unable to load server profile" (click retries).
};
// The card's single full-width button.
enum class UiGuildCardAction : u8 {
    none,
    go_to_server, // Already a member.
    join,         // Discoverable server, or a known invite.
    joining,      // Join sent; disabled until the server arrives.
};
enum class UiGuildCardControl : u8 { none, action, retry };
// The badge between the server's name and its tag chip, as in the official
// client's server header: verification, partnership or community (public
// when also discoverable) first, else its boost level while it has boosts.
enum class UiGuildCardBadge : u8 { none, boost, verified, partnered, community, discoverable };
struct UiGuildCardServer {
    bool verified, partnered, community, discoverable;
    bool apply_to_join; // MEMBER_VERIFICATION_MANUAL_APPROVAL with the verification gate
    u8 premium_tier;    // boost level 0-3
    u32 boosts;         // premium_subscription_count
};
UiGuildCardBadge ui_guild_card_badge(const UiGuildCardServer&) noexcept;
struct UiGuildCardContent {
    u64 guild_id; // Its snowflake dates "Est. Mon YYYY".
    UiGuildCardPhase phase;
    Utf8View name, description, tag; // Copied.
    Utf8View traits[kUiGuildCardTraits];
    u32 trait_count;
    u32 online_count, member_count;
    // 0xRRGGBB when has_banner_color: the brand colour, else the icon's
    // colour; without one the banner glows in the theme's profile.server_banner.
    u32 banner_color;
    bool has_banner_color;
    UiGuildCardServer server;
    MediaAssetKey icon, badge; // Host keys (the badge leads the tag chip).
    MediaAssetKey banner;      // Custom banner picture, cover-fit over the colour.
    UiGuildCardAction action;
    // The profile's games (game_application_ids): the icons of the first ones and the total count.
    MediaAssetKey games[kUiGuildCardGames];
    u32 game_count;
    // Their names (the icons' tooltips; a lone game's label), the pictures of the games after the fifth (the first
    // dimmed under "+N", each beside its name in "+N"'s tooltip) and their names, one per line. Copied.
    Utf8View game_names[kUiGuildCardGames];
    MediaAssetKey games_more_icons[kUiGuildCardGamesMore];
    Utf8View games_more_names;
    // A game the server plays a lot (game_activity's level 2): a flame marks its icon.
    bool game_high[kUiGuildCardGames];
    // A trait's emoji before its label: a Unicode glyph, else a custom emoji the host draws.
    Utf8View trait_emoji[kUiGuildCardTraits];
    MediaAssetKey trait_emoji_media[kUiGuildCardTraits];
    // A preview pinned with its top-left corner at the anchor's (the Server Profile settings aside): no entrance.
    bool pinned;
};
struct UiGuildCardScene : UiHoverCard {
    // Panel-relative geometry, offset by the entrance in paint and text.
    UiRect banner, icon_ring, icon, name, server_badge, tag_chip, tag_badge, tag_text, online_dot, online, members_dot,
        members, established, description, traits[kUiGuildCardTraits], overflow, button, status_icon, status_title,
        status_text;
    char name_text[kUiGuildCardNameBytes], description_text[kUiGuildCardDescriptionBytes],
        tag[kUiGuildCardTagBytes], trait_text[kUiGuildCardTraits][kUiGuildCardTraitBytes];
    char online_text[32], members_text[32], established_text[32], overflow_text[16], games_more_text[16];
    // The games row: icons, then the "+N" slot; a lone game's name.
    UiRect games[kUiGuildCardGames], games_more, game_name;
    MediaAssetKey game_keys[kUiGuildCardGames], games_more_keys[kUiGuildCardGamesMore];
    u32 game_count, shown_games, games_more_bytes;
    char game_name_text[kUiGuildCardGames][kUiGuildCardGameNameBytes];
    u32 game_name_bytes[kUiGuildCardGames];
    bool game_high[kUiGuildCardGames];
    char games_more_hint[384];
    u32 games_more_hint_bytes;
    bool pinned;
    // Trait emoji (16 DIP squares before the labels): Unicode glyphs, else the host's custom emoji.
    char trait_emoji[kUiGuildCardTraits][kUiGuildCardEmojiBytes];
    u32 trait_emoji_bytes[kUiGuildCardTraits];
    MediaAssetKey trait_emoji_keys[kUiGuildCardTraits];
    // The folded traits, one per line with their emoji (the "+ N more" chip's tooltip, drawn as chips).
    char overflow_hint[kUiGuildCardTraits * (kUiGuildCardTraitBytes + kUiGuildCardEmojiBytes + 2)];
    // The server badge's tooltip: its title line, then its subtitle lines.
    char badge_hint[192];
    u32 badge_hint_bytes;
    u32 name_bytes, description_bytes, tag_bytes, trait_bytes[kUiGuildCardTraits], trait_count;
    u32 visible_traits; // traits laid out; the rest fold into the overflow chip
    u32 online_bytes, members_bytes, established_bytes, overflow_bytes, overflow_hint_bytes;
    u32 banner_color;
    MediaAssetKey icon_key, badge_key, banner_key;
    UiGuildCardPhase phase;
    UiGuildCardAction action;
    UiGuildCardBadge server_badge_kind;
    u8 boost_tier;
    bool boosted;
    bool has_banner_color;
};

// Shows (or re-projects) the card for identity (the guild) beside anchor
// inside viewport; measure sizes the description and traits when given.
bool ui_guild_card_show(UiGuildCardScene*, u64 identity, const UiGuildCardContent&, const UiRect& anchor,
                        const UiRect& viewport, u64 now_ms, bool reduced_motion, UiTextMeasureCache* measure) noexcept;
void ui_guild_card_close(UiGuildCardScene*) noexcept;
bool ui_guild_card_tick(UiGuildCardScene*, u64 now_ms, bool reduced_motion) noexcept;
u64 ui_guild_card_next_wake(const UiGuildCardScene&, bool reduced_motion) noexcept;
bool ui_guild_card_contains(const UiGuildCardScene&, float x, float y) noexcept;
UiGuildCardControl ui_guild_card_hit(const UiGuildCardScene&, float x, float y) noexcept;
// The tooltip of what lies under the pointer, as the official card offers them: the server badge (a card: its kind,
// then who can join and its boost level; below it), "+ N more" (the folded traits as chips; above it), a game icon
// (its name; below) and "+N" (the other games beside their pictures; below).
enum class UiGuildCardHintKind : u8 { none, server_badge, traits, game, games };
struct UiGuildCardHint {
    UiGuildCardHintKind kind;
    u32 index; // the game
    UiRect anchor;
    Utf8View text; // borrowed from the scene
    // "+N": the other games' pictures, one per line of text.
    ConstSlice<MediaAssetKey> media;
};
bool ui_guild_card_hint(const UiGuildCardScene&, float x, float y, UiGuildCardHint* output) noexcept;
// An icon-derived banner colour under the accessibility saturation setting (Apply to custom colours): its HSL
// saturation times saturation (0-1), as the official client tints avatar colours.
u32 ui_guild_card_desaturate(u32 rgb, float saturation) noexcept;
// How many traits show before "+ N more", as the official client's overflow list decides it: chips (widths, gap
// apart) fill at most `lines` lines of `width`, and the last line keeps room for an overflow chip of overflow_width
// unless the final trait ends it.
u32 ui_guild_card_visible_traits(const float* widths, u32 count, float overflow_width, float width, u32 lines) noexcept;
bool ui_guild_card_set_hover(UiGuildCardScene*, float x, float y) noexcept;
bool ui_guild_card_set_pressed(UiGuildCardScene*, bool pressed) noexcept;
bool ui_guild_card_spinner_visible(const UiGuildCardScene&) noexcept;
// Media the host draws: 0 the icon, 1 the tag badge, 2 the banner picture,
// kUiGuildCardGameMedia + i the shown game icons, then the dimmed icon under
// "+N" and kUiGuildCardTraitEmojiMedia + i the traits' custom emoji. The banner
// covers its rectangle (cover = true, centred), masked to the card's rounded
// top corners (mask, mask_radius).
inline constexpr u32 kUiGuildCardBannerMedia = 2;
inline constexpr u32 kUiGuildCardGameMedia = 3;
inline constexpr u32 kUiGuildCardGamesMoreMedia = kUiGuildCardGameMedia + kUiGuildCardGames;
inline constexpr u32 kUiGuildCardTraitEmojiMedia = kUiGuildCardGamesMoreMedia + 1;
inline constexpr u32 kUiGuildCardMediaCount = kUiGuildCardTraitEmojiMedia + kUiGuildCardTraits;
struct UiGuildCardMedia {
    MediaAssetKey key;
    UiRect rectangle, clip;
    float radius, opacity;
    UiRect mask;
    float mask_radius;
    bool cover;
};
bool ui_guild_card_media(const UiGuildCardScene&, u32 index, UiGuildCardMedia* output) noexcept;
// The card and its banner glow; the banner picture (media 2) draws over
// them, then the foreground (icon ring and plate, server badge, chips,
// button) and the icon and badge media.
bool ui_guild_card_paint(const UiGuildCardScene&, const Theme&, PaintStream*) noexcept;
bool ui_guild_card_paint_foreground(const UiGuildCardScene&, const Theme&, PaintStream*) noexcept;
// What draws over the media: a busy game's flame marker on its icon's corner.
bool ui_guild_card_paint_overlay(const UiGuildCardScene&, const Theme&, PaintStream*) noexcept;
// chroma.js brighten(c, 1.75): the glow's bright centre.
Color ui_guild_card_banner_highlight(Color base) noexcept;
// A server profile banner (the card's, the Server Profile settings swatches): radial-gradient(105.43% 127.05% at
// 50.1% 127.05%, brighten(base, 1.75) 20.65%, base 85.16%) over `rectangle` with every corner rounded by radius, at
// base's alpha. gradient_height (0: the rectangle's height) places the gradient over the top of a taller rectangle.
bool ui_guild_profile_banner_paint(PaintStream* paint, const UiRect& rectangle, Color base, float radius,
                                   float gradient_height = 0) noexcept;
bool ui_guild_card_text_output(const UiGuildCardScene&, const Theme&, UiTextOutput*) noexcept;
} // namespace maicord
