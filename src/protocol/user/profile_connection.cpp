#include "maicord/model/user/profile_connection.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
// By ProfileConnectionKind (from 1): the type as Discord names it, the display name and the logo's asset file.
struct Platform {
    Utf8View type, name, icon;
};
constexpr Platform kPlatforms[] = {
    {utf8_literal("amazon-music"), utf8_literal("Amazon Music"), utf8_literal("cda6fb74e92e32f1")},
    {utf8_literal("battlenet"), utf8_literal("Battle.net"), utf8_literal("0e1a419c00a471a2")},
    {utf8_literal("bluesky"), utf8_literal("Bluesky"), utf8_literal("8a42ef6bc4dd4e0d")},
    {utf8_literal("bungie"), utf8_literal("Bungie.net"), utf8_literal("9b762e8a42709cc4")},
    {utf8_literal("crunchyroll"), utf8_literal("Crunchyroll"), utf8_literal("4e56bb1ecb9b9dc3")},
    {utf8_literal("domain"), utf8_literal("Domain"), utf8_literal("8d54a5919eb645f9")},
    {utf8_literal("ebay"), utf8_literal("eBay"), utf8_literal("d0f7fa72a2ca6388")},
    {utf8_literal("epicgames"), utf8_literal("Epic Games"), utf8_literal("e87d94bf0041b63e")},
    {utf8_literal("facebook"), utf8_literal("Facebook"), utf8_literal("bda550439e2a1af0")},
    {utf8_literal("github"), utf8_literal("GitHub"), utf8_literal("1cc92a6d139af44d")},
    {utf8_literal("instagram"), utf8_literal("Instagram"), utf8_literal("ea0692691904fca9")},
    {utf8_literal("leagueoflegends"), utf8_literal("League of Legends"), utf8_literal("fc30b78cd5e1db04")},
    {utf8_literal("letterboxd"), utf8_literal("Letterboxd"), utf8_literal("cb1a567ff6191fb5")},
    {utf8_literal("mastodon"), utf8_literal("Mastodon"), utf8_literal("53fcd6d102d44c2b")},
    {utf8_literal("paypal"), utf8_literal("PayPal"), utf8_literal("81a39a4672d62d60")},
    {utf8_literal("playstation"), utf8_literal("PlayStation Network"), utf8_literal("ae74d3de4958bf78")},
    {utf8_literal("reddit"), utf8_literal("Reddit"), utf8_literal("afc3f4fa75178004")},
    {utf8_literal("riotgames"), utf8_literal("Riot Games"), utf8_literal("fa9e14fc0fd3680f")},
    {utf8_literal("roblox"), utf8_literal("Roblox"), utf8_literal("268c81933d752854")},
    {utf8_literal("skype"), utf8_literal("Skype"), utf8_literal("4f2cb7753e5d5e61")},
    {utf8_literal("spotify"), utf8_literal("Spotify"), utf8_literal("40518c6ced9fed93")},
    {utf8_literal("steam"), utf8_literal("Steam"), utf8_literal("b68f2f4df4af9085")},
    {utf8_literal("tiktok"), utf8_literal("TikTok"), utf8_literal("b4002d11fb3b5c1c")},
    {utf8_literal("twitch"), utf8_literal("Twitch"), utf8_literal("45900de231038a6d")},
    {utf8_literal("twitter"), utf8_literal("X"), utf8_literal("90d18081c2eb2ea3")},
    {utf8_literal("xbox"), utf8_literal("Xbox"), utf8_literal("8cea2073e188eabe")},
    {utf8_literal("youtube"), utf8_literal("YouTube"), utf8_literal("c569f6e10f1fa390")},
};
static_assert(sizeof(kPlatforms) / sizeof(kPlatforms[0]) == static_cast<u32>(ProfileConnectionKind::youtube));
const Platform* platform(ProfileConnectionKind kind) noexcept {
    const u32 index = static_cast<u32>(kind);
    return index && index <= sizeof(kPlatforms) / sizeof(kPlatforms[0]) ? &kPlatforms[index - 1] : nullptr;
}
bool is(const Utf8View& text, const Utf8View& literal) noexcept {
    return utf8_equal(text, literal);
}
bool unreserved(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')';
}
// Appends to a fixed buffer; a text that does not fit leaves `ok` false.
struct Writer {
    char* data;
    u32 capacity, used;
    bool ok;
    void text(const Utf8View& value) noexcept {
        if (!ok || value.bytes > capacity - used) {
            ok = false;
            return;
        }
        copy_bytes(data + used, value.data, value.bytes);
        used += value.bytes;
    }
    // encodeURIComponent; `keep_colon` leaves ':' as it is (a Bluesky DID).
    void component(const Utf8View& value, bool keep_colon = false) noexcept {
        static constexpr char hex[] = "0123456789ABCDEF";
        for (u32 i = 0; ok && i < value.bytes; ++i) {
            const char c = value.data[i];
            if (unreserved(c) || (keep_colon && c == ':')) {
                text({&c, 1});
                continue;
            }
            const char escaped[3]{'%', hex[static_cast<u8>(c) >> 4], hex[static_cast<u8>(c) & 15]};
            text({escaped, 3});
        }
    }
};
// A domain connection's ID is a host name; anything else would build another URL.
bool host_name(const Utf8View& id) noexcept {
    if (!id.bytes)
        return false;
    for (u32 i = 0; i < id.bytes; ++i) {
        const char c = id.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.'))
            return false;
    }
    return true;
}
// A Mastodon connection's ID is the account's URL: an https URL of printable ASCII without spaces.
bool https_url(const Utf8View& id) noexcept {
    constexpr auto scheme = utf8_literal("https://");
    if (id.bytes <= scheme.bytes || !utf8_equal({id.data, scheme.bytes}, scheme))
        return false;
    for (u32 i = 0; i < id.bytes; ++i)
        if (static_cast<u8>(id.data[i]) <= ' ' || static_cast<u8>(id.data[i]) >= 0x7F)
            return false;
    return true;
}
} // namespace
ProfileConnectionKind profile_connection_kind(const Utf8View& type) noexcept {
    for (u32 i = 0; i < sizeof(kPlatforms) / sizeof(kPlatforms[0]); ++i)
        if (utf8_equal(kPlatforms[i].type, type))
            return static_cast<ProfileConnectionKind>(i + 1);
    return ProfileConnectionKind::unknown;
}
Utf8View profile_connection_name(ProfileConnectionKind kind) noexcept {
    const auto* p = platform(kind);
    return p ? p->name : Utf8View{};
}
Utf8View profile_connection_icon(ProfileConnectionKind kind) noexcept {
    const auto* p = platform(kind);
    return p ? p->icon : Utf8View{};
}
Utf8View profile_connection_chip_icon(ProfileConnectionChipIcon icon) noexcept {
    switch (icon) {
    case ProfileConnectionChipIcon::dota_2:
        return utf8_literal("44e2229fd99bc14e");
    case ProfileConnectionChipIcon::team_fortress_2:
        return utf8_literal("9eb53b4fa3cc12d5");
    default:
        return {};
    }
}
bool profile_connection_link_trusted(const ProfileConnection& connection) noexcept {
    return profile_connection_kind(connection.type.view()) != ProfileConnectionKind::domain;
}
u32 profile_connection_url(const ProfileConnection& connection, Slice<char> output) noexcept {
    const auto kind = profile_connection_kind(connection.type.view());
    const Utf8View name = connection.name.view(), id = connection.id.view();
    Writer w{output.data, output.count, 0, true};
    // The page is built from the name or from the ID, which must then be there.
    const auto by_name = [&](const Utf8View& prefix, const Utf8View& suffix = {}) noexcept {
        if (name.bytes) {
            w.text(prefix);
            w.component(name);
            w.text(suffix);
        }
    };
    const auto by_id = [&](const Utf8View& prefix, const Utf8View& suffix = {}, bool keep_colon = false) noexcept {
        if (id.bytes) {
            w.text(prefix);
            w.component(id, keep_colon);
            w.text(suffix);
        }
    };
    switch (kind) {
    case ProfileConnectionKind::twitch:
        by_name(utf8_literal("https://www.twitch.tv/"));
        break;
    case ProfileConnectionKind::youtube:
        by_id(utf8_literal("https://www.youtube.com/channel/"));
        break;
    case ProfileConnectionKind::bluesky:
        by_id(utf8_literal("https://bsky.app/profile/"), {}, true);
        break;
    case ProfileConnectionKind::steam:
        by_id(utf8_literal("https://steamcommunity.com/profiles/"));
        break;
    case ProfileConnectionKind::reddit:
        by_name(utf8_literal("https://www.reddit.com/u/"));
        break;
    case ProfileConnectionKind::twitter:
        by_name(utf8_literal("https://x.com/"));
        break;
    case ProfileConnectionKind::spotify:
        by_id(utf8_literal("https://open.spotify.com/user/"));
        break;
    case ProfileConnectionKind::github:
        by_name(utf8_literal("https://github.com/"));
        break;
    case ProfileConnectionKind::roblox:
        by_id(utf8_literal("https://roblox.com/users/"), utf8_literal("/profile"));
        break;
    case ProfileConnectionKind::ebay:
        by_name(utf8_literal("https://www.ebay.com/usr/"));
        break;
    case ProfileConnectionKind::tiktok:
        by_name(utf8_literal("https://www.tiktok.com/@"));
        break;
    case ProfileConnectionKind::instagram:
        by_name(utf8_literal("https://www.instagram.com/"));
        break;
    case ProfileConnectionKind::letterboxd:
        by_name(utf8_literal("https://letterboxd.com/"), utf8_literal("/"));
        break;
    case ProfileConnectionKind::mastodon:
        if (https_url(id))
            w.text(id);
        break;
    case ProfileConnectionKind::domain:
        if (host_name(id)) {
            w.text(utf8_literal("https://"));
            w.text(id);
            w.text(utf8_literal("/"));
        }
        break;
    default:
        break;
    }
    return w.ok ? w.used : 0;
}
ProfileConnectionField profile_connection_field(ProfileConnectionKind kind, const Utf8View& name) noexcept {
    constexpr u8 none = 0xFF;
    using K = ProfileConnectionKind;
    const auto slot = [&](u8 index, u8 scale = 1) noexcept { return ProfileConnectionField{index, scale, 0}; };
    const auto mark = [&](u8 bit) noexcept { return ProfileConnectionField{none, 1, bit}; };
    switch (kind) {
    case K::reddit:
        return is(name, utf8_literal("total_karma")) ? slot(0)
               : is(name, utf8_literal("gold"))      ? mark(profile_connection_gold)
               : is(name, utf8_literal("mod"))       ? mark(profile_connection_mod)
                                                     : slot(none);
    case K::steam:
        return is(name, utf8_literal("game_count"))         ? slot(0)
               : is(name, utf8_literal("item_count_dota2")) ? slot(1)
               : is(name, utf8_literal("item_count_tf2"))   ? slot(2)
                                                            : slot(none);
    case K::twitter:
    case K::bluesky:
    case K::mastodon:
        return is(name, utf8_literal("statuses_count"))                ? slot(0)
               : is(name, utf8_literal("followers_count"))             ? slot(1)
               : kind == K::twitter && is(name, utf8_literal("verified")) ? mark(profile_connection_verified)
                                                                          : slot(none);
    case K::ebay:
        return is(name, utf8_literal("positive_feedback_percentage")) ? slot(0, 10)
               : is(name, utf8_literal("top_rated_seller"))           ? mark(profile_connection_top_rated)
                                                                      : slot(none);
    case K::paypal:
        return is(name, utf8_literal("verified")) ? mark(profile_connection_verified) : slot(none);
    case K::tiktok:
        return is(name, utf8_literal("follower_count"))    ? slot(0)
               : is(name, utf8_literal("following_count")) ? slot(1)
               : is(name, utf8_literal("likes_count"))     ? slot(2)
               : is(name, utf8_literal("verified"))        ? mark(profile_connection_verified)
                                                           : slot(none);
    case K::letterboxd:
        return is(name, utf8_literal("watches"))                     ? slot(0)
               : is(name, utf8_literal("films_in_diary_this_year")) ? slot(1)
                                                                    : slot(none);
    default:
        return slot(none);
    }
}
namespace {
struct ChipWriter {
    ProfileConnectionChip* chips;
    u32 count;
    // "<figure><label>"; the figure is a grouped count (or a percentage in tenths), bold in the line.
    void add(const ProfileConnection& c, u32 slot, bool percent, const Utf8View& label,
             ProfileConnectionChipIcon icon = ProfileConnectionChipIcon::none) noexcept {
        if (!(c.count_mask & (1u << slot)))
            return;
        auto& chip = chips[count++];
        reset_value(&chip);
        char* at = chip.text.data;
        const u32 value = c.count[slot];
        if (percent) {
            at += decimal_write_grouped(at, value / 10);
            if (value % 10) {
                *at++ = '.';
                *at++ = static_cast<char>('0' + value % 10);
            }
            *at++ = '%';
        } else
            at += decimal_write_grouped(at, value);
        chip.figure_bytes = static_cast<u8>(at - chip.text.data);
        chip.text.bytes = static_cast<u16>(chip.figure_bytes);
        text(&chip, label);
        chip.icon = icon;
    }
    void text(ProfileConnectionChip* chip, const Utf8View& label) noexcept {
        copy_bytes(chip->text.data + chip->text.bytes, label.data, label.bytes);
        chip->text.bytes = static_cast<u16>(chip->text.bytes + label.bytes);
    }
    void mark(const ProfileConnection& c, u8 bit, const Utf8View& label) noexcept {
        if (!(c.marks & bit))
            return;
        auto& chip = chips[count++];
        reset_value(&chip);
        copy_bytes(chip.text.data, label.data, label.bytes);
        chip.text.bytes = static_cast<u16>(label.bytes);
    }
};
} // namespace
u32 profile_connection_chips(const ProfileConnection& c, ProfileConnectionChip* chips) noexcept {
    using K = ProfileConnectionKind;
    using I = ProfileConnectionChipIcon;
    ChipWriter w{chips, 0};
    switch (profile_connection_kind(c.type.view())) {
    case K::reddit:
        w.add(c, 0, false, utf8_literal(" Karma"));
        w.mark(c, profile_connection_gold, utf8_literal("Gold"));
        w.mark(c, profile_connection_mod, utf8_literal("Mod"));
        break;
    case K::steam: {
        // "1 item", "2 items": the item counts are the only plural.
        const auto items = [&](u32 slot, I icon) noexcept {
            w.add(c, slot, false, c.count[slot] == 1 ? utf8_literal(" item") : utf8_literal(" items"), icon);
        };
        w.add(c, 0, false, utf8_literal(" Games"));
        items(1, I::dota_2);
        items(2, I::team_fortress_2);
        break;
    }
    case K::twitter:
    case K::bluesky:
    case K::mastodon:
        w.add(c, 0, false, utf8_literal(" Posts"));
        w.add(c, 1, false, utf8_literal(" Followers"));
        break;
    case K::ebay:
        // Zero says nothing: the official client lists the rating only above it.
        if (c.count[0] > 0)
            w.add(c, 0, true, utf8_literal(" Positive Ratings"));
        w.mark(c, profile_connection_top_rated, utf8_literal("Top Rated Seller"));
        break;
    case K::paypal:
        w.mark(c, profile_connection_verified, utf8_literal("Verified Bank Account"));
        break;
    case K::tiktok:
        w.add(c, 0, false, utf8_literal(" Followers"));
        w.add(c, 1, false, utf8_literal(" Following"));
        w.add(c, 2, false, utf8_literal(" Likes"));
        w.mark(c, profile_connection_verified, utf8_literal("Verified"));
        break;
    case K::letterboxd:
        w.add(c, 0, false, utf8_literal(" Films Watched"));
        w.add(c, 1, false, utf8_literal(" Films Logged This Year"));
        break;
    default:
        break;
    }
    return w.count;
}
} // namespace maicord
