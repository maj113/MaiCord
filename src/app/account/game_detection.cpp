#include "maicord/app/account/game_detection.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/app/account/account_profile.hpp"
#include "maicord/app/net/file_persistence.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/packed_reader.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/media/compression/inflated_block.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/process_info.hpp"
#endif
#include "maicord/os/file.hpp"

namespace maicord {
namespace {
#include <game_pack.inc>
// Common titles and their Windows executables (src/app/account/game_data.inc: names only, no service-owned
// assets), shipped as one zlib stream (the format is in tools/pack_tables.py) that the first reader inflates.
DetectableGame g_builtin[kGameCount];
InflatedBlock g_builtin_block;

// Runs once on the inflated bytes, before any thread sees them.
void publish_builtin(const u8* data) noexcept {
    PackedReader in{data};
    for (auto& game : g_builtin) {
        game.name = in.text();
        game.executables = in.text();
    }
}
bool ready() noexcept {
    return g_builtin_block.state == 2 ||
           inflated_block_load(g_builtin_block, kGamePacked, sizeof(kGamePacked), kGameBytes, publish_builtin) != nullptr;
}
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
// Adds a process name to the snapshot once, normalized as registered executables are.
void add_process(Slice<char> storage, Slice<Utf8View> names, u32* count, u32* used, Utf8View name) noexcept {
    char normalized[kRegisteredGameExecutableBytes];
    u8 bytes = 0;
    if (!game_detection_normalize(name, normalized, &bytes) || bytes > storage.count - *used ||
        running({names.data, *count}, {normalized, bytes}))
        return;
    copy_bytes(storage.data + *used, normalized, bytes);
    names[(*count)++] = {storage.data + *used, bytes};
    *used += bytes;
}
#if defined(_WIN32)
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
#endif
} // namespace

ConstSlice<DetectableGame> game_detection_builtin() noexcept {
    return ready() ? ConstSlice<DetectableGame>{g_builtin, kGameCount} : ConstSlice<DetectableGame>{};
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
bool registered_games_add(RegisteredGames* games, const Utf8View& name,
                          const Utf8View& executable) noexcept {
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
    reset_value(&games->items[--games->count]);
    return true;
}
GameDetectionMatch game_detection_match(const RegisteredGames& custom, ConstSlice<DetectableGame> builtin,
                                        ConstSlice<Utf8View> processes) noexcept {
    for (u32 i = 0; i < custom.count && i < kRegisteredGameMaximum; ++i) {
        const auto& game = custom.items[i];
        if (running(processes, {game.executable, game.executable_bytes}))
            return {{game.name, game.name_bytes}, i, true, true};
    }
    for (u32 i = 0; i < builtin.count; ++i)
        for (u32 p = 0; p < processes.count; ++p)
            if (listed(builtin[i].executables, processes[p]))
                return {builtin[i].name, i, false, true};
    return {};
}
#if defined(_WIN32)
u32 game_detection_snapshot(Slice<char> storage, Slice<Utf8View> names, bool windowed_only) noexcept {
    if (!storage.data || !names.data)
        return 0;
    WindowOwners owners{};
    if (windowed_only)
        EnumWindows(collect_window, reinterpret_cast<LPARAM>(&owners));
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    const DWORD self = process_id();
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
        if (converted > 1)
            add_process(storage, names, &count, &used, {utf8, static_cast<u32>(converted - 1)});
    }
    CloseHandle(snapshot);
    return count;
}
#else
u32 game_detection_snapshot(Slice<char> storage, Slice<Utf8View> names, bool windowed_only) noexcept {
    // No client learns which processes own windows on Wayland, so "windowed" lists every process of the user. The
    // name is the process's comm: the program name the kernel keeps (Wine and Proton processes carry the .exe name).
    (void)windowed_only;
    if (!storage.data || !names.data)
        return 0;
    DirIter directory{};
    if (!dir_open(&directory, L"/proc"))
        return 0;
    const u32 self = process_id();
    u32 count = 0, used = 0;
    DirEntry entry{};
    while (count < names.count && dir_next(&directory, &entry)) {
        u32 pid = 0;
        bool numeric = entry.name[0] != 0;
        for (u32 i = 0; entry.name[i] && numeric; ++i) {
            numeric = entry.name[i] >= L'0' && entry.name[i] <= L'9';
            pid = pid * 10 + static_cast<u32>(entry.name[i] - L'0');
        }
        if (!numeric || pid == self)
            continue;
        wchar_t path[48] = L"/proc/";
        u32 length = 6;
        for (u32 i = 0; entry.name[i] && length < 30; ++i)
            path[length++] = entry.name[i];
        for (const wchar_t c : L"/comm")
            path[length++] = c;
        // No access hint: an fadvise of a 16-byte procfs file is one more system call per process for nothing.
        const FileHandle file = file_open(path, {FileAccess::read, FileCreate::existing,
                                                 kFileShareRead | kFileShareWrite, 0});
        if (file == INVALID_HANDLE_VALUE)
            continue;
        char text[64];
        u32 read = 0;
        const bool got = file_read(file, text, sizeof(text), &read);
        file_close(file);
        while (got && read && (text[read - 1] == '\n' || text[read - 1] == '\r'))
            --read;
        if (got)
            add_process(storage, names, &count, &used, {text, read});
    }
    dir_close(&directory);
    return count;
}
#endif
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
    reset_value(games);
    wchar_t path[kAccountProfileMaximumPathCharacters];
    if (!data_path(path, kAccountProfileMaximumPathCharacters, L"games.bin").ok())
        return false;
    u8 record[5 + kRegisteredGameMaximum * (2 + kRegisteredGameNameBytes + kRegisteredGameExecutableBytes)];
    const FileReadResult read = file_persistence_read_file(path, {record, sizeof(record)});
    return read.error == FileReadError::missing || (read.ok() && registered_games_decode({record, read.bytes}, games));
}
bool registered_games_save(const RegisteredGames& games) noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters];
    u8 record[5 + kRegisteredGameMaximum * (2 + kRegisteredGameNameBytes + kRegisteredGameExecutableBytes)];
    const u32 bytes = registered_games_encode(games, {record, sizeof(record)});
    if (!bytes)
        return false;
    const DataPathResult built = data_path(path, kAccountProfileMaximumPathCharacters, L"games.bin", true);
    return built.ok() && file_persistence_write_atomic(path, built.length, {record, bytes}).ok();
}
} // namespace maicord
