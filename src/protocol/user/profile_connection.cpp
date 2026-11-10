#include "maicord/model/user/profile_connection.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/text_bank.hpp"

namespace maicord {
namespace {
using K = ProfileConnectionKind;
// By ProfileConnectionKind (from 1): the type as Discord names it, the display name and the logo's asset file; the
// texts are '\0'-separated lists, the logos 16 characters each.
constexpr char kTypes[] =
    "amazon-music\0battlenet\0bluesky\0bungie\0crunchyroll\0domain\0ebay\0epicgames\0facebook\0github\0instagram\0"
    "leagueoflegends\0letterboxd\0mastodon\0paypal\0playstation\0reddit\0riotgames\0roblox\0skype\0spotify\0steam\0"
    "tiktok\0twitch\0twitter\0xbox\0youtube";
constexpr char kNames[] =
    "Amazon Music\0Battle.net\0Bluesky\0Bungie.net\0Crunchyroll\0Domain\0eBay\0Epic Games\0Facebook\0GitHub\0Instagram\0"
    "League of Legends\0Letterboxd\0Mastodon\0PayPal\0PlayStation Network\0Reddit\0Riot Games\0Roblox\0Skype\0Spotify\0"
    "Steam\0TikTok\0Twitch\0X\0Xbox\0YouTube";
constexpr char kIcons[] =
    "cda6fb74e92e32f1" "0e1a419c00a471a2" "8a42ef6bc4dd4e0d" "9b762e8a42709cc4" "4e56bb1ecb9b9dc3" "8d54a5919eb645f9"
    "d0f7fa72a2ca6388" "e87d94bf0041b63e" "bda550439e2a1af0" "1cc92a6d139af44d" "ea0692691904fca9" "fc30b78cd5e1db04"
    "cb1a567ff6191fb5" "53fcd6d102d44c2b" "81a39a4672d62d60" "ae74d3de4958bf78" "afc3f4fa75178004" "fa9e14fc0fd3680f"
    "268c81933d752854" "4f2cb7753e5d5e61" "40518c6ced9fed93" "b68f2f4df4af9085" "b4002d11fb3b5c1c" "45900de231038a6d"
    "90d18081c2eb2ea3" "8cea2073e188eabe" "c569f6e10f1fa390";
constexpr u32 kPlatformCount = 27;
static_assert(kPlatformCount == static_cast<u32>(K::youtube) && sizeof(kIcons) == kPlatformCount * 16 + 1);
// Entry `index` of a '\0'-separated list.
Utf8View entry(const char* list, u32 index) noexcept {
    while (index--)
        list += c_string_length(list) + 1;
    return {list, c_string_length(list)};
}
// A service's account page, in kPageTexts' order (prefix, suffix): the prefix, the name (or the ID) as a URI
// component, then the suffix. Mastodon and domain connections carry their own URL and host instead.
struct Page {
    K kind;
    bool by_id, keep_colon; // keep_colon: a Bluesky DID keeps its ':'
};
constexpr Page kPages[] = {
    {K::twitch, false, false},   {K::youtube, true, false}, {K::bluesky, true, true},  {K::steam, true, false},
    {K::reddit, false, false},   {K::twitter, false, false}, {K::spotify, true, false}, {K::github, false, false},
    {K::roblox, true, false},    {K::ebay, false, false},   {K::tiktok, false, false}, {K::instagram, false, false},
    {K::letterboxd, false, false},
};
constexpr char kPageTexts[] =
    "https://www.twitch.tv/\0\0"
    "https://www.youtube.com/channel/\0\0"
    "https://bsky.app/profile/\0\0"
    "https://steamcommunity.com/profiles/\0\0"
    "https://www.reddit.com/u/\0\0"
    "https://x.com/\0\0"
    "https://open.spotify.com/user/\0\0"
    "https://github.com/\0\0"
    "https://roblox.com/users/\0/profile\0"
    "https://www.ebay.com/usr/\0\0"
    "https://www.tiktok.com/@\0\0"
    "https://www.instagram.com/\0\0"
    "https://letterboxd.com/\0/";
Utf8View text_of(const char* text) noexcept {
    return {text, c_string_length(text)};
}
bool is(const Utf8View& text, const Utf8View& literal) noexcept {
    return utf8_equal(text, literal);
}
// encodeURIComponent; `keep_colon` leaves ':' as it is.
void component(TextWriter& w, const Utf8View& value, bool keep_colon) noexcept {
    for (u32 i = 0; i < value.bytes; ++i) {
        const char c = value.data[i];
        if (ascii_is_alnum(c) || c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' ||
            c == '\'' || c == '(' || c == ')' || (keep_colon && c == ':')) {
            w.add(&c, 1);
            continue;
        }
        const char escaped[3]{'%', ascii_hex_digit_upper(static_cast<u8>(c) >> 4), ascii_hex_digit_upper(c)};
        w.add(escaped, 3);
    }
}
// A domain connection's ID is a host name; anything else would build another URL.
bool host_name(const Utf8View& id) noexcept {
    if (!id.bytes)
        return false;
    for (u32 i = 0; i < id.bytes; ++i) {
        const char c = id.data[i];
        if (!ascii_is_alnum(c) && c != '-' && c != '.')
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
    const char* at = kTypes;
    for (u32 i = 0; i < kPlatformCount; ++i) {
        const Utf8View candidate = text_of(at);
        if (utf8_equal(candidate, type))
            return static_cast<K>(i + 1);
        at += candidate.bytes + 1;
    }
    return K::unknown;
}
Utf8View profile_connection_name(ProfileConnectionKind kind) noexcept {
    const u32 index = static_cast<u32>(kind);
    return index && index <= kPlatformCount ? entry(kNames, index - 1) : Utf8View{};
}
Utf8View profile_connection_icon(ProfileConnectionKind kind) noexcept {
    const u32 index = static_cast<u32>(kind);
    return index && index <= kPlatformCount ? Utf8View{kIcons + (index - 1) * 16, 16} : Utf8View{};
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
    return profile_connection_kind(connection.type.view()) != K::domain;
}
u32 profile_connection_url(const ProfileConnection& connection, Slice<char> output) noexcept {
    const auto kind = profile_connection_kind(connection.type.view());
    const Utf8View id = connection.id.view();
    TextWriter w{output};
    if (kind == K::mastodon) {
        if (https_url(id))
            w.add(id);
    } else if (kind == K::domain) {
        if (host_name(id)) {
            w.add("https://");
            w.add(id);
            w.add("/");
        }
    } else {
        for (u32 i = 0; i < sizeof(kPages) / sizeof(kPages[0]); ++i) {
            const Page& page = kPages[i];
            if (page.kind != kind)
                continue;
            // The page is built from the name or from the ID, which must then be there.
            const Utf8View value = page.by_id ? id : connection.name.view();
            if (value.bytes) {
                w.add(entry(kPageTexts, 2 * i));
                component(w, value, page.keep_colon);
                w.add(entry(kPageTexts, 2 * i + 1));
            }
            break;
        }
    }
    return w.finish();
}
ProfileConnectionField profile_connection_field(ProfileConnectionKind kind, const Utf8View& name) noexcept {
    constexpr u8 none = 0xFF;
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
