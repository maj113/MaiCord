#pragma once

#include "../support/model_store_fixtures.hpp"
#include "maicord/media/core/asset_key.hpp"
#include "maicord/model/guild/member_store.hpp"
#include "maicord/app/plugins/catalog_index.hpp"

namespace maicord {
struct UiRect;
struct D3D11Renderer;
struct UiSettingsScene;
struct UiPageRegistry;
namespace app_detail {
struct ContentPageOwner;
}
namespace showcase_probe_detail {

// 2026-10-18 13:00 UTC; every message is earlier the same day.
constexpr u64 kNowSeconds = 1'792'328'400ull;
constexpr u64 kSnowflakeEpochMs = 1'420'070'400'000ull;
constexpr u64 kGuild = 100, kChannel = 311, kSelf = 1, kBot = 30, kList = 1;
constexpr u64 kRoleMaintainers = 901, kRoleDesigners = 902, kRoleContributors = 903;
constexpr u32 kWidth = 1440, kHeight = 900;
constexpr u32 kMessages = 16, kUsers = 16, kMembers = 16;
constexpr u32 kGuilds = 8, kChannels = 24, kRows = 32;

constexpr u64 snowflake(u32 minutes_before_now, u32 serial = 0) noexcept {
    return ((kNowSeconds * 1000 - minutes_before_now * 60'000ull - kSnowflakeEpochMs) << 22) + serial;
}

void paint_landscape(u8* out, u32 out_w, u32 out_h) noexcept;

void paint_gallery(u8* out, u32 out_w, u32 out_h) noexcept;

void paint_avatar(u8* out, u32 s, u32 a, u32 b, u32 variant) noexcept;

void paint_logo(u8* out, u32 s) noexcept;

void paint_guild_icon(u8* out, u32 s, u32 a, u32 b, u32 shape) noexcept;

void paint_banner(u8* out, u32 w, u32 h) noexcept;

void paint_game_art(u8* out, u32 s) noexcept;

void paint_record(u8* out, u32 s) noexcept;

void paint_party(u8* out, u32 w, u32 h, u32 variant) noexcept;

// A game's key art in one of kShowcaseGames palettes, at any aspect (covers are 3:4, icons square).
void paint_game_cover(u8* out, u32 w, u32 h, u32 game) noexcept;

// A pixel-art night scene for the gallery channel: 0 moonlit hills, 1 a city skyline.
void paint_pixel_scene(u8* out, u32 w, u32 h, u32 scene) noexcept;

// --- The content pages (showcase_probe_hub.cpp, showcase_probe_channels.cpp) -------------------------------------

// The games of the Server Hub, by their application ids.
constexpr u32 kShowcaseGames = 5;
constexpr u64 kShowcaseGameIds[kShowcaseGames]{720000000000000001ull, 720000000000000002ull, 720000000000000003ull,
                                               720000000000000004ull, 720000000000000005ull};
// A leaderboard entry: the member, minutes played this week and the games (indexes, the last played first).
struct ShowcaseLeader {
    u64 user;
    u32 minutes;
    u8 games[3];
    u8 game_count;
    u16 game_total;
};
// The probe's world as the pages read it: the account's stores, the server, the people with pictures and the hub's
// numbers.
struct ShowcaseWorld {
    const ChatStateStore* state;
    const UserStore* users;
    const GuildMemberStore* members;
    u64 guild, list;
    MediaAssetKey guild_icon;
    const u64* people;
    u32 people_count;
    u32 dpi, width, height;
    u32 member_count, online_count, boosts;
    const ShowcaseLeader* leaders;
    u32 leader_count;
    const char* const* game_names;
    const u32* game_players;
    u32 players_total, games_total;
};
// What a page's picture slot shows.
enum class ShowcaseArt : u8 { none, avatar, guild_icon, banner, game_icon, game_cover };
struct ShowcasePicture {
    ShowcaseArt art;
    u64 subject; // the user, the guild or the game's index
};
// The application's account UI over the world's stores (the pages read g_app.account_ui).
bool showcase_account_ui(const ShowcaseWorld& world) noexcept;
// The Server Hub page with its data loaded (hub.cpp's owner), or null.
const app_detail::ContentPageOwner* showcase_hub_page(const ShowcaseWorld& world) noexcept;
ShowcasePicture showcase_hub_picture(u32 slot) noexcept;
// Channels & Roles on its Browse Channels tab (channels_roles.cpp's page), or null.
const app_detail::ContentPageOwner* showcase_channels_page(const ShowcaseWorld& world) noexcept;

// --- The store pages (showcase_probe_store.cpp) -------------------------------------------------------------------

// Settings shows the Plugins, Themes or Effects page with the catalog's listings and their pictures, bound to the
// renderer as the store host binds it. False when the page cannot be laid out or a picture is missing.
bool showcase_store_page(D3D11Renderer& r, UiSettingsScene* settings, UiPageRegistry* registry, CatalogKind kind,
                         const UiRect& viewport) noexcept;
void showcase_store_close(D3D11Renderer& r) noexcept;

} // namespace showcase_probe_detail

using namespace showcase_probe_detail;
} // namespace maicord
