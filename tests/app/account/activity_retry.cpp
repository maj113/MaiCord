#include "../../../src/app/account/activity.cpp"
#include "maicord/core/scratch_pages.hpp"

// Game detection runs the real matcher (game_detection.cpp) and list build (detectable_list.cpp) over a fake process
// snapshot and a saved list built from Discord's entries; the tests never enumerate processes, fetch or touch files.
namespace maicord {
namespace {
GameScan g_fake_scan;
u32 g_fake_saves, g_refresh_starts;
} // namespace
void game_detection_scan(const RegisteredGames&, const DetectableList&, GameScan* scan) noexcept {
    copy_value(scan, g_fake_scan);
}
void game_detection_release() noexcept {}
u32 game_detection_choices(Slice<char>, Slice<Utf8View>) noexcept {
    return 0;
}
bool registered_games_load(RegisteredGames* games) noexcept {
    reset_value(games);
    return true;
}
bool registered_games_save(const RegisteredGames&) noexcept {
    ++g_fake_saves;
    return true;
}
// The saved list: ROBLOX and Terraria as Discord lists them, last checked long ago (so a refresh is due).
u8* detectable_image_load(u32* bytes) noexcept {
    static u32 structurals[1024];
    static DetectableGameRow rows[8];
    static DetectableKey keys[16];
    static char text[1024];
    alignas(8) static u8 image[4096];
    const Utf8View games = utf8_literal(
        R"([{"executables":[{"name":"roblox.exe","os":"win32"},{"name":"robloxplayerbeta.exe","os":"win32"}],)"
        R"("id":"363445589247131668","name":"ROBLOX"},{"executables":[{"name":"terraria.exe","os":"win32"}],)"
        R"("id":"1402418344912752671","name":"Terraria"}])");
    *bytes = detectable_list_build({games, {}, {}, {}, {}, 1, 1},
                                   {{structurals, 1024}, {rows, 8}, {keys, 16}, {text, sizeof(text)}},
                                   {image, sizeof(image)});
    u8* pages = *bytes ? static_cast<u8*>(commit_pages(*bytes)) : nullptr;
    if (pages)
        copy_bytes(pages, image, *bytes);
    return pages;
}
bool detectable_refresh_start(u64) noexcept {
    ++g_refresh_starts;
    return true;
}
DetectableRefreshResult detectable_refresh_take(bool* finished) noexcept {
    *finished = false;
    return {};
}
void detectable_refresh_cancel() noexcept {}
} // namespace maicord

namespace maicord::app_detail {
namespace {
u32 retry_completions;
bool WINAPI complete_retry(void*, u64, u64, u64, const RestClientResponse*, Utf8View) noexcept {
    ++retry_completions;
    return true;
}
GatewayLocalPresence g_published;
u32 g_publishes;
bool WINAPI capture_presence(void*, const GatewayLocalPresence* input, GatewayActivitySharing* output) noexcept {
    if (output)
        reset_value(output);
    if (input) {
        copy_value(&g_published, *input);
        ++g_publishes;
    }
    return true;
}
// The fake snapshot: processes as game_detection_scan reports them (normalized paths).
void fake(u32 count, const GameProcess* list) noexcept {
    reset_value(&g_fake_scan);
    for (u32 i = 0; i < count; ++i)
        g_fake_scan.processes[i] = list[i];
    g_fake_scan.count = count;
}
bool published(Utf8View name, u64 application_id) noexcept {
    return g_published.game && g_published.game_application_id == application_id &&
           utf8_equal({g_published.game_name, g_published.game_name_bytes}, name);
}

// The presence sequence over fake snapshots: shown, held, cleared, shown again and replaced.
void detection(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr u64 kRoblox = 363445589247131668ull;
    constexpr Utf8View kPlayer = utf8_literal("c:/users/u/appdata/local/roblox/versions/version-1/robloxplayerbeta.exe");
    constexpr Utf8View kCrash = utf8_literal("c:/users/u/appdata/local/roblox/versions/version-1/robloxcrashhandler.exe");
    constexpr Utf8View kStudio = utf8_literal("c:/users/u/appdata/local/roblox/versions/version-1/robloxstudiobeta.exe");
    const auto old_instance = g_app.account.instance;
    const auto old_presence = g_app.account.api.local_presence;
    const auto old_preferences = g_app.preferences;
    g_app.account.instance = &g_publishes;
    g_app.account.api.local_presence = capture_presence;
    g_app.preferences.game_detection = true;
    g_app.preferences.streamer_auto = false;
    g = {};
    g.account_scope = g_app.plugin_account_scope_serial;
    reset_value(&g_games);
    g_games_loaded = false;
    g_publishes = 0;
    u64 now = 1'000'000;
    const auto tick = [&]() noexcept {
        now += kScanIntervalMs;
        application_activity_tick(now);
    };

    const GameProcess playing[]{{kPlayer, {}, 100, true}, {kCrash, {}, 101, false}, {kStudio, {}, 102, true}};
    fake(3, playing);
    application_activity_tick(now);
    check(!g_published.game && g.scan_at == now + kScanIntervalMs,
          "activity: the first scan waits an interval instead of running during startup");
    tick();
    check(published(utf8_literal("ROBLOX"), kRoblox) && g_published.game_started_ms && g.game_pid == 100,
          "activity: the Roblox player is published with its application id; studio and crash handler are not");
    check(g_games.count == 1 && g_games.items[0].application_id == kRoblox && g_games.items[0].last_played_ms &&
              g_fake_saves,
          "activity: a shown game joins Added Games with its last played time");
    g.game_started_ms = g.published.game_started_ms = 1; // A start the next steps can tell apart.
    const u32 shown = g_publishes;
    tick();
    check(g_publishes == shown && g.game_started_ms == 1, "activity: a rescan of the same process publishes nothing");

    // The reported case: Roblox closed, its player process lingering without a window beside the crash handler.
    const GameProcess lingering[]{{kPlayer, {}, 100, false}, {kCrash, {}, 101, false}};
    fake(2, lingering);
    tick();
    check(!g_published.game && g_publishes == shown + 1, "activity: a windowless player clears Playing ROBLOX");

    fake(1, playing);
    tick();
    check(published(utf8_literal("ROBLOX"), kRoblox) && g_published.game_started_ms != 1,
          "activity: a new process on a reused pid starts a new session");
    g.game_started_ms = g.published.game_started_ms = 1;
    const GameProcess reused[]{{utf8_literal("d:/steam/steamapps/common/terraria/terraria.exe"), {}, 100, true}};
    fake(1, reused);
    tick();
    check(published(utf8_literal("Terraria"), 1402418344912752671ull) && g_published.game_started_ms != 1,
          "activity: a pid reused by another game replaces the game and its start");

    fake(0, nullptr);
    tick();
    check(!g_published.game, "activity: the game exiting clears the presence");

    // Toggle detection on the current game hides it at once and keeps it hidden.
    fake(1, playing);
    tick();
    bool toggled = false;
    check(published(utf8_literal("ROBLOX"), kRoblox) &&
              application_activity_settings_activate(SettingBinding::current_game, false, 0, &toggled) && toggled &&
              (application_activity_tick(now), !g_published.game),
          "activity: turning detection off for the current game clears it");
    tick();
    check(!g_published.game, "activity: a game turned off stays hidden while it runs");
    check(g_refresh_starts == 1 && g_detectables.list.header && g_detectables.list.header->game_count == 2,
          "activity: the saved list loads once and a stale one starts one refresh (the next waits an hour)");

    // Streamer tools alone when game detection is off.
    g_app.preferences.game_detection = false;
    g_app.preferences.streamer_auto = true;
    const GameProcess studio[]{{utf8_literal("c:/program files/obs-studio/bin/64bit/obs64.exe"), {}, 300, false},
                               {kPlayer, {}, 100, true}};
    fake(2, studio);
    tick();
    check(!g_detectables.image && !g_detectables.loaded, "activity: turning game detection off drops the list");
    check(application_activity_streamer_tool_running() && !g_published.game,
          "activity: streamer tools are seen while game detection is off, games are not");
    g_app.preferences.streamer_auto = false;
    tick();
    check(!application_activity_streamer_tool_running() && application_activity_next_wake() == g.idle_check_at,
          "activity: with both off nothing scans");

    g = {};
    reset_value(&g_games);
    g_games_loaded = false;
    g_app.account.instance = old_instance;
    g_app.account.api.local_presence = old_presence;
    g_app.preferences = old_preferences;
}
} // namespace

void test_activity_retry(void (*check)(bool, const char*) noexcept) noexcept {
    const auto old_instance = g_app.account.instance;
    const auto old_complete = g_app.account.api.self_status_complete;
    g_app.account.instance = &retry_completions;
    g_app.account.api.self_status_complete = complete_retry;
    retry_completions = 0;
    for (u32 kind = 0; kind < 5; ++kind) {
        g = {};
        g.request_id = 71;
        g.queued = true;
        g.attempts = static_cast<u8>(kind == 4 ? 3 : 1);
        RestClientResponse response{};
        response.request_id = 71;
        response.status_code = 429;
        response.rate_limited = kind != 1;
        response.retry_at_ms = kind == 2 ? 0 : 1234567;
        response.rejection.captcha = kind == 0;
        check(application_activity_sharing_response(response, {}), "activity exact completion consumed");
        if (kind == 3)
            check(g.queued && !g.failed && !g.request_id && g.retry_at == 1234567 && g.attempts == 1,
                  "activity genuine 429 retains exact deadline and attempt count");
        else
            check(!g.queued && g.failed && !g.request_id && !g.retry_at,
                  "activity challenge, unknown cooldown or exhausted retry is terminal");
    }
    check(retry_completions == 5, "activity completes each owned receipt once");
    g = {};
    g_app.account.instance = old_instance;
    g_app.account.api.self_status_complete = old_complete;
    detection(check);
}
} // namespace maicord::app_detail
