#include "maicord/app/game_detection.hpp"
#include "maicord/app/account_profile.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/process_info.hpp"

namespace maicord {
namespace {
// Common titles and their Windows executables. Names only: no service-owned assets.
constexpr DetectableGame kBuiltin[]{
    {utf8_literal("Among Us"), utf8_literal("among us.exe"), 0},
    {utf8_literal("Apex Legends"), utf8_literal("r5apex.exe|r5apex_dx12.exe"), 0},
    {utf8_literal("Baldur's Gate 3"), utf8_literal("bg3.exe|bg3_dx11.exe"), 0},
    {utf8_literal("Call of Duty"), utf8_literal("cod.exe|cod24-cod.exe"), 0},
    {utf8_literal("Counter-Strike 2"), utf8_literal("cs2.exe"), 0},
    {utf8_literal("Cyberpunk 2077"), utf8_literal("cyberpunk2077.exe"), 0},
    {utf8_literal("Dead by Daylight"), utf8_literal("deadbydaylight-win64-shipping.exe"), 0},
    {utf8_literal("Deep Rock Galactic"), utf8_literal("fsd-win64-shipping.exe"), 0},
    {utf8_literal("Destiny 2"), utf8_literal("destiny2.exe"), 0},
    {utf8_literal("Dota 2"), utf8_literal("dota2.exe"), 0},
    {utf8_literal("ELDEN RING"), utf8_literal("eldenring.exe"), 0},
    {utf8_literal("Euro Truck Simulator 2"), utf8_literal("eurotrucks2.exe"), 0},
    {utf8_literal("Factorio"), utf8_literal("factorio.exe"), 0},
    {utf8_literal("Fall Guys"), utf8_literal("fallguys_client_game.exe"), 0},
    {utf8_literal("Fortnite"), utf8_literal("fortniteclient-win64-shipping.exe"), 0},
    {utf8_literal("Garry's Mod"), utf8_literal("gmod.exe"), 0},
    {utf8_literal("Genshin Impact"), utf8_literal("genshinimpact.exe|yuanshen.exe"), 0},
    {utf8_literal("Geometry Dash"), utf8_literal("geometrydash.exe"), 0},
    {utf8_literal("Grand Theft Auto V"), utf8_literal("gta5.exe|gta5_enhanced.exe"), 0},
    {utf8_literal("HELLDIVERS 2"), utf8_literal("helldivers2.exe"), 0},
    {utf8_literal("Hearthstone"), utf8_literal("hearthstone.exe"), 0},
    {utf8_literal("Hollow Knight"), utf8_literal("hollow_knight.exe"), 0},
    {utf8_literal("League of Legends"), utf8_literal("league of legends.exe"), 0},
    {utf8_literal("Lethal Company"), utf8_literal("lethal company.exe"), 0},
    {utf8_literal("Marvel Rivals"), utf8_literal("marvel-win64-shipping.exe"), 0},
    {utf8_literal("Minecraft"), utf8_literal("minecraft.windows.exe"), 0},
    {utf8_literal("osu!"), utf8_literal("osu!.exe"), 0},
    {utf8_literal("Overwatch 2"), utf8_literal("overwatch.exe"), 0},
    {utf8_literal("Palworld"), utf8_literal("palworld-win64-shipping.exe"), 0},
    {utf8_literal("Path of Exile"), utf8_literal("pathofexile_x64.exe|pathofexile.exe"), 0},
    {utf8_literal("Phasmophobia"), utf8_literal("phasmophobia.exe"), 0},
    {utf8_literal("PUBG: BATTLEGROUNDS"), utf8_literal("tslgame.exe"), 0},
    {utf8_literal("Rainbow Six Siege"), utf8_literal("rainbowsix.exe|rainbowsix_vulkan.exe"), 0},
    {utf8_literal("Roblox"), utf8_literal("robloxplayerbeta.exe"), 0},
    {utf8_literal("Rocket League"), utf8_literal("rocketleague.exe"), 0},
    {utf8_literal("Rust"), utf8_literal("rustclient.exe"), 0},
    {utf8_literal("Stardew Valley"), utf8_literal("stardew valley.exe"), 0},
    {utf8_literal("Team Fortress 2"), utf8_literal("tf_win64.exe|tf.exe"), 0},
    {utf8_literal("Terraria"), utf8_literal("terraria.exe"), 0},
    {utf8_literal("The Sims 4"), utf8_literal("ts4_x64.exe"), 0},
    {utf8_literal("VALORANT"), utf8_literal("valorant-win64-shipping.exe"), 0},
    {utf8_literal("Valheim"), utf8_literal("valheim.exe"), 0},
    {utf8_literal("Warframe"), utf8_literal("warframe.x64.exe"), 0},
    {utf8_literal("World of Warcraft"), utf8_literal("wow.exe"), 0},
};
constexpr u8 kMagic[4] = {'M', 'C', 'G', '1'};

bool listed(Utf8View list, Utf8View name) noexcept {
    u32 begin = 0;
    for (u32 i = 0; i <= list.bytes; ++i)
        if (i == list.bytes || list.data[i] == '|') {
            if (utf8_equal({list.data + begin, i - begin}, name))
                return true;
            begin = i + 1;
        }
    return false;
}
bool running(ConstSlice<Utf8View> processes, Utf8View executable) noexcept {
    for (u32 i = 0; i < processes.count; ++i)
        if (utf8_equal(processes[i], executable))
            return true;
    return false;
}
struct WindowOwners {
    DWORD pids[512];
    u32 count;
};
BOOL CALLBACK collect_window(HWND window, LPARAM opaque) {
    auto& owners = *reinterpret_cast<WindowOwners*>(opaque);
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) || !GetWindowTextLengthW(window))
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    for (u32 i = 0; i < owners.count; ++i)
        if (owners.pids[i] == pid)
            return TRUE;
    if (owners.count < 512)
        owners.pids[owners.count++] = pid;
    return TRUE;
}
} // namespace

ConstSlice<DetectableGame> game_detection_builtin() noexcept {
    return {kBuiltin, sizeof(kBuiltin) / sizeof(kBuiltin[0])};
}
bool game_detection_normalize(Utf8View basename, char* output, u8* bytes) noexcept {
    if (!output || !bytes || !basename.data || !basename.bytes || basename.bytes > kRegisteredGameExecutableBytes ||
        !utf8_validate(basename).valid)
        return false;
    for (u32 i = 0; i < basename.bytes; ++i) {
        const char c = basename.data[i];
        if (c == '\\' || c == '/' || c == ':' || c == '|' || static_cast<u8>(c) < 0x20)
            return false;
        output[i] = ascii_lower(c);
    }
    *bytes = static_cast<u8>(basename.bytes);
    return true;
}
u32 game_detection_display_name(Utf8View basename, Slice<char> output) noexcept {
    if (!output.data || !basename.bytes)
        return 0;
    u32 bytes = basename.bytes;
    if (bytes > 4 && basename.data[bytes - 4] == '.' && ascii_lower(basename.data[bytes - 3]) == 'e' &&
        ascii_lower(basename.data[bytes - 2]) == 'x' && ascii_lower(basename.data[bytes - 1]) == 'e')
        bytes -= 4;
    bytes = utf8_copy_truncated(output.data, output.count, {basename.data, bytes});
    if (bytes && output.data[0] >= 'a' && output.data[0] <= 'z')
        output.data[0] = static_cast<char>(output.data[0] - 'a' + 'A');
    return bytes;
}
bool registered_games_add(RegisteredGames* games, Utf8View name, Utf8View executable) noexcept {
    if (!games || games->count >= kRegisteredGameMaximum || !name.bytes || name.bytes > kRegisteredGameNameBytes ||
        !utf8_validate(name).valid)
        return false;
    RegisteredGame game{};
    if (!game_detection_normalize(executable, game.executable, &game.executable_bytes))
        return false;
    for (u32 i = 0; i < games->count; ++i)
        if (utf8_equal({games->items[i].executable, games->items[i].executable_bytes},
                 {game.executable, game.executable_bytes}))
            return false;
    copy_bytes(game.name, name.data, name.bytes);
    game.name_bytes = static_cast<u8>(name.bytes);
    games->items[games->count++] = game;
    return true;
}
bool registered_games_remove(RegisteredGames* games, u32 index) noexcept {
    if (!games || index >= games->count)
        return false;
    for (u32 i = index; i + 1 < games->count; ++i)
        games->items[i] = games->items[i + 1];
    games->items[--games->count] = {};
    return true;
}
GameDetectionMatch game_detection_match(const RegisteredGames& custom, ConstSlice<DetectableGame> builtin,
                                        ConstSlice<Utf8View> processes) noexcept {
    for (u32 i = 0; i < custom.count && i < kRegisteredGameMaximum; ++i) {
        const auto& game = custom.items[i];
        if (running(processes, {game.executable, game.executable_bytes}))
            return {{game.name, game.name_bytes}, 0, i, true, true};
    }
    for (u32 i = 0; i < builtin.count; ++i)
        for (u32 p = 0; p < processes.count; ++p)
            if (listed(builtin[i].executables, processes[p]))
                return {builtin[i].name, builtin[i].application_id, i, false, true};
    return {};
}
u32 game_detection_snapshot(Slice<char> storage, Slice<Utf8View> names, bool windowed_only) noexcept {
    if (!storage.data || !names.data)
        return 0;
    WindowOwners owners{};
    if (windowed_only)
        EnumWindows(collect_window, reinterpret_cast<LPARAM>(&owners));
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    const DWORD self = GetCurrentProcessId();
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    u32 count = 0, used = 0;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && count < names.count;
         more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == self || entry.th32ProcessID <= 4)
            continue;
        if (windowed_only) {
            bool owns = false;
            for (u32 i = 0; i < owners.count && !owns; ++i)
                owns = owners.pids[i] == entry.th32ProcessID;
            if (!owns)
                continue;
        }
        char utf8[kRegisteredGameExecutableBytes * 3];
        const int converted = WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1, utf8, sizeof(utf8), nullptr,
                                                  nullptr);
        if (converted <= 1)
            continue;
        char normalized[kRegisteredGameExecutableBytes];
        u8 bytes = 0;
        if (!game_detection_normalize({utf8, static_cast<u32>(converted - 1)}, normalized, &bytes) ||
            bytes > storage.count - used)
            continue;
        bool duplicate = false;
        for (u32 i = 0; i < count && !duplicate; ++i)
            duplicate = utf8_equal(names[i], {normalized, bytes});
        if (duplicate)
            continue;
        copy_bytes(storage.data + used, normalized, bytes);
        names[count++] = {storage.data + used, bytes};
        used += bytes;
    }
    CloseHandle(snapshot);
    return count;
}
u32 registered_games_encode(const RegisteredGames& games, Slice<u8> output) noexcept {
    if (!output.data || games.count > kRegisteredGameMaximum)
        return 0;
    u32 at = 0;
    const auto put = [&](const void* data, u32 bytes) noexcept {
        if (bytes > output.count - at)
            return false;
        copy_bytes(output.data + at, data, bytes);
        at += bytes;
        return true;
    };
    const u8 count = static_cast<u8>(games.count);
    if (!put(kMagic, 4) || !put(&count, 1))
        return 0;
    for (u32 i = 0; i < games.count; ++i) {
        const auto& g = games.items[i];
        if (!put(&g.name_bytes, 1) || !put(g.name, g.name_bytes) || !put(&g.executable_bytes, 1) ||
            !put(g.executable, g.executable_bytes))
            return 0;
    }
    return at;
}
bool registered_games_decode(ConstSlice<u8> input, RegisteredGames* output) noexcept {
    if (!output || !input.data || input.count < 5 || !bytes_equal(input.data, kMagic, 4) ||
        input.data[4] > kRegisteredGameMaximum)
        return false;
    RegisteredGames games{};
    u32 at = 5;
    for (u32 i = 0; i < input.data[4]; ++i) {
        if (at >= input.count)
            return false;
        const u8 name = input.data[at++];
        if (name > input.count - at)
            return false;
        const Utf8View name_view{reinterpret_cast<const char*>(input.data + at), name};
        at += name;
        if (at >= input.count)
            return false;
        const u8 exe = input.data[at++];
        if (exe > input.count - at)
            return false;
        const Utf8View exe_view{reinterpret_cast<const char*>(input.data + at), exe};
        at += exe;
        if (!registered_games_add(&games, name_view, exe_view))
            return false;
    }
    if (at != input.count)
        return false;
    *output = games;
    return true;
}
bool registered_games_load(RegisteredGames* games) noexcept {
    if (!games)
        return false;
    *games = {};
    wchar_t path[kAccountProfileMaximumPathCharacters]{};
    if (!data_path(path, kAccountProfileMaximumPathCharacters, L"games.bin").ok())
        return false;
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    u8 record[5 + kRegisteredGameMaximum * (2 + kRegisteredGameNameBytes + kRegisteredGameExecutableBytes)];
    LARGE_INTEGER size{};
    u32 native = 0;
    const bool read = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= sizeof(record) &&
                      file_persistence_read_complete(file, {record, static_cast<u32>(size.QuadPart)}, &native);
    CloseHandle(file);
    return read && registered_games_decode({record, static_cast<u32>(size.QuadPart)}, games);
}
bool registered_games_save(const RegisteredGames& games) noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters]{};
    u8 record[5 + kRegisteredGameMaximum * (2 + kRegisteredGameNameBytes + kRegisteredGameExecutableBytes)];
    const u32 bytes = registered_games_encode(games, {record, sizeof(record)});
    if (!bytes)
        return false;
    const DataPathResult built = data_path(path, kAccountProfileMaximumPathCharacters, L"games.bin", true);
    return built.ok() && file_persistence_write_atomic(path, built.length, {record, bytes}).ok();
}
} // namespace maicord
