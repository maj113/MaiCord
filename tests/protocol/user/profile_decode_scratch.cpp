#include "../../../src/app/chat/messages/history_decode.hpp"
#include "maicord/model/user/profile_connection.hpp"
#include "maicord/core/virtual_memory.hpp"

namespace maicord {
void test_profile_decode_scratch(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace app_detail;
    auto* ui = static_cast<AccountUiRuntime*>(
        pages_allocate(sizeof(AccountUiRuntime)));
    if (!ui) { check(false, "profile scratch fixture allocation"); return; }
    UserProfile retained{};
    const auto full = utf8_literal(
        R"({"user":{"id":"42","username":"user","global_name":"Name"},"user_profile":{"bio":"kept bio","banner":"a_banner","theme_colors":[1,2],"accent_color":0},"guild_member":{"roles":["10","11"]},"mutual_guilds":[{"id":"7","name":"guild"}]})");
    // accent_color 0 is black, a colour of its own.
    check(application_profile_decode_into(ui->profile_decode, full, 42, 7, retained) && retained.role_count == 2 &&
          retained.has_theme && retained.has_accent && retained.accent_color == 0 &&
          utf8_equal(retained.bio.view(), utf8_literal("kept bio")),
          "profile scratch commits complete decoded profile");
    // Simulate the intervening history/navigation phase reusing the real union.
    for (auto& value : ui->message_history_page_structurals) value = 0xa5a5a5a5u;
    check(retained.user_id == 42 && retained.role_count == 2 && retained.roles[1] == 11 &&
          utf8_equal(retained.banner.view(), utf8_literal("a_banner")),
          "profile fields remain owned after decoder scratch is overwritten");
    check(!application_profile_decode_into(ui->profile_decode, full, 43, 7, retained) && retained.user_id == 42 &&
          retained.role_count == 2 && utf8_equal(retained.bio.view(), utf8_literal("kept bio")),
          "identity mismatch preserves the displayed profile");
    const auto invalid = utf8_literal(
        R"({"user":{"id":"42","username":"partial"},"user_profile":{"bio":"do not commit","theme_colors":["invalid"]}})");
    check(!application_profile_decode_into(ui->profile_decode, invalid, 42, 7, retained) &&
          utf8_equal(retained.username.view(), utf8_literal("user")) && retained.role_count == 2 &&
          utf8_equal(retained.bio.view(), utf8_literal("kept bio")),
          "late decode failure cannot expose a partially decoded profile");
    const auto minimal = utf8_literal(R"({"user":{"id":"42","username":"next"}})");
    check(application_profile_decode_into(ui->profile_decode, minimal, 42, 0, retained) && retained.guild_id == 0 &&
          retained.role_count == 0 && retained.bio.bytes == 0 && !retained.has_theme && !retained.has_accent &&
          utf8_equal(retained.display_name.view(), utf8_literal("next")),
          "scratch reuse clears fields absent from the next valid profile");
    // Profile widgets (Userdoccers: Get User Profile `widgets`, Widget object): game lists with their games'
    // comments and tags, and an application widget; unknown widget types and unreadable games are left out.
    const auto widgets = utf8_literal(
        R"({"user":{"id":"42","username":"gamer"},"widgets":[)"
        R"({"id":"1","data":{"type":"favorite_games","games":[{"game_id":"501","comment":"best","tags":["expert",)"
        R"("love_it","unknown_tag"]},{"game_id":"x"},{"game_id":"502"}]}},)"
        R"({"id":"2","data":{"type":"clips_gallery","clips":[]}},)"
        R"({"id":"3","data":{"type":"played_games","games":[{"game_id":"502"},{"game_id":"503"}]}},)"
        R"({"id":"4","data":{"type":"application","application_id":"77"}}]})");
    u64 ids[8]{};
    const bool decoded = application_profile_decode_into(ui->profile_decode, widgets, 42, 0, retained);
    const auto& w = retained.widgets;
    const auto& g = retained.widget_games;
    check(decoded && retained.widget_count == 3 && retained.widget_game_count == 4 &&
              w[0].kind == ProfileWidgetKind::favorite_games && w[0].id == 1 && w[0].first_game == 0 &&
              w[0].game_count == 2 && w[1].kind == ProfileWidgetKind::played_games && w[1].first_game == 2 &&
              w[1].game_count == 2 && w[2].kind == ProfileWidgetKind::application && w[2].application_id == 77 &&
              !w[2].game_count && g[0].game_id == 501 && utf8_equal(g[0].comment.view(), utf8_literal("best")) &&
              g[0].tags == (1u << profile_widget_expert | 1u << profile_widget_love_it) && g[1].game_id == 502 &&
              !g[1].tags && profile_widget_game_ids(retained, ids, 8) == 3 && ids[0] == 501 && ids[1] == 502 &&
              ids[2] == 503 && profile_widget_game_ids(retained, ids, 2) == 2,
          "profile widgets: game lists, their games' comments and tags and application widgets are decoded");
    check(application_profile_decode_into(ui->profile_decode, minimal, 42, 0, retained) && !retained.widget_count &&
              !retained.widget_game_count,
          "profile widgets: a profile without widgets clears them");
    // Connected accounts (Userdoccers: Connected Accounts, partial connection): a type the official client does not
    // list is left out; metadata is read per type, as text or as a number.
    const auto accounts = utf8_literal(
        R"({"user":{"id":"42","username":"linked"},"connected_accounts":[)"
        R"({"type":"steam","id":"76561198000000001","name":"Zupko","verified":true,"metadata":{"created_at":"2020-01-19T12:00:00+00:00","game_count":"172","item_count_dota2":"0","item_count_tf2":"84"}},)"
        R"({"type":"github","id":"583231","name":"Zupk","verified":true,"metadata":{}},)"
        R"({"type":"spotify","id":"zupk user","name":"Đero Đerković","verified":true,"metadata":null},)"
        R"({"type":"reddit","id":"t2_x","name":"alien","verified":true,"metadata":{"gold":"0","mod":"1","total_karma":"20223","created_at":"2019-05-02T20:28:37"}},)"
        R"({"type":"twitter","id":"12","name":"jack","verified":true,"metadata":{"verified":"1","statuses_count":"1234","followers_count":5600000}},)"
        R"({"type":"ebay","id":"e","name":"seller","verified":false,"metadata":{"positive_feedback_percentage":"99.8","top_rated_seller":"1"}},)"
        R"({"type":"paypal","id":"p","name":"pal","verified":true,"metadata":{"verified":"1"}},)"
        R"({"type":"tiktok","id":"t","name":"tik","verified":true,"metadata":{"follower_count":"3","following_count":"4","likes_count":"1000","verified":"1"}},)"
        R"({"type":"soundcloud","id":"s","name":"sound","verified":false}]})");
    const bool connected = application_profile_decode_into(ui->profile_decode, accounts, 42, 0, retained);
    const auto& c = retained.connections;
    check(connected && retained.connection_count == 8 && c[0].count_mask == 7 && c[0].count[0] == 172 &&
              c[0].count[1] == 0 && c[0].count[2] == 84 &&
              utf8_equal(c[0].created_at.view(), utf8_literal("2020-01-19T12:00:00+00:00")) && c[0].verified &&
              utf8_equal(c[0].id.view(), utf8_literal("76561198000000001")) && !c[1].count_mask && !c[2].count_mask &&
              c[3].count_mask == 1 && c[3].count[0] == 20223 && c[3].marks == profile_connection_mod &&
              utf8_equal(c[3].created_at.view(), utf8_literal("2019-05-02T20:28:37Z")) && c[4].count_mask == 3 &&
              c[4].count[0] == 1234 && c[4].count[1] == 5600000 && c[4].marks == profile_connection_verified &&
              c[5].count_mask == 1 && c[5].count[0] == 998 && c[5].marks == profile_connection_top_rated &&
              !c[5].verified && c[6].marks == profile_connection_verified && c[7].count_mask == 7 &&
              c[7].count[2] == 1000 && c[7].marks == profile_connection_verified,
          "profile connections: metadata decodes per service; an unlisted type is left out");
    // The statistics as the official client words them: counts with separators, the item counts' plural, a
    // percentage's decimal, yes/no marks as their own lines, nothing for a service without any.
    const auto chips = [&](u32 index, const char* const* expected, u32 count) noexcept {
        ProfileConnectionChip got[kProfileConnectionChipMaximum]{};
        const u32 n = profile_connection_chips(c[index], got);
        bool same = n == count;
        for (u32 i = 0; same && i < n; ++i)
            same = utf8_equal(got[i].text.view(), {expected[i], c_string_length(expected[i])});
        return same;
    };
    const char* const steam[]{"172 Games", "0 items", "84 items"}, *const reddit[]{"20,223 Karma", "Mod"},
        *const twitter[]{"1,234 Posts", "5,600,000 Followers"},
        *const ebay[]{"99.8% Positive Ratings", "Top Rated Seller"}, *const paypal[]{"Verified Bank Account"},
        *const tiktok[]{"3 Followers", "4 Following", "1,000 Likes", "Verified"};
    ProfileConnection one{};
    one.type.bytes = 5;
    copy_bytes(one.type.data, "steam", 5);
    one.count_mask = 6;
    one.count[1] = 1;
    one.count[2] = 2;
    ProfileConnectionChip single[kProfileConnectionChipMaximum]{};
    const u32 singles = profile_connection_chips(one, single);
    ProfileConnectionChip steam_chips[kProfileConnectionChipMaximum]{};
    profile_connection_chips(c[0], steam_chips);
    check(chips(0, steam, 3) && chips(3, reddit, 2) && chips(4, twitter, 2) && chips(5, ebay, 2) &&
              chips(6, paypal, 1) && chips(7, tiktok, 4) && chips(1, nullptr, 0) && chips(2, nullptr, 0) &&
              singles == 2 && utf8_equal(single[0].text.view(), utf8_literal("1 item")) &&
              single[0].figure_bytes == 1 && utf8_equal(single[1].text.view(), utf8_literal("2 items")) &&
              steam_chips[0].figure_bytes == 3 && steam_chips[0].icon == ProfileConnectionChipIcon::none &&
              steam_chips[1].icon == ProfileConnectionChipIcon::dota_2 &&
              steam_chips[2].icon == ProfileConnectionChipIcon::team_fortress_2,
          "profile connections: statistics read as the official client words them");
    // Page URLs, by ID or by name, percent-encoded as a URI component; no page without what the rule needs.
    struct Page {
        const char *type, *id, *name, *url;
    };
    const Page pages[]{
        {"twitch", "1", "a b", "https://www.twitch.tv/a%20b"},
        {"youtube", "UC123", "n", "https://www.youtube.com/channel/UC123"},
        {"bluesky", "did:plc:abc", "n", "https://bsky.app/profile/did:plc:abc"},
        {"steam", "7656", "n", "https://steamcommunity.com/profiles/7656"},
        {"reddit", "t2", "alien", "https://www.reddit.com/u/alien"},
        {"twitter", "1", "jack", "https://x.com/jack"},
        {"spotify", "zupk user", "n", "https://open.spotify.com/user/zupk%20user"},
        {"github", "1", "Zupk", "https://github.com/Zupk"},
        {"roblox", "55", "n", "https://roblox.com/users/55/profile"},
        {"ebay", "e", "seller", "https://www.ebay.com/usr/seller"},
        {"tiktok", "t", "tik", "https://www.tiktok.com/@tik"},
        {"instagram", "i", "gram", "https://www.instagram.com/gram"},
        {"letterboxd", "l", "film", "https://letterboxd.com/film/"},
        {"mastodon", "https://mastodon.social/@u", "n", "https://mastodon.social/@u"},
        {"domain", "example.com", "example.com", "https://example.com/"},
        {"github", "1", "\xC4\x90", "https://github.com/%C4%90"},
        {"mastodon", "http://mastodon.social/@u", "n", nullptr},
        {"domain", "not a host", "n", nullptr},
        {"twitch", "1", "", nullptr},
        {"steam", "", "n", nullptr},
        {"xbox", "1", "n", nullptr},
        {"skype", "1", "n", nullptr},
    };
    bool urls = true;
    for (const auto& page : pages) {
        ProfileConnection connection{};
        connection.type.bytes = static_cast<u16>(c_string_length(page.type));
        copy_bytes(connection.type.data, page.type, connection.type.bytes);
        connection.id.bytes = static_cast<u16>(c_string_length(page.id));
        copy_bytes(connection.id.data, page.id, connection.id.bytes);
        connection.name.bytes = static_cast<u16>(c_string_length(page.name));
        copy_bytes(connection.name.data, page.name, connection.name.bytes);
        char url[256];
        const u32 bytes = profile_connection_url(connection, {url, sizeof(url)});
        urls = urls && (page.url ? utf8_equal({url, bytes}, {page.url, c_string_length(page.url)}) : bytes == 0);
        char tiny[8];
        urls = urls && !profile_connection_url(connection, {tiny, sizeof(tiny)});
    }
    ProfileConnection domain{};
    domain.type.bytes = 6;
    copy_bytes(domain.type.data, "domain", 6);
    check(urls && profile_connection_link_trusted(c[1]) && !profile_connection_link_trusted(domain) &&
              utf8_equal(profile_connection_name(profile_connection_kind(utf8_literal("twitter"))), utf8_literal("X")) &&
              profile_connection_kind(utf8_literal("soundcloud")) == ProfileConnectionKind::unknown,
          "profile connections: each service's page URL is built from its ID or name; only a domain asks first");
    pages_release(ui);
}
}
