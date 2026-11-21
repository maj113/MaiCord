// The showcase's Server Hub page: hub.cpp's own page builder over the probe's world. The account's stores are the
// probe's; what the rest of the application would answer comes from the hub page fixture, which also loads the hub's
// data (tests/support/hub_page_fixture.hpp, shared with maicord_hub_page).
#include "../../src/app/guild/hub.cpp"
#include "../support/hub_page_fixture.hpp"
#include "showcase_probe_internal.hpp"
#include "maicord/core/virtual_memory.hpp"

// --- The page over the probe's world -----------------------------------------------------------------------------

namespace maicord::showcase_probe_detail {
using namespace app_detail;
namespace {
const ShowcaseWorld* g_world;
AccountUiRuntime* g_ui;
} // namespace

bool showcase_account_ui(const ShowcaseWorld& world) noexcept {
    g_world = &world;
    if (!g_ui) {
        g_ui = static_cast<AccountUiRuntime*>(pages_allocate(sizeof(AccountUiRuntime)));
        if (!g_ui)
            return false;
        if (!guild_detail_store_bind(&g_ui->guild_details, {g_ui->guild_detail_records, kChatStateGuildCapacity},
                                     {g_ui->guild_detail_text, kGuildDetailTextBytes}))
            return false;
    }
    // The probe's stores, read as the account's own: the copies share the probe's records.
    g_ui->chat_state = *world.state;
    g_ui->user_store = *world.users;
    g_ui->guild_member_store = *world.members;
    GuildDetailInput details{};
    details.guild_id = world.guild;
    details.member_count = world.member_count;
    details.premium_subscription_count = world.boosts;
    details.known = guild_detail_boost_count | guild_detail_member_count;
    if (!guild_detail_store_apply(&g_ui->guild_details, details) ||
        !guild_member_store_observe_list(&g_ui->guild_member_store, world.guild, world.list, world.member_count,
                                         world.online_count, true, true, false, 0, world.member_count))
        return false;
    g_app.account_ui = g_ui;
    g_app.account.status.running = true;
    g_app.login.visible = false;
    g_app.renderer.dpi = world.dpi;
    g_app.renderer.width = world.width;
    g_app.renderer.height = world.height;
    return true;
}

// The hub as the page has it once its answers came, with the probe's banner, games, players and leaders.
const ContentPageOwner* showcase_hub_page(const ShowcaseWorld& world) noexcept {
    if (!showcase_account_ui(world))
        return nullptr;
    static hub_page_fixture::Leader leaders[kGuildSpaceLeaders];
    const u32 count = world.leader_count < kGuildSpaceLeaders ? world.leader_count : kGuildSpaceLeaders;
    for (u32 i = 0; i < count; ++i) {
        const ShowcaseLeader& l = world.leaders[i];
        leaders[i] = {l.user, l.minutes, {l.games[0], l.games[1], l.games[2]}, l.game_count, l.game_total};
    }
    u64 players[4]{};
    for (u32 i = 0; i < 4 && i < count; ++i)
        players[i] = world.leaders[i].user;
    return hub_page_fixture::load({world.guild, world.guild_icon, "5a1b0c0ffee0ba5e0d1ab0b5c0ffee00",
                                   world.game_names, kShowcaseGameIds, world.game_players, kShowcaseGames, leaders,
                                   count, players, count < 4 ? count : 4, world.players_total, world.games_total});
}

ShowcasePicture showcase_hub_picture(u32 slot) noexcept {
    const auto* m = media(slot);
    if (!m)
        return {};
    if (g_world && m->key.id == g_world->guild_icon.id && m->key.variant == g_world->guild_icon.variant)
        return {ShowcaseArt::guild_icon, g_world->guild};
    const Utf8View url = m->url;
    // "/cover2.png", "/icon2.png": the game is the digit after the word, at the start of the path's last part.
    const auto game = [&](const char* word) noexcept -> u32 {
        const u32 n = c_string_length(word);
        for (u32 i = 1; i + n < url.bytes; ++i)
            if (url.data[i - 1] == '/' && bytes_equal(url.data + i, word, n) && url.data[i + n] >= '0' &&
                url.data[i + n] <= '9')
                return static_cast<u32>(url.data[i + n] - '0');
        return ~0u;
    };
    const auto has = [&](const char* word) noexcept {
        const u32 n = c_string_length(word);
        for (u32 i = 0; i + n <= url.bytes; ++i)
            if (bytes_equal(url.data + i, word, n))
                return true;
        return false;
    };
    if (const u32 g = game("cover"); g < kShowcaseGames)
        return {ShowcaseArt::game_cover, g};
    if (const u32 g = game("icon"); g < kShowcaseGames)
        return {ShowcaseArt::game_icon, g};
    if (has("/guild-space/"))
        return {ShowcaseArt::banner, 0};
    // The rest are the members' avatars, under the keys the member list uploaded them with.
    for (u32 i = 0; g_world && i < g_world->people_count; ++i) {
        const UserRecord* user = user_store_find(*g_world->users, g_world->people[i]);
        const Utf8View hash = user ? user_store_avatar_hash(*user) : Utf8View{};
        const MediaAssetKey key = media_asset_user_avatar(g_world->people[i], user_avatar_variant(hash), false);
        if (m->key.id == key.id && m->key.variant == key.variant)
            return {ShowcaseArt::avatar, g_world->people[i]};
    }
    log_u64(LogLevel::error, "showcase", "hub_picture_unmatched_slot", slot);
    return {};
}
} // namespace maicord::showcase_probe_detail
