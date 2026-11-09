// Game detection: Discord's detectable list (decode, the image build, the image as saved and loaded again, matching
// against a list of 25,000 games, the refresh against a fake server with ETags and 304s) and matching over fake
// process snapshots (path-suffix, file-name and argument entries, launchers, exclusions, windowless processes,
// several games at once, per-game detection, added programs) and the saved games record.
#include "maicord/app/account/game_detection.hpp"
#include "maicord/app/update/https.hpp"
#include "maicord/core/crc32.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "../../support/test_output.hpp"

namespace maicord::game_detection_test {
u32 failures = 0;
void check(bool valid, const char* name) noexcept {
    test_check(&failures, valid, name);
}
bool same(Utf8View a, Utf8View b) noexcept {
    return utf8_equal(a, b);
}

constexpr u64 kRoblox = 363445589247131668ull;
constexpr u64 kTerraria = 1402418344912752671ull;
constexpr u64 kStardew = 359509387670192128ull;
constexpr Utf8View kRobloxDirectory = utf8_literal("c:/users/u/appdata/local/roblox/versions/version-1/");

// Discord's own entries (cdn.discordapp.com/detectables/games-v1.json, store SKUs left out), plus a game it knows
// only by SKU and one with macOS executables alone.
constexpr Utf8View kGames = utf8_literal(
    R"([{"aliases":["Garry's Mod"],"executables":[{"arguments":"-game garrysmod","name":"hl2.exe","os":"win32"},)"
    R"({"arguments":"-game garrysmod","name":">hl2_osx","os":"darwin"},{"name":"win64/gmod.exe","os":"win32"},)"
    R"({"name":"bin/gmad.exe","os":"win32"},{"arguments":"-game garrysmod","name":">hl2.exe","os":"win32"},)"
    R"({"arguments":"-game garrysmod","name":"garrysmod/hl2.exe","os":"win32"}],"id":"356879032584896512",)"
    R"("name":"Garry's Mod"},{"executables":[{"arguments":"-game tf","name":">hl2.exe","os":"win32"},)"
    R"({"arguments":"-game tf","name":"hl2tf.exe","os":"win32"},{"arguments":"-game tf","name":">hl2_osx",)"
    R"("os":"darwin"}],"id":"356888577310851072","name":"Team Fortress 2"},{"aliases":["Stardew Valley"],)"
    R"("executables":[{"name":"stardew valley.exe","os":"win32"},{"name":"stardew valley/stardew valley.exe",)"
    R"("os":"win32"}],"id":"359509387670192128","name":"Stardew Valley"},{"aliases":["Roblox"],"executables":)"
    R"([{"name":"roblox.exe","os":"win32"},{"name":"robloxplayerbeta.exe","os":"win32"},{"name":"roblox.app",)"
    R"("os":"darwin"}],"id":"363445589247131668","name":"ROBLOX"},{"aliases":["Counter-Strike 2"],"executables":)"
    R"([{"name":"win64/cs2.exe","os":"win32"}],"id":"1158877933042143272","name":"Counter-Strike 2"},)"
    R"({"aliases":["Terraria"],"executables":[{"name":"terraria.app","os":"darwin"},{"name":"terraria/terraria.exe",)"
    R"("os":"win32"},{"name":"terraria.bin.x86","os":"linux"},{"name":"terraria.exe","os":"win32"}],)"
    R"("id":"1402418344912752671","name":"Terraria"},{"executables":[{"arguments":"net.minecraft.client.main.Main",)"
    R"("name":">javaw.exe","os":"win32"},{"name":"minecraft.windows.exe","os":"win32"}],"id":"1402418491272986635",)"
    R"("name":"Minecraft"},{"executables":[{"name":"league of legends.exe","os":"win32"},{"is_launcher":true,)"
    R"("name":"leagueclientux.exe","os":"win32"},{"name":"league of legends.app","os":"darwin"}],)"
    R"("id":"1402418696126992445","name":"League of Legends"},{"id":"1","name":"Store only",)"
    R"("third_party_skus":[{"distributor":"steam","id":"1"}]},{"executables":[{"name":"mac only.app","os":"darwin"}],)"
    R"("id":"2","name":"Mac only"}])");
// GET /games/detectable/exclusions as Discord answers it.
constexpr Utf8View kExclusions = utf8_literal(
    R"({"executables":["brocrashreporter.exe","crashreportclient.exe","install.exe","launcher.exe",)"
    R"("unitycrashhandler64.exe","UE4PrereqSetup_x64.exe"],"patterns":["vcredist.*\\.exe$","^(a|b)\\.exe"]})");

alignas(8) u8 g_image[64 * 1024];
u32 g_structurals[16384];
DetectableGameRow g_rows[64];
DetectableKey g_keys[128];
char g_text[16384];
DetectableBuildScratch scratch() noexcept {
    return {{g_structurals, 16384}, {g_rows, 64}, {g_keys, 128}, {g_text, sizeof(g_text)}};
}
DetectableList fixture_list() noexcept {
    const u32 bytes = detectable_list_build({kGames, kExclusions, {}, utf8_literal("W/\"g1\""),
                                             utf8_literal("W/\"e1\""), 1000, 2000},
                                            scratch(), {g_image, sizeof(g_image)});
    DetectableList list{};
    detectable_list_view({g_image, bytes}, &list);
    return list;
}

GameProcess process(Utf8View path, u32 pid, bool windowed = true, Utf8View arguments = {}) noexcept {
    return {path, arguments, pid, windowed};
}
char g_paths[8][160];
// A Roblox executable in the player's install directory.
Utf8View roblox(u32 slot, Utf8View file) noexcept {
    copy_bytes(g_paths[slot], kRobloxDirectory.data, kRobloxDirectory.bytes);
    copy_bytes(g_paths[slot] + kRobloxDirectory.bytes, file.data, file.bytes);
    return {g_paths[slot], kRobloxDirectory.bytes + file.bytes};
}
DetectableList g_list;
GameDetectionMatch select(const RegisteredGames& games, const GameProcess* list, u32 count, u32 previous = 0,
                          u32 foreground = 0) noexcept {
    return game_detection_select(games, g_list, {list, count}, previous, foreground);
}

void decode() noexcept {
    g_list = fixture_list();
    const auto* h = g_list.header;
    check(h && h->game_count == 8 && h->games_checked_ms == 1000 && h->exclusions_checked_ms == 2000 &&
              same({h->games_etag, h->games_etag_bytes}, utf8_literal("W/\"g1\"")),
          "decode: games with executables here are kept (not SKU-only or macOS-only ones), with the ETags");
    const auto gmod = detectable_list_game(g_list, 0);
    check(gmod.application_id == 356879032584896512ull && same(gmod.name, utf8_literal("Garry's Mod")) &&
              same(gmod.executables, utf8_literal("hl2.exe?-game garrysmod|win64/gmod.exe|bin/gmad.exe|"
                                                  ">hl2.exe?-game garrysmod|garrysmod/hl2.exe?-game garrysmod")),
          "decode: executables keep their path, >, arguments (lowercase) and drop the macOS ones");
    const auto league = detectable_list_game(g_list, 7);
    check(same(league.executables, utf8_literal("league of legends.exe|*leagueclientux.exe")),
          "decode: is_launcher marks an executable as a launcher");
    const auto terraria = detectable_list_game(g_list, 5);
    check(same(terraria.executables, utf8_literal("terraria/terraria.exe|terraria.bin.x86|terraria.exe")),
          "decode: the Linux executables are kept beside the Windows ones");
    const Utf8View exclusions{g_list.text + h->games_text_bytes, h->exclusions_bytes};
    const Utf8View patterns{exclusions.data + exclusions.bytes, h->patterns_bytes};
    check(same(exclusions, utf8_literal("brocrashreporter.exe|crashreportclient.exe|install.exe|launcher.exe|"
                                        "unitycrashhandler64.exe|ue4prereqsetup_x64.exe")) &&
              same(patterns, utf8_literal("vcredist.*\\.exe$")),
          "decode: exclusions are lowercased; patterns outside the supported subset are dropped");
    // Keys: one per distinct file name of a game, sorted.
    bool sorted = true;
    for (u32 i = 1; i < h->key_count; ++i)
        sorted = sorted && (g_list.keys[i - 1].hash < g_list.keys[i].hash ||
                            (g_list.keys[i - 1].hash == g_list.keys[i].hash &&
                             g_list.keys[i - 1].game < g_list.keys[i].game));
    check(sorted && h->key_count == 15, "decode: keys are sorted and a game's repeated file name keeps one key");
    check(!detectable_list_build({utf8_literal("[{\"id\":"), {}, {}, {}, {}, 0, 0}, scratch(),
                                 {g_image + 32768, 32768}) &&
              !detectable_list_build({kGames, {}, {}, {}, {}, 0, 0}, scratch(), {g_image + 32768, 256}),
          "decode: malformed JSON or an image that does not fit builds nothing");
    g_list = fixture_list();
}

void image() noexcept {
    // The image is the file: saved, read back into other memory and viewed again, it is the same list.
    alignas(8) static u8 copy[64 * 1024];
    const u32 bytes = g_list.header->bytes;
    copy_bytes(copy, g_image, bytes);
    DetectableList loaded{};
    const GameProcess player[]{process(roblox(0, utf8_literal("robloxplayerbeta.exe")), 100)};
    check(detectable_list_view({copy, bytes}, &loaded) && loaded.header->game_count == 8 &&
              game_detection_select({}, loaded, {player, 1}, 0, 0).application_id == kRoblox,
          "image: a saved image read back matches as the built one");
    const auto refuse = [&](u32 offset, u8 value, u32 size) noexcept {
        copy_bytes(copy, g_image, bytes);
        copy[offset] = value;
        DetectableList view{};
        return !detectable_list_view({copy, size}, &view) && !view.header;
    };
    const u32 keys = sizeof(DetectableImageHeader) + g_list.header->game_count * sizeof(DetectableGameRow);
    // Byte 7 of a key is its game index's top byte; byte 15 of a row its executables offset's.
    check(refuse(16, 'X', bytes) && refuse(0, 0, bytes - 1) && refuse(keys + 7, 0xFF, bytes) &&
              refuse(sizeof(DetectableImageHeader) + 15, 0xFF, bytes),
          "image: a wrong magic, a truncated file, a key past the games or a row past the text are refused");
}

void roblox_cases() noexcept {
    const RegisteredGames none{};
    // Roblox's executables on a real install: Discord's ROBLOX entry names roblox.exe and robloxplayerbeta.exe.
    const GameProcess install[]{process(roblox(0, utf8_literal("robloxplayerbeta.exe")), 100),
                                process(roblox(1, utf8_literal("robloxstudiobeta.exe")), 101),
                                process(roblox(2, utf8_literal("robloxplayerinstaller.exe")), 102),
                                process(roblox(3, utf8_literal("robloxcrashhandler.exe")), 103, false)};
    auto match = select(none, install, 4);
    check(match.found && !match.custom && match.pid == 100 && match.application_id == kRoblox &&
              same(match.name, utf8_literal("ROBLOX")),
          "roblox: the player shows as ROBLOX with its application id");
    match = select(none, install + 1, 3);
    check(!match.found, "roblox: studio, the installer and the crash handler are not the game");
    // The reported case: the player closed but its process lingering without a window, with the crash handler.
    const GameProcess lingering[]{process(roblox(0, utf8_literal("robloxplayerbeta.exe")), 100, false),
                                  process(roblox(3, utf8_literal("robloxcrashhandler.exe")), 103, false)};
    check(!select(none, lingering, 2).found, "roblox: a player process without a window is not playing");
    const GameProcess near[]{process(utf8_literal("c:/tools/myroblox.exe"), 1),
                             process(utf8_literal("c:/tools/roblox.exe.old"), 2),
                             process(utf8_literal("c:/robloxplayerbeta.exe/helper.exe"), 3)};
    check(!select(none, near, 3).found, "roblox: a path only containing an executable's name does not match");
    check(!game_detection_select(none, {}, {install, 1}, 0, 0).found, "roblox: before any list, nothing matches");
}

void matching() noexcept {
    const RegisteredGames none{};
    const GameProcess cs2[]{process(utf8_literal("d:/steam/steamapps/common/cs2/game/bin/win64/cs2.exe"), 1)};
    const GameProcess wrong_directory[]{process(utf8_literal("d:/x/bin/notwin64/cs2.exe"), 1),
                                        process(utf8_literal("d:/cs2.exe"), 2)};
    check(same(select(none, cs2, 1).name, utf8_literal("Counter-Strike 2")) && !select(none, wrong_directory, 2).found,
          "match: an entry with directories matches the last path segments only");
    const GameProcess league[]{process(utf8_literal("c:/riot games/league of legends/leagueclientux.exe"), 5),
                               process(utf8_literal("c:/riot games/league of legends/game/league of legends.exe"), 6)};
    check(!select(none, league, 1).found && select(none, league, 2).pid == 6,
          "match: a launcher alone is not played; its game is");
    const GameProcess linux_terraria[]{process(utf8_literal("/home/u/.steam/terraria/terraria.bin.x86"), 7)};
    check(select(none, linux_terraria, 1).application_id == kTerraria, "match: a Linux executable matches");
    // Arguments: hl2.exe is Team Fortress 2 or Garry's Mod by its command line.
    const Utf8View hl2 = utf8_literal("d:/steam/steamapps/common/team fortress 2/hl2.exe");
    const Utf8View long_line = utf8_literal("\"d:/steam/steamapps/common/team fortress 2/hl2.exe\" -steam -game tf "
                                            "-novid -high +fps_max 240");
    char kept[64];
    const u32 kept_bytes = game_detection_arguments(g_list, utf8_literal("hl2.exe"), long_line, {kept, sizeof(kept)});
    const GameProcess tf2[]{process(hl2, 9, true, {kept, kept_bytes})};
    const GameProcess gmod[]{process(hl2, 9, true, utf8_literal("-game garrysmod\n"))};
    const GameProcess bare[]{process(hl2, 9)};
    check(same({kept, kept_bytes}, utf8_literal("-game tf\n")) &&
              same(select(none, tf2, 1).name, utf8_literal("Team Fortress 2")) &&
              same(select(none, gmod, 1).name, utf8_literal("Garry's Mod")) && !select(none, bare, 1).found,
          "match: arguments pick the game an executable runs; without them nothing matches");
    const auto candidate = game_detection_candidate(none, g_list, utf8_literal("hl2.exe"));
    const auto player = game_detection_candidate(none, g_list, utf8_literal("robloxplayerbeta.exe"));
    check(candidate.named && candidate.arguments && player.named && !player.arguments &&
              !game_detection_candidate(none, g_list, utf8_literal("explorer.exe")).named &&
              game_detection_candidate(none, g_list, utf8_literal("obs64.exe")).named,
          "match: the scan reads paths and command lines only for files an entry names");
    check(game_detection_blocked(g_list, utf8_literal("c:/games/x/unitycrashhandler64.exe")) &&
              game_detection_blocked(g_list, utf8_literal("c:/redist/vcredist_x64.exe")) &&
              !game_detection_blocked(g_list, utf8_literal("c:/redist/vcredist_x64.exe.bak")) &&
              !game_detection_blocked(g_list, utf8_literal("c:/games/x/x.exe")) &&
              !game_detection_blocked({}, utf8_literal("c:/games/x/launcher.exe")),
          "match: Discord's exclusions block names and patterns; before any list nothing is blocked");
    check(detectable_pattern_match(utf8_literal("^c:/x/.*\\.exe$"), utf8_literal("c:/x/y.exe")) &&
              !detectable_pattern_match(utf8_literal("^c:/x/.*\\.exe$"), utf8_literal("d:/c:/x/y.exe")) &&
              detectable_pattern_match(utf8_literal("a.*b.*c"), utf8_literal("xxaybzc")) &&
              !detectable_pattern_supported(utf8_literal("a+b")) && !detectable_pattern_supported(utf8_literal(".*")),
          "match: patterns read literals, .*, ^ and $, and nothing else");
    check(game_detection_streamer_tool(process(utf8_literal("c:/program files/obs-studio/bin/64bit/obs64.exe"), 1)) &&
              game_detection_streamer_tool(process(utf8_literal("/usr/bin/obs"), 1)) &&
              !game_detection_streamer_tool(process(utf8_literal("c:/apps/obsidian.exe"), 1)),
          "match: streamer tools are the official client's list");
    char path[kRegisteredGameExecutableBytes];
    u8 bytes = 0;
    check(game_detection_normalize(utf8_literal("C:\\Users\\U\\Roblox\\RobloxPlayerBeta.EXE"), path, &bytes) &&
              same({path, bytes}, utf8_literal("c:/users/u/roblox/robloxplayerbeta.exe")),
          "match: paths fold to lowercase with '/' separators");
    char deep[400];
    for (u32 i = 0; i < sizeof(deep); ++i)
        deep[i] = i % 10 == 9 ? '\\' : 'a';
    copy_bytes(deep + sizeof(deep) - 9, "\\game.exe", 9);
    check(game_detection_normalize({deep, sizeof(deep)}, path, &bytes) && bytes <= kRegisteredGameExecutableBytes &&
              path[0] != '/' && same({path + bytes - 9, 9}, utf8_literal("/game.exe")),
          "match: a long path keeps its last whole segments");
    char name[64];
    check(same({name, game_detection_display_name(utf8_literal("c:/games/stardew valley.exe"), {name, 64})},
               utf8_literal("Stardew valley")),
          "match: display names drop the directory and .exe and capitalize");
}

void several() noexcept {
    RegisteredGames games{};
    const GameProcess both[]{process(utf8_literal("d:/steam/steamapps/common/terraria/terraria.exe"), 10),
                             process(utf8_literal("d:/steam/steamapps/common/stardew valley/stardew valley.exe"), 20)};
    check(select(games, both, 2).application_id == kStardew,
          "several: with no focus and nothing shown, the list order decides");
    check(select(games, both, 2, 10).application_id == kTerraria, "several: the game already shown stays");
    check(select(games, both, 2, 10, 20).application_id == kStardew, "several: the focused game wins");
    const u32 entry = registered_games_seen(&games, utf8_literal("Terraria"), kTerraria);
    games.items[entry].flags |= registered_game_detection_off;
    check(entry == 0 && select(games, both, 2, 10, 10).application_id == kStardew && !select(games, both, 1).found,
          "several: a game with detection off is skipped");
    check(registered_games_add(&games, utf8_literal("Lumen Drift"), utf8_literal("D:\\Games\\Lumen\\lumen.exe")) &&
              !registered_games_add(&games, utf8_literal("Again"), utf8_literal("d:/games/lumen/LUMEN.exe")),
          "several: added programs normalize and reject duplicates");
    const GameProcess added[]{both[1], process(utf8_literal("d:/games/lumen/lumen.exe"), 30),
                              process(utf8_literal("d:/other/lumen.exe"), 31)};
    auto match = select(games, added, 3);
    check(match.found && match.custom && match.pid == 30 && same(match.name, utf8_literal("Lumen Drift")) &&
              !select(games, added + 2, 1).found,
          "several: an added program wins over listed games and needs its own path");
    const GameProcess tools[]{process(utf8_literal("d:/games/lumen/launcher.exe"), 40)};
    check(registered_games_add(&games, utf8_literal("Tools"), utf8_literal("d:/games/lumen/launcher.exe")) &&
              !select(games, tools, 1).found,
          "several: an added program Discord's exclusions name never shows");
}

// 25,000 games, as many as Discord lists: every hundredth also ships "shared.exe".
void big_list() noexcept {
    constexpr u32 kGamesCount = 25'000;
    const usize json_bytes = 4u * 1024 * 1024;
    char* json = static_cast<char*>(commit_pages(json_bytes));
    u32 at = 0;
    const auto put = [&](Utf8View text) noexcept {
        copy_bytes(json + at, text.data, text.bytes);
        at += text.bytes;
    };
    const auto number = [&](u64 value) noexcept { at += decimal_write(json + at, value); };
    put(utf8_literal("["));
    for (u32 i = 0; i < kGamesCount; ++i) {
        put(i ? utf8_literal(",{\"executables\":[{\"name\":\"synthetic/game") : utf8_literal("{\"executables\":[{\"name\":\"synthetic/game"));
        number(i);
        put(utf8_literal(".exe\",\"os\":\"win32\"}"));
        if (i % 100 == 0)
            put(utf8_literal(",{\"name\":\"shared.exe\",\"os\":\"win32\"}"));
        put(utf8_literal("],\"id\":\""));
        number(9'000'000'000'000'000ull + i);
        put(utf8_literal("\",\"name\":\"Synthetic "));
        number(i);
        put(utf8_literal("\"}"));
    }
    put(utf8_literal("]"));
    const usize scratch_bytes = (at / 2 + 64) * 4 + (at / 32 + 16) * sizeof(DetectableGameRow) +
                                (at / 16 + 16) * sizeof(DetectableKey) + at + 1024;
    u8* scratch = static_cast<u8*>(commit_pages(scratch_bytes));
    u8* image = static_cast<u8*>(commit_pages(json_bytes));
    auto* rows = reinterpret_cast<DetectableGameRow*>(scratch);
    auto* keys = reinterpret_cast<DetectableKey*>(rows + at / 32 + 16);
    auto* structurals = reinterpret_cast<u32*>(keys + at / 16 + 16);
    char* text = reinterpret_cast<char*>(structurals + at / 2 + 64);
    const u32 bytes = json && scratch && image
                          ? detectable_list_build({{json, at}, {}, {}, {}, {}, 1, 0},
                                                  {{structurals, at / 2 + 64}, {rows, at / 32 + 16},
                                                   {keys, at / 16 + 16}, {text, at + 1024}},
                                                  {image, static_cast<u32>(json_bytes)})
                          : 0;
    DetectableList list{};
    check(bytes && detectable_list_view({image, bytes}, &list) && list.header->game_count == kGamesCount &&
              list.header->key_count == kGamesCount + kGamesCount / 100,
          "big list: 25,000 games build into one image with a key per executable file");
    const GameProcess last[]{process(utf8_literal("c:/games/synthetic/game24999.exe"), 1)};
    const GameProcess shared[]{process(utf8_literal("c:/x/shared.exe"), 2)};
    const auto found = game_detection_select({}, list, {last, 1}, 0, 0);
    check(found.application_id == 9'000'000'000'000'000ull + 24'999 && same(found.name, utf8_literal("Synthetic 24999")) &&
              game_detection_select({}, list, {shared, 1}, 0, 0).index == 0 &&
              game_detection_candidate({}, list, utf8_literal("shared.exe")).named &&
              !game_detection_candidate({}, list, utf8_literal("game25000.exe")).named,
          "big list: a game deep in the list matches; a file many games ship goes to the first");
    if (json)
        pages_release(json, json_bytes);
    if (scratch)
        pages_release(scratch, scratch_bytes);
    if (image)
        pages_release(image, json_bytes);
}

// The fake server: the CDN's games list and the API's exclusions, each with an ETag, answering 304 to a matching
// If-None-Match; `games` and `exclusions` are what it serves, `games_tag`/`exclusions_tag` their ETags.
struct FakeServer {
    Utf8View games, exclusions, games_tag, exclusions_tag;
    bool offline;
    u32 requests, not_modified;
    // 0 plain; with Accept-Encoding: gzip, 1 a valid gzip body, 2 one with a wrong CRC-32, 3 one cut short.
    u8 gzip;
    bool asked_gzip;
};
UpdateTransportError fake_exchange(void* context, const UpdateHttpRequest& request, const HttpsExchangeOptions& options,
                                   UpdateHttpResponse* response) noexcept {
    auto& s = *static_cast<FakeServer*>(context);
    ++s.requests;
    reset_value(response);
    if (s.offline)
        return UpdateTransportError::connect;
    const bool games = same(request.host_name, utf8_literal("cdn.discordapp.com")) &&
                       same({request.target.data, request.target.count}, utf8_literal("/detectables/games-v1.json"));
    const bool exclusions = same(request.host_name, utf8_literal("discord.com")) &&
                            same({request.target.data, request.target.count},
                                 utf8_literal("/api/v9/games/detectable/exclusions"));
    if (!games && !exclusions) {
        response->status = 404;
        return UpdateTransportError::none;
    }
    const Utf8View body = games ? s.games : s.exclusions, tag = games ? s.games_tag : s.exclusions_tag;
    Utf8View sent{};
    for (u32 i = 0; i < options.headers.count; ++i)
        if (same(options.headers[i].name, utf8_literal("If-None-Match")))
            sent = options.headers[i].value;
    copy_bytes(response->etag, tag.data, tag.bytes);
    response->etag_bytes = static_cast<u8>(tag.bytes);
    if (sent.bytes && same(sent, tag)) {
        ++s.not_modified;
        response->status = 304;
        return UpdateTransportError::none;
    }
    response->status = 200;
    s.asked_gzip |= options.accept_gzip;
    if (s.gzip && options.accept_gzip) {
        // A gzip member of stored deflate blocks, then CRC-32 and ISIZE (RFC 1952).
        static u8 packed[64 * 1024];
        constexpr u8 kHeader[10]{0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
        copy_bytes(packed, kHeader, 10);
        u32 at = 10;
        for (u32 done = 0; done < body.bytes || !done;) {
            const u32 n = minimum<u32>(body.bytes - done, 0xFFFF);
            packed[at++] = done + n == body.bytes ? 1 : 0;
            const u8 lengths[4]{static_cast<u8>(n), static_cast<u8>(n >> 8), static_cast<u8>(~n),
                                static_cast<u8>(~n >> 8)};
            copy_bytes(packed + at, lengths, 4);
            copy_bytes(packed + at + 4, body.data + done, n);
            at += 4 + n;
            done += n;
            if (!n)
                break;
        }
        const u32 crc = crc32_extend(0, body.data, body.bytes) ^ (s.gzip == 2 ? 1u : 0u);
        for (u32 i = 0; i < 4; ++i) {
            packed[at + i] = static_cast<u8>(crc >> (8 * i));
            packed[at + 4 + i] = static_cast<u8>(body.bytes >> (8 * i));
        }
        at += 8;
        if (s.gzip == 3)
            at -= 20; // the end of the data and the trailer's CRC never arrive
        response->gzip = true;
        response->body_bytes = at;
        return request.sink(request.sink_context, {packed, at}) ? UpdateTransportError::none
                                                                : UpdateTransportError::sink;
    }
    response->body_bytes = body.bytes;
    return request.sink(request.sink_context, {reinterpret_cast<const u8*>(body.data), body.bytes})
               ? UpdateTransportError::none
               : UpdateTransportError::sink;
}
// Content-Encoding: gzip, as the CDN answers the official client: inflated and checked before the build.
void refresh_gzip() noexcept {
    FakeServer server{kGames, kExclusions, utf8_literal("W/\"g1\""), utf8_literal("W/\"e1\""), false, 0, 0, 1, false};
    const auto packed = detectable_refresh_run({fake_exchange, &server, {}, 5000});
    DetectableList list{};
    check(server.asked_gzip && packed.image && detectable_list_view({packed.image, packed.bytes}, &list) &&
              list.header->game_count == 8 && list.header->exclusions_bytes != 0,
          "gzip: the lists are asked for gzipped and inflated before the build");
    if (packed.image)
        pages_release(packed.image, packed.bytes);
    for (u8 damage = 2; damage <= 3; ++damage) {
        server.gzip = damage;
        check(!detectable_refresh_run({fake_exchange, &server, {}, 5000}).image,
              damage == 2 ? "gzip: a body whose CRC-32 does not match builds nothing"
                          : "gzip: a truncated body builds nothing");
    }
}

void refresh() noexcept {
    FakeServer server{kGames, kExclusions, utf8_literal("W/\"g1\""), utf8_literal("W/\"e1\""), true, 0, 0};
    check(!detectable_refresh_run({fake_exchange, &server, {}, 5000}).image && server.requests == 2,
          "refresh: offline with nothing saved yields nothing");
    server.offline = false;
    const auto first = detectable_refresh_run({fake_exchange, &server, {}, 5000});
    DetectableList list{};
    check(first.image && detectable_list_view({first.image, first.bytes}, &list) && list.header->game_count == 8 &&
              list.header->games_checked_ms == 5000 && list.header->exclusions_checked_ms == 5000 &&
              same({list.header->games_etag, list.header->games_etag_bytes}, utf8_literal("W/\"g1\"")) &&
              same({list.header->exclusions_etag, list.header->exclusions_etag_bytes}, utf8_literal("W/\"e1\"")),
          "refresh: the first run fetches both lists and keeps their ETags");
    // A day later nothing changed: both answer 304 and the saved lists stay, checked again.
    const auto second = detectable_refresh_run({fake_exchange, &server, {first.image, first.bytes}, 90'000'000});
    DetectableList kept{};
    check(second.image && detectable_list_view({second.image, second.bytes}, &kept) && server.not_modified == 2 &&
              kept.header->game_count == 8 && kept.header->games_checked_ms == 90'000'000 &&
              kept.header->exclusions_checked_ms == 90'000'000 && second.bytes == first.bytes &&
              bytes_equal(first.image + sizeof(DetectableImageHeader), second.image + sizeof(DetectableImageHeader),
                          first.bytes - sizeof(DetectableImageHeader)),
          "refresh: 304s keep the saved lists as they were and note the check");
    // The games list changed (a new ETag): it is fetched again; the exclusions stay (304).
    server.games = utf8_literal(R"([{"executables":[{"name":"newgame.exe","os":"win32"}],"id":"77","name":"New"}])");
    server.games_tag = utf8_literal("W/\"g2\"");
    const auto third = detectable_refresh_run({fake_exchange, &server, {second.image, second.bytes}, 180'000'000});
    DetectableList changed{};
    check(third.image && detectable_list_view({third.image, third.bytes}, &changed) &&
              changed.header->game_count == 1 && detectable_list_game(changed, 0).application_id == 77 &&
              changed.header->exclusions_bytes == kept.header->exclusions_bytes && server.not_modified == 3 &&
              same({changed.header->games_etag, changed.header->games_etag_bytes}, utf8_literal("W/\"g2\"")),
          "refresh: a changed games list replaces the games and keeps the exclusions");
    // Offline later: the saved image stays as it is (no new image).
    server.offline = true;
    check(!detectable_refresh_run({fake_exchange, &server, {third.image, third.bytes}, 270'000'000}).image,
          "refresh: a failed run keeps the saved image");
    const DetectableRefreshResult* results[]{&first, &second, &third};
    for (const auto* r : results)
        if (r->image)
            pages_release(r->image, r->bytes);
}

void record() noexcept {
    RegisteredGames games{};
    const u32 seen = registered_games_seen(&games, utf8_literal("ROBLOX"), kRoblox);
    games.items[seen].last_played_ms = 1'700'000'000'000ull;
    games.items[seen].flags = registered_game_detection_off;
    check(registered_games_add(&games, utf8_literal("Lumen Drift"), utf8_literal("d:/games/lumen/lumen.exe")) &&
              registered_games_seen(&games, utf8_literal("ROBLOX"), kRoblox) == seen,
          "record: a detected game has one entry");
    u8 bytes[16384];
    RegisteredGames back{};
    const u32 size = registered_games_encode(games, {bytes, sizeof(bytes)});
    check(size && registered_games_decode({bytes, size}, &back) && back.count == 2 &&
              back.items[0].application_id == kRoblox && back.items[0].last_played_ms == 1'700'000'000'000ull &&
              back.items[0].flags == registered_game_detection_off && back.items[1].flags == registered_game_added &&
              same({back.items[1].executable, back.items[1].executable_bytes}, utf8_literal("d:/games/lumen/lumen.exe")),
          "record: entries keep their id, flags, last played time and path");
    check(!registered_games_decode({bytes, size - 1}, &back), "record: a truncated record is refused");
    const u8 old[] = {'M', 'C', 'G', '1', 1, 7, 'T', 'e', 'r', 'r', 'a', ' ', 'X', 8, 'g', 'a', 'm', 'e', '.', 'e', 'x',
                      'e', 'x'};
    check(registered_games_decode({old, sizeof(old) - 1}, &back) && back.count == 1 &&
              back.items[0].flags == registered_game_added &&
              same({back.items[0].executable, back.items[0].executable_bytes}, utf8_literal("game.exe")) &&
              !registered_games_decode({old, sizeof(old)}, &back),
          "record: the previous added-programs record still loads");
    RegisteredGames full{};
    for (u32 i = 0; i < kRegisteredGameMaximum; ++i) {
        const u32 slot = registered_games_seen(&full, utf8_literal("Game"), 1000 + i);
        full.items[slot].last_played_ms = 5000 - i;
    }
    full.items[kRegisteredGameMaximum - 2].flags = registered_game_detection_off;
    const u32 replaced = registered_games_seen(&full, utf8_literal("New"), 99);
    check(replaced == kRegisteredGameMaximum - 1 && full.count == kRegisteredGameMaximum &&
              full.items[replaced].application_id == 99,
          "record: a full list replaces the detected game played longest ago");
}
} // namespace maicord::game_detection_test

extern "C" __declspec(noreturn) void WINAPI maicord_game_detection_entry() noexcept {
    using namespace maicord::game_detection_test;
    decode();
    image();
    roblox_cases();
    matching();
    several();
    big_list();
    refresh();
    refresh_gzip();
    record();
    ExitProcess(failures ? 1 : 0);
}
