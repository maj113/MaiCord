#pragma once

#include "maicord/model/user/profile.hpp"

namespace maicord {

// The connection types a profile lists, the official client's table of services (a type outside it is not shown).
enum class ProfileConnectionKind : u8 {
    unknown,
    amazon_music,
    battlenet,
    bluesky,
    bungie,
    crunchyroll,
    domain,
    ebay,
    epic_games,
    facebook,
    github,
    instagram,
    league_of_legends,
    letterboxd,
    mastodon,
    paypal,
    playstation,
    reddit,
    riot_games,
    roblox,
    skype,
    spotify,
    steam,
    tiktok,
    twitch,
    twitter,
    xbox,
    youtube
};
ProfileConnectionKind profile_connection_kind(const Utf8View& type) noexcept;
// "GitHub", "X", "PlayStation Network".
Utf8View profile_connection_name(ProfileConnectionKind kind) noexcept;
// The service's logo as a file of the web client's asset host: 16 hexadecimal digits (the name of
// https://discord.com/assets/<icon>.png), empty for a type without one.
Utf8View profile_connection_icon(ProfileConnectionKind kind) noexcept;
// The https URL of the account's page, built from its ID or name the way the official client does (the name or the
// ID percent-encoded as a URI component); 0 when the type has no page or the URL does not fit `output`.
u32 profile_connection_url(const ProfileConnection& connection, Slice<char> output) noexcept;
// Whether opening the page asks first: a domain is arbitrary, every other service is a known site.
bool profile_connection_link_trusted(const ProfileConnection& connection) noexcept;

// Metadata statistics of a connection, as the slots of ProfileConnection::count and the bits of its marks.
// Slots by type: reddit total_karma; steam game_count, item_count_dota2, item_count_tf2; twitter, bluesky and
// mastodon statuses_count, followers_count; ebay positive_feedback_percentage (in tenths of a percent); tiktok
// follower_count, following_count, likes_count; letterboxd watches, films_in_diary_this_year.
// Marks: reddit gold and mod, ebay top_rated_seller, paypal, tiktok and twitter verified.
enum ProfileConnectionMark : u8 {
    profile_connection_gold = 1,
    profile_connection_mod = 2,
    profile_connection_verified = 4,
    profile_connection_top_rated = 8
};
// Where a metadata field of a type lands: a count slot (`slot` below 3, `scale` 10 for a percentage kept in tenths,
// else 1) or a mark bit, or nowhere (`slot` 0xFF and no mark).
struct ProfileConnectionField {
    u8 slot, scale, mark;
};
ProfileConnectionField profile_connection_field(ProfileConnectionKind kind, const Utf8View& name) noexcept;
// A statistic as a line of text: the figure (the leading `figure_bytes`, drawn in bold) and its label, and the
// small picture of a game beside it (Steam's item counts).
enum class ProfileConnectionChipIcon : u8 { none, dota_2, team_fortress_2 };
struct ProfileConnectionChip {
    ProfileString<44> text;
    u8 figure_bytes;
    ProfileConnectionChipIcon icon;
};
inline constexpr u32 kProfileConnectionChipMaximum = 4;
// The chips the connection's metadata calls for, in the official client's order; returns how many it wrote.
u32 profile_connection_chips(const ProfileConnection& connection, ProfileConnectionChip* chips) noexcept;
// The asset file of a chip picture (see profile_connection_icon).
Utf8View profile_connection_chip_icon(ProfileConnectionChipIcon icon) noexcept;

} // namespace maicord
