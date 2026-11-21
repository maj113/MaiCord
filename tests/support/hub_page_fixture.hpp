#pragma once
// For the one unit of a test or probe that includes src/app/guild/hub.cpp (include this after it): what the
// application around the hub answers (the game cache, the account's status and settings, the profile, the sharing
// dialog and the content page host, which the page does not need to lay out), and the hub's data loaded as
// GET /guilds/{g}/space and its hydrate answer bring it. The unit defines what its executable has nowhere else:
// application_content_page, application_content_page_close, application_guild_banner_media and game_lookup_want.
#include "maicord/core/text_bank.hpp"
#include "maicord/core/time.hpp"
#include "maicord/media/core/asset.hpp"

namespace maicord::hub_page_fixture {
// A leaderboard entry: the member, the minutes played this week and the games (indexes into Hub::game_ids, the one
// played last first).
struct Leader {
    u64 user;
    u32 minutes;
    u8 games[3];
    u8 game_count;
    u16 game_total;
};
struct Hub {
    u64 guild;
    MediaAssetKey guild_icon;
    const char* banner_hash; // the hub's custom banner, or null for none
    const char* const* game_names;
    const u64* game_ids;
    const u32* game_players;
    u32 game_count;
    const Leader* leaders;
    u32 leader_count;
    const u64* players; // the faces under Popular Games
    u32 player_count, players_total, games_total;
};
Hub g_hub;
// The hub's games: an icon and a cover each, by an asset hash that names the game ("icon2", "cover2").
GameRecord g_games[kGuildSpaceGames];
} // namespace maicord::hub_page_fixture

namespace maicord::app_detail {
const GameRecord* game_lookup_find(u64 id) noexcept {
    for (const GameRecord& game : hub_page_fixture::g_games)
        if (game.game.id && game.game.id == id)
            return &game;
    return nullptr;
}
u32 game_lookup_revision() noexcept {
    return 1;
}
UiSettingsAvatarMedia application_guild_icon_media(u64 guild_id) noexcept {
    UiSettingsAvatarMedia media{};
    if (guild_id == hub_page_fixture::g_hub.guild)
        media.key = hub_page_fixture::g_hub.guild_icon;
    return media;
}
// The member list's avatar keys: a page shows the pictures the list already holds.
UiSettingsAvatarMedia application_listed_user_avatar_media(u64 user_id, const Utf8View& hash, u16) noexcept {
    UiSettingsAvatarMedia media{};
    media.key = media_asset_user_avatar(user_id, user_avatar_variant(hash), false);
    return media;
}
bool application_friends_notification_gates(u64, u32* gates) noexcept {
    *gates = 0;
    return true;
}
GatewayClientSelfStatusSnapshot application_self_status_snapshot() noexcept {
    GatewayClientSelfStatusSnapshot status{};
    status.status = status.confirmed_status = GatewayUserStatus::online;
    status.available = status.authoritative = true;
    return status;
}
bool application_self_status_request(GatewayUserStatus) noexcept {
    return false;
}
bool application_user_settings_guild_restricted(GuildPrivacyList, u64) noexcept {
    return false;
}
bool application_user_settings_set_guild_restricted(GuildPrivacyList, u64, bool) noexcept {
    return false;
}
bool application_guild_community_hub(GuildCommunityOperation, u64) noexcept {
    return true;
}
bool application_profile_open(u64, const UiRect&, ProfileAnchor) noexcept {
    return false;
}
bool application_profile_expand(u64, ProfileTab) noexcept {
    return false;
}
bool application_game_profile_open(u64) noexcept {
    return false;
}
// The Activity Sharing dialog never opens: the account counts.
bool application_activity_settings_project(SettingBinding, UiSettingsControl*) noexcept {
    return false;
}
bool application_activity_settings_activate(SettingBinding, bool, u32, bool*) noexcept {
    return false;
}
u64 settings_layer_next_wake(const SettingsLayer&) noexcept {
    return 0;
}
void settings_modal_finish_exit(const SettingsModal*) noexcept {}
void settings_modal_yield() noexcept {}
bool settings_dialog_begin(SettingsDialog&, UiPageKey, Utf8View) noexcept {
    return false;
}
bool settings_dialog_show(SettingsDialog&, UiPageKey, u64, bool, bool (*)() noexcept) noexcept {
    return false;
}
void settings_modal_suspend(SettingsModal&) noexcept {}
void settings_modal_close(SettingsModal&, bool) noexcept {}
bool settings_modal_present(bool) noexcept {
    return false;
}
bool settings_modal_update(SettingsModal&) noexcept {
    return false;
}
bool settings_modal_refresh(SettingsModal&) noexcept {
    return false;
}
// The unit lays the page out itself.
bool application_content_page_open(const ContentPageOwner*) noexcept {
    return true;
}
void application_content_page_invalidate() noexcept {}
float application_content_page_pane_offset() noexcept {
    return 0;
}
bool application_content_page_pane_scroll_to(double) noexcept {
    return false;
}
} // namespace maicord::app_detail

namespace maicord::hub_page_fixture {
using namespace app_detail;

// The hub of `hub` loaded, as the page has it once its answers came (g_app.account_ui must be set): a leaderboard
// (Most Game Time) and a text widget in the main column, Popular Games beside them. The week started three days ago.
// The account gave its personalization consent and shares its activity, so it counts. Null on a failed decode.
inline const ContentPageOwner* load(const Hub& hub) noexcept {
    g_hub = hub;
    if (!owner && !(owner = owner_allocate<Owner>()))
        return nullptr;
    for (u32 g = 0; g < kGuildSpaceGames; ++g) {
        GameRecord& record = g_games[g];
        record = {};
        if (g >= hub.game_count)
            continue;
        record.state = GameState::known;
        record.full = true;
        auto& game = record.game;
        game.id = hub.game_ids[g];
        const Utf8View name{hub.game_names[g], c_string_length(hub.game_names[g])};
        copy_bytes(game.name, name.data, name.bytes);
        game.name_bytes = static_cast<u8>(name.bytes);
        const char icon[]{'i', 'c', 'o', 'n', static_cast<char>('0' + g)};
        const char cover[]{'c', 'o', 'v', 'e', 'r', static_cast<char>('0' + g)};
        copy_bytes(game.icon, icon, sizeof(icon));
        game.icon_bytes = sizeof(icon);
        copy_bytes(game.cover, cover, sizeof(cover));
        game.cover_bytes = sizeof(cover);
    }
    auto& p = *owner;
    p.guild_id = hub.guild;
    p.account_scope = g_app.plugin_account_scope_serial;
    p.profile_id = g_app.account.status.profile_id;
    p.phase = Phase::loaded;
    p.hydrate_failed = false;
    consent = 1;
    static u32 scratch[8192];
    static char body[16384];
    TextWriter space{{body, sizeof(body)}};
    space.add(R"({"guild_id":")");
    space.number(hub.guild);
    space.add(R"(","header":{"custom_banner_hash":)");
    if (hub.banner_hash) {
        space.add(R"(")");
        space.add({hub.banner_hash, c_string_length(hub.banner_hash)});
        space.add(R"(")");
    } else {
        space.add("null");
    }
    space.add(R"(},"widgets":[)"
              R"({"id":"710000000000000001","type":"leaderboard","position":{"column":0,"order":0},)"
              R"("requires_hydration":true,"default_title":"Gaming Leaderboard","locked":false,)"
              R"("config":{"type":"leaderboard"}},)"
              R"({"id":"710000000000000002","type":"popular_games","position":{"column":1,"order":0},)"
              R"("requires_hydration":true,"default_title":"Popular Games","locked":false,)"
              R"("config":{"type":"popular_games"}},)"
              R"({"id":"710000000000000003","type":"image_text","position":{"column":1,"order":1},)"
              R"("requires_hydration":false,"default_title":"Text & Image","locked":false,)"
              R"("config":{"type":"image_text","title":"Game nights","text":"Fridays at 20:00 in the Lounge. )"
              R"(Bring a build of your plugin or a theme you are working on, then we play something together.",)"
              R"("image_hash":null}}]})");
    if (!space.ok || !guild_space_decode({body, space.used}, {scratch, 8192}, &p.space))
        return nullptr;
    const u64 now = clock_unix_milliseconds() / 1000;
    TextWriter hydrate{{body, sizeof(body)}};
    hydrate.add(R"({"widgets":[{"id":"710000000000000001","data":{"stat":1,"next_stat":2,"week_start_ts":)");
    hydrate.number(now - 3 * 86'400 - 3'600);
    hydrate.add(R"(,"entries":[)");
    for (u32 i = 0; i < hub.leader_count; ++i) {
        const Leader& l = hub.leaders[i];
        hydrate.add(i ? utf8_literal(",{\"user_id\":\"") : utf8_literal("{\"user_id\":\""));
        hydrate.number(l.user);
        hydrate.add(R"(","rank":)");
        hydrate.number(i + 1);
        hydrate.add(R"(,"value":)");
        hydrate.number(u64{l.minutes} * 60'000);
        hydrate.add(R"(,"time_played_seconds":)");
        hydrate.number(u64{l.minutes} * 60);
        hydrate.add(R"(,"application_ids":[)");
        for (u32 g = 0; g < l.game_count; ++g) {
            hydrate.add(g ? utf8_literal(",\"") : utf8_literal("\""));
            hydrate.number(hub.game_ids[l.games[g]]);
            hydrate.add(R"(")");
        }
        hydrate.add(R"(],"application_count":)");
        hydrate.number(l.game_total);
        hydrate.add("}");
    }
    hydrate.add(R"(]}},{"id":"710000000000000002","data":{"games":[)");
    for (u32 g = 0; g < hub.game_count; ++g) {
        hydrate.add(g ? utf8_literal(",{\"application_id\":\"") : utf8_literal("{\"application_id\":\""));
        hydrate.number(hub.game_ids[g]);
        hydrate.add(R"(","total_users":)");
        hydrate.number(hub.game_players[g]);
        hydrate.add("}");
    }
    hydrate.add(R"(],"user_ids":[)");
    for (u32 i = 0; i < hub.player_count; ++i) {
        hydrate.add(i ? utf8_literal(",\"") : utf8_literal("\""));
        hydrate.number(hub.players[i]);
        hydrate.add(R"(")");
    }
    hydrate.add(R"(],"total_users":)");
    hydrate.number(hub.players_total);
    hydrate.add(R"(,"total_games":)");
    hydrate.number(hub.games_total);
    hydrate.add("}}]}");
    if (!hydrate.ok || !guild_space_hydrate_decode({body, hydrate.used}, {scratch, 8192}, &p.space))
        return nullptr;
    return &kPage;
}
} // namespace maicord::hub_page_fixture
