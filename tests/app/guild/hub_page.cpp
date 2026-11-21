// The Server Hub page's leaderboard through hub.cpp's own page builder and the store scene's layout: each leader's
// row fills the list's row pitch (40 DIPs, as the official client's rows), so a short board shows every row whole
// and a long one shows exactly eight in its 320 DIP list. No account, network or renderer is involved.
#include "../../../src/app/guild/hub.cpp"
#include "../../support/hub_page_fixture.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/virtual_memory.hpp"
namespace maicord {
// The banner's glow reads the icon's colour from the renderer, which this test does not link.
bool renderer_media_dominant_color(D3D11Renderer*, MediaAssetKey, u32*) noexcept {
    return false;
}
} // namespace maicord

namespace maicord::app_detail {
// What the page asks of the rest of the application and this executable has nowhere else.
const ContentPageOwner* application_content_page() noexcept {
    return nullptr;
}
void application_content_page_close() noexcept {}
u32 application_guild_banner_media(GuildBannerMedia*, u32) noexcept {
    return 0;
}
void game_lookup_want(u64) noexcept {}
} // namespace maicord::app_detail

namespace maicord::hub_page_test {
using namespace app_detail;
u32 failures;
void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}

constexpr u64 kGuild = 100, kSelf = 1, kFirstLeader = 200;
constexpr u32 kLeaders = 20;
constexpr u64 kGameIds[]{720000000000000001ull, 720000000000000002ull, 720000000000000003ull};
constexpr const char* kGameNames[]{"Lumen Drift", "Neon Circuit", "Ringfall"};
constexpr u32 kGamePlayers[]{7, 5, 3};
constexpr float kRowPitch = 40, kListHeight = 320;

UserRecord g_user_records[kLeaders + 1];
u32 g_user_order[kLeaders + 1];
u8 g_user_names[2048];
GuildMemberRecord g_member_records[kLeaders + 1];
u32 g_member_order[kLeaders + 1];
UiStoreScene g_scene;

// The account's stores: the leaders are known users, and members of the server without roles.
bool account() noexcept {
    auto* ui = static_cast<AccountUiRuntime*>(pages_allocate(sizeof(AccountUiRuntime)));
    if (!ui || !guild_detail_store_bind(&ui->guild_details, {ui->guild_detail_records, kChatStateGuildCapacity},
                                        {ui->guild_detail_text, kGuildDetailTextBytes}) ||
        !user_store_initialize(&ui->user_store, {g_user_records, kLeaders + 1}, {g_user_order, kLeaders + 1},
                               {g_user_names, sizeof(g_user_names)}) ||
        !guild_member_store_initialize(&ui->guild_member_store, {g_member_records, kLeaders + 1},
                                       {g_member_order, kLeaders + 1}))
        return false;
    ui->chat_state.self_user_id = kSelf;
    for (u32 i = 0; i < kLeaders; ++i) {
        char name[16] = "Player ";
        name[7] = static_cast<char>('A' + i);
        if (!user_store_upsert(&ui->user_store, kFirstLeader + i, {name, 8}, {name, 8}).ok())
            return false;
    }
    g_app.account_ui = ui;
    g_app.account.status.running = true;
    g_app.renderer.dpi = 96;
    g_app.renderer.width = 1440;
    g_app.renderer.height = 900;
    return ui_store_scene_open(&g_scene);
}

// Lays the page out for a board of `count` leaders and checks the leaderboard's list. `label` names the board.
void board(u32 count, const char* label) noexcept {
    hub_page_fixture::Leader leaders[kLeaders]{};
    for (u32 i = 0; i < count; ++i)
        leaders[i] = {kFirstLeader + i, 1800 - i * 60, {static_cast<u8>(i % 3)}, 1, 1};
    const u64 players[]{kFirstLeader, kFirstLeader + 1, kFirstLeader + 2};
    const ContentPageOwner* page = hub_page_fixture::load(
        {kGuild, {}, nullptr, kGameNames, kGameIds, kGamePlayers, 3, leaders, count, players, 3, 11, 14});
    check(page != nullptr, label);
    if (!page)
        return;
    // The content page host's column at its widest.
    constexpr float kWidth = 1072;
    const UiRect panel{0, 0, kWidth + 32, 2000}, column{16, 16, 16 + kWidth, 2000};
    ui_store_scene_begin(&g_scene);
    UiStoreBuilder builder(&g_scene);
    page->build(builder, kWidth);
    const bool laid_out = builder.ok() && ui_store_scene_commit(&g_scene, panel, column, panel);
    check(laid_out, "hub_page_laid_out");
    if (!laid_out || g_scene.pane == kUiStoreNone) {
        check(false, "hub_page_leaderboard_pane");
        return;
    }
    const UiStoreNode& pane = g_scene.nodes[g_scene.pane];
    u32 rows = 0, whole = 0;
    float total = 0;
    bool pitch = true;
    for (u16 i = pane.first; i != kUiStoreNone; i = g_scene.nodes[i].next) {
        const UiStoreNode& row = g_scene.nodes[i];
        pitch = pitch && row.h == kRowPitch;
        total += row.h;
        whole += row.y >= pane.y && row.y + row.h <= pane.y + pane.h;
        ++rows;
    }
    test_write_line(label);
    test_write_metric("  rows=", rows);
    test_write_metric("  pane_height=", static_cast<u64>(pane.h));
    test_write_metric("  rows_height=", static_cast<u64>(total));
    test_write_metric("  rows_shown_whole=", whole);
    check(rows == count, "hub_page_one_row_per_leader");
    check(pitch, count <= 8 ? "hub_page_short_board_row_pitch" : "hub_page_long_board_row_pitch");
    if (count * kRowPitch <= kListHeight) {
        // Nothing to scroll: the list is exactly as tall as its rows, and none is cut.
        check(pane.h == total, "hub_page_short_board_unclipped");
        check(whole == count, "hub_page_short_board_rows_whole");
    } else {
        // The 320 DIP list shows eight whole rows and nothing of the ninth.
        check(pane.h == kListHeight, "hub_page_long_board_list_height");
        check(whole == 8, "hub_page_long_board_eight_rows");
    }
}

void run() noexcept {
    check(account(), "hub_page_account");
    if (failures)
        return;
    board(3, "hub_page_board_3");
    board(kLeaders, "hub_page_board_20");
}
} // namespace maicord::hub_page_test

MAICORD_TEST_ENTRY(maicord_hub_page_entry) {
    maicord::hub_page_test::run();
    maicord::test_exit(maicord::hub_page_test::failures, "Server Hub page");
}
