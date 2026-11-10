// The system side of game detection: one process snapshot per scan, the "Add it!" program list, the saved games
// and detectable list files, and the detectable list's refresh worker. Matching is game_detection.cpp, the list's
// format and build detectable_list.cpp.
#include "maicord/app/account/game_detection.hpp"
#include "maicord/app/account/account_profile.hpp"
#include "maicord/app/net/file_persistence.hpp"
#include "maicord/app/update/https.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/interlocked.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/os/file.hpp"
#include "maicord/os/thread.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/process_info.hpp"
#else
#include "maicord/sys/linux/calls.hpp"
#endif

namespace maicord {
namespace {
constexpr u32 kRecordBytes = 5 + kRegisteredGameMaximum * (19 + kRegisteredGameNameBytes + kRegisteredGameExecutableBytes);
// The command-line buffer, grown to the longest line read and reused (on Windows the query's UNICODE_STRING and
// its text, folded in place); only processes an argument entry names are read, so it usually never exists.
u8* g_line;
u32 g_line_capacity;
bool line_room(u32 bytes) noexcept {
    if (bytes <= g_line_capacity)
        return true;
    if (g_line)
        pages_release(g_line, g_line_capacity);
    g_line_capacity = (bytes + 4095) & ~4095u;
    g_line = static_cast<u8*>(commit_pages(g_line_capacity));
    if (!g_line)
        g_line_capacity = 0;
    return g_line != nullptr;
}
// Folds a line to lowercase ASCII (other characters become '?'): argument texts are ASCII.
char fold(u32 c) noexcept {
    return c && c < 0x80 ? ascii_lower(static_cast<char>(c)) : c ? '?' : ' ';
}

Utf8View file_of(Utf8View path) noexcept {
    u32 start = path.bytes;
    while (start && path.data[start - 1] != '/')
        --start;
    return {path.data + start, path.bytes - start};
}
// Appends a candidate: its normalized path (or its file name when the path is unknown) and the argument texts its
// command line holds.
void add_process(GameScan* scan, u32* used, u32 pid, Utf8View path, Utf8View command_line,
                 const DetectableList& list) noexcept {
    char* text = scan->text + *used;
    const u32 room = sizeof(scan->text) - *used;
    u8 bytes = 0;
    if (room < kRegisteredGameExecutableBytes || !game_detection_normalize(path, text, &bytes))
        return;
    const Utf8View normalized{text, bytes};
    const u32 arguments = command_line.bytes ? game_detection_arguments(list, file_of(normalized), command_line,
                                                                       {text + bytes, room - bytes})
                                             : 0;
    scan->processes[scan->count++] = {normalized, {text + bytes, arguments}, pid, true};
    *used += bytes + arguments;
}

#if defined(_WIN32)
constexpr u32 kProcessCommandLineInformation = 60; // PROCESSINFOCLASS; a UNICODE_STRING and its text follow.
constexpr u32 kCommandLineQueryLimit = 65536 + 16;
struct CommandLineText {
    u16 length, maximum_length;
    const wchar_t* text;
};

struct WindowOwners {
    DWORD pids[512];
    u32 count;
    bool titled; // Only windows with a title (the "Add it!" list).
};
void add_owner(WindowOwners& owners, HWND window) noexcept {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    for (u32 i = 0; i < owners.count; ++i)
        if (owners.pids[i] == pid)
            return;
    if (owners.count < 512)
        owners.pids[owners.count++] = pid;
}
BOOL CALLBACK collect_child(HWND window, LPARAM opaque) {
    add_owner(*reinterpret_cast<WindowOwners*>(opaque), window);
    return TRUE;
}
// Visible top-level windows that no other window owns. A packaged (UWP) app's frame belongs to the frame host; the
// app's own process owns the core window inside it.
BOOL CALLBACK collect_window(HWND window, LPARAM opaque) {
    auto& owners = *reinterpret_cast<WindowOwners*>(opaque);
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) || (owners.titled && !GetWindowTextLengthW(window)))
        return TRUE;
    add_owner(owners, window);
    constexpr wchar_t kFrame[] = L"ApplicationFrameWindow";
    wchar_t name[sizeof(kFrame) / sizeof(kFrame[0])];
    if (GetClassNameW(window, name, sizeof(kFrame) / sizeof(kFrame[0])) == sizeof(kFrame) / sizeof(kFrame[0]) - 1 &&
        bytes_equal(name, kFrame, sizeof(kFrame)))
        EnumChildWindows(window, collect_child, opaque);
    return TRUE;
}
bool owns_window(const WindowOwners& owners, u32 pid) noexcept {
    for (u32 i = 0; i < owners.count; ++i)
        if (owners.pids[i] == pid)
            return true;
    return false;
}
// The image path of a process as UTF-8, or 0.
u32 image_path(HANDLE process, char* output, u32 capacity) noexcept {
    wchar_t image[1024];
    DWORD length = 1024;
    if (!process || !QueryFullProcessImageNameW(process, 0, image, &length) || !length)
        return 0;
    const int converted = WideCharToMultiByte(CP_UTF8, 0, image, static_cast<int>(length), output,
                                              static_cast<int>(capacity), nullptr, nullptr);
    return converted > 0 ? static_cast<u32>(converted) : 0;
}
Utf8View command_line(HANDLE process) noexcept {
    if (!process)
        return {};
    const auto query = [&](ULONG* needed) noexcept {
        return NtQueryInformationProcess(process, static_cast<PROCESSINFOCLASS>(kProcessCommandLineInformation),
                                         g_line, g_line_capacity, needed) >= 0;
    };
    ULONG needed = 0;
    if (!query(&needed) && (!needed || needed > kCommandLineQueryLimit || !line_room(needed) || !query(&needed)))
        return {};
    // Character i lands at byte i, before any character still to be read (they start past the header).
    const auto& line = *reinterpret_cast<const CommandLineText*>(g_line);
    const wchar_t* text = line.text;
    const u32 units = line.length / 2;
    char* output = reinterpret_cast<char*>(g_line);
    for (u32 i = 0; i < units; ++i)
        output[i] = fold(text[i]);
    return {output, units};
}
u32 file_name(const PROCESSENTRY32W& entry, char* output) noexcept {
    char utf8[kRegisteredGameExecutableBytes * 3];
    const int converted =
        WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1, utf8, sizeof(utf8), nullptr, nullptr);
    u8 bytes = 0;
    return converted > 1 && game_detection_normalize({utf8, static_cast<u32>(converted - 1)}, output, &bytes) ? bytes
                                                                                                             : 0;
}
} // namespace

void game_detection_scan(const RegisteredGames& custom, const DetectableList& list, GameScan* scan) noexcept {
    scan->count = scan->foreground_pid = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    const DWORD self = process_id();
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    u32 used = 0;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && scan->count < kGameScanProcesses;
         more = Process32NextW(snapshot, &entry)) {
        char file[kRegisteredGameExecutableBytes];
        const u32 file_bytes = entry.th32ProcessID == self || entry.th32ProcessID <= 4 ? 0 : file_name(entry, file);
        const auto candidate = file_bytes ? game_detection_candidate(custom, list, {file, file_bytes})
                                          : GameCandidate{};
        if (!candidate.named)
            continue;
        // A process MaiCord may not query keeps its file name.
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        char path[1024 * 3];
        const u32 path_bytes = image_path(process, path, sizeof(path));
        add_process(scan, &used, entry.th32ProcessID, path_bytes ? Utf8View{path, path_bytes} : Utf8View{file, file_bytes},
                    candidate.arguments ? command_line(process) : Utf8View{}, list);
        if (process)
            CloseHandle(process);
    }
    CloseHandle(snapshot);
    if (!scan->count)
        return;
    WindowOwners owners; // only the first `count` pids are read
    owners.count = 0;
    owners.titled = false;
    EnumWindows(collect_window, reinterpret_cast<LPARAM>(&owners));
    for (u32 i = 0; i < scan->count; ++i)
        scan->processes[i].windowed = owns_window(owners, scan->processes[i].pid);
    DWORD foreground = 0;
    if (const HWND window = GetForegroundWindow())
        GetWindowThreadProcessId(window, &foreground);
    scan->foreground_pid = foreground;
}
u32 game_detection_choices(Slice<char> storage, Slice<Utf8View> paths) noexcept {
    WindowOwners owners;
    owners.count = 0;
    owners.titled = true;
    EnumWindows(collect_window, reinterpret_cast<LPARAM>(&owners));
    const DWORD self = process_id();
    u32 count = 0, used = 0;
    for (u32 i = 0; i < owners.count && count < paths.count; ++i) {
        if (owners.pids[i] == self || owners.pids[i] <= 4)
            continue;
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owners.pids[i]);
        char path[1024 * 3];
        const u32 path_bytes = image_path(process, path, sizeof(path));
        if (process)
            CloseHandle(process);
        u8 bytes = 0;
        if (!path_bytes || storage.count - used < kRegisteredGameExecutableBytes ||
            !game_detection_normalize({path, path_bytes}, storage.data + used, &bytes))
            continue;
        const Utf8View normalized{storage.data + used, bytes};
        bool listed = false;
        for (u32 k = 0; k < count && !listed; ++k)
            listed = utf8_equal(paths[k], normalized);
        if (listed)
            continue;
        paths[count++] = normalized;
        used += bytes;
    }
    return count;
}
#else
// Linux reads /proc: argv[0] names the program (Wine and Proton keep the Windows path there), and /proc/<pid>/exe
// stands in when argv[0] has no directory. Wayland tells no client which process owns a window, so every process
// counts as windowed and none as focused.
bool parse_pid(const char* name, u32* pid) noexcept {
    u32 value = 0;
    if (!*name)
        return false;
    for (; *name; ++name) {
        if (*name < '0' || *name > '9')
            return false;
        value = value * 10 + static_cast<u32>(*name - '0');
    }
    *pid = value;
    return true;
}
// argv[0] of a process, folded as game_detection_normalize folds it; 0 for kernel threads and unreadable ones.
struct ProcessName {
    char path[kRegisteredGameExecutableBytes];
    u8 bytes;
    int fd; // /proc/<pid>/cmdline, open
};
bool open_process(int proc, const char* pid, ProcessName* name) noexcept {
    char relative[32];
    u32 length = 0;
    while (pid[length] && length < 20) {
        relative[length] = pid[length];
        ++length;
    }
    copy_bytes(relative + length, "/cmdline", 9);
    const long fd = sys_openat(proc, relative, kOpenReadOnly | kOpenCloseOnExec);
    if (fd < 0)
        return false;
    name->fd = static_cast<int>(fd);
    char line[512];
    const long read = sys_read(name->fd, line, sizeof(line));
    u32 bytes = 0;
    while (read > 0 && bytes < static_cast<u32>(read) && line[bytes])
        ++bytes;
    if (!bytes || !game_detection_normalize({line, bytes}, name->path, &name->bytes)) {
        sys_close(name->fd);
        return false;
    }
    return true;
}
// /proc/<pid>/exe for a process started without a directory, when it names the same file.
u32 resolve_path(const char* pid, const ProcessName& name, char* output, u32 capacity) noexcept {
    const Utf8View argv0{name.path, name.bytes};
    if (file_of(argv0).bytes != argv0.bytes)
        return 0;
    char link[40] = "/proc/";
    u32 length = 6;
    while (*pid && length < 26)
        link[length++] = *pid++;
    copy_bytes(link + length, "/exe", 5);
    const long bytes = sys_readlink(link, output, capacity);
    if (bytes <= 0)
        return 0;
    char folded[kRegisteredGameExecutableBytes];
    u8 folded_bytes = 0;
    return game_detection_normalize({output, static_cast<u32>(bytes)}, folded, &folded_bytes) &&
                   utf8_equal(file_of({folded, folded_bytes}), argv0)
               ? static_cast<u32>(bytes)
               : 0;
}
constexpr u32 kCommandLineReadLimit = 1024 * 1024;
Utf8View command_line(int fd) noexcept {
    for (;;) {
        if (!line_room(4096))
            return {};
        const long bytes = sys_pread(fd, g_line, g_line_capacity, 0);
        if (bytes <= 0)
            return {};
        // A full buffer may have cut the line: read it again into a larger one.
        if (static_cast<u32>(bytes) == g_line_capacity && g_line_capacity < kCommandLineReadLimit &&
            line_room(g_line_capacity * 2))
            continue;
        char* line = reinterpret_cast<char*>(g_line);
        for (long i = 0; i < bytes; ++i)
            line[i] = fold(static_cast<u8>(line[i]));
        return {line, static_cast<u32>(bytes)};
    }
}
// Calls visit(pid text, ProcessName) for each readable process but this one, until it returns false.
template <typename Visit> void each_process(Visit visit) noexcept {
    const long proc = sys_open("/proc", kOpenReadOnly | kOpenDirectory);
    if (proc < 0)
        return;
    const u32 self = process_id();
    alignas(8) char records[4096];
    for (bool more = true; more;) {
        const long count = sys_getdents(static_cast<int>(proc), records, sizeof(records));
        if (count == -kErrorInterrupted)
            continue;
        if (count <= 0)
            break;
        for (long at = 0; at < count && more;) {
            const auto* record = reinterpret_cast<const Dirent64*>(records + at);
            at += record->record_length;
            const char* name = reinterpret_cast<const char*>(record) + kDirentNameOffset;
            u32 pid = 0;
            ProcessName process{};
            if (!parse_pid(name, &pid) || pid == self || !open_process(static_cast<int>(proc), name, &process))
                continue;
            more = visit(name, pid, process);
            sys_close(process.fd);
        }
    }
    sys_close(static_cast<int>(proc));
}
} // namespace

void game_detection_scan(const RegisteredGames& custom, const DetectableList& list, GameScan* scan) noexcept {
    scan->count = scan->foreground_pid = 0;
    u32 used = 0;
    each_process([&](const char* pid_text, u32 pid, const ProcessName& process) noexcept {
        const Utf8View argv0{process.path, process.bytes};
        const auto candidate = game_detection_candidate(custom, list, file_of(argv0));
        if (candidate.named) {
            char resolved[1024];
            const u32 resolved_bytes = resolve_path(pid_text, process, resolved, sizeof(resolved));
            add_process(scan, &used, pid, resolved_bytes ? Utf8View{resolved, resolved_bytes} : argv0,
                        candidate.arguments ? command_line(process.fd) : Utf8View{}, list);
        }
        return scan->count < kGameScanProcesses;
    });
}
u32 game_detection_choices(Slice<char> storage, Slice<Utf8View> paths) noexcept {
    u32 count = 0, used = 0;
    each_process([&](const char*, u32, const ProcessName& process) noexcept {
        const Utf8View path{process.path, process.bytes};
        const Utf8View file = file_of(path);
        const bool program = (file.bytes > 4 && bytes_equal(file.data + file.bytes - 4, ".exe", 4)) ||
                             (path.bytes > 6 && bytes_equal(path.data, "/home/", 6));
        bool listed = !program || storage.count - used < path.bytes;
        for (u32 k = 0; k < count && !listed; ++k)
            listed = utf8_equal(paths[k], path);
        if (!listed) {
            copy_bytes(storage.data + used, path.data, path.bytes);
            paths[count++] = {storage.data + used, path.bytes};
            used += path.bytes;
        }
        return count < paths.count;
    });
    return count;
}
#endif

void game_detection_release() noexcept {
    if (g_line)
        pages_release(g_line, g_line_capacity);
    g_line = nullptr;
    g_line_capacity = 0;
}

namespace {
bool detectables_path(wchar_t* path, u32* length, bool create) noexcept {
    const DataPathResult built = data_path(path, kAccountProfileMaximumPathCharacters, L"detectables.bin", create);
    *length = built.length;
    return built.ok();
}
// The refresh worker: one thread per refresh (once a day), joined by the UI thread.
struct Refresh {
    Thread* thread;
    UpdateHttpsTransport* https;
    volatile LONG finished;
    u64 now_ms;
    DetectableRefreshResult result;
} g_refresh;
UpdateTransportError native_exchange(void* context, const UpdateHttpRequest& request,
                                     const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
    return https_exchange(static_cast<UpdateHttpsTransport*>(context), request, options, response);
}
u32 refresh_main(void*) noexcept {
    auto& r = g_refresh;
    u32 previous_bytes = 0;
    u8* previous = detectable_image_load(&previous_bytes);
    r.result = detectable_refresh_run({native_exchange, r.https, {previous, previous_bytes}, r.now_ms});
    if (previous)
        pages_release(previous, previous_bytes);
    wchar_t path[kAccountProfileMaximumPathCharacters];
    u32 length = 0;
    // An image that cannot be saved still serves this run.
    if (r.result.image && detectables_path(path, &length, true))
        file_persistence_write_atomic(path, length, {r.result.image, r.result.bytes}, false);
    InterlockedExchange(&r.finished, 1);
    return 0;
}
void refresh_close() noexcept {
    auto& r = g_refresh;
    thread_close(r.thread);
    update_https_release(r.https);
    pages_release(r.https, sizeof(UpdateHttpsTransport));
    r.thread = nullptr;
    r.https = nullptr;
}
} // namespace

u8* detectable_image_load(u32* bytes) noexcept {
    *bytes = 0;
    wchar_t path[kAccountProfileMaximumPathCharacters];
    u32 length = 0;
    if (!detectables_path(path, &length, false))
        return nullptr;
    const FileHandle file =
        file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead | kFileShareWrite | kFileShareDelete, 0});
    if (file == INVALID_HANDLE_VALUE)
        return nullptr;
    u64 size = 0;
    u8* image = file_size(file, &size) && size && size <= kDetectableGamesMaximumBytes
                    ? static_cast<u8*>(commit_pages(static_cast<usize>(size)))
                    : nullptr;
    u32 error = 0;
    const bool read = image && file_persistence_read_complete(file, {image, static_cast<u32>(size)}, &error);
    file_close(file);
    if (image && !read) {
        pages_release(image, static_cast<usize>(size));
        image = nullptr;
    }
    *bytes = image ? static_cast<u32>(size) : 0;
    return image;
}
bool detectable_refresh_start(u64 now_ms) noexcept {
    auto& r = g_refresh;
    if (r.thread)
        return false;
    r.https = static_cast<UpdateHttpsTransport*>(commit_pages(sizeof(UpdateHttpsTransport)));
    if (!r.https || !update_https_initialize(r.https)) {
        if (r.https)
            pages_release(r.https, sizeof(UpdateHttpsTransport));
        r.https = nullptr;
        return false;
    }
    r.now_ms = now_ms;
    r.result = {};
    InterlockedExchange(&r.finished, 0);
    r.thread = thread_start(refresh_main, nullptr, 256 * 1024, nullptr);
    if (!r.thread) {
        update_https_release(r.https);
        pages_release(r.https, sizeof(UpdateHttpsTransport));
        r.https = nullptr;
    }
    return r.thread != nullptr;
}
DetectableRefreshResult detectable_refresh_take(bool* finished) noexcept {
    auto& r = g_refresh;
    *finished = r.thread && read_acquire(&r.finished) && thread_wait(r.thread, 0);
    if (!*finished)
        return {};
    refresh_close();
    const DetectableRefreshResult result = r.result;
    r.result = {};
    return result;
}
void detectable_refresh_cancel() noexcept {
    auto& r = g_refresh;
    if (!r.thread)
        return;
    update_https_cancel(r.https);
    // A worker that does not stop keeps its memory: it may still be writing to it.
    if (!thread_wait(r.thread, 5000))
        return;
    refresh_close();
    if (r.result.image)
        pages_release(r.result.image, r.result.bytes);
    r.result = {};
}

bool registered_games_load(RegisteredGames* games) noexcept {
    reset_value(games);
    wchar_t path[kAccountProfileMaximumPathCharacters];
    if (!data_path(path, kAccountProfileMaximumPathCharacters, L"games.bin").ok())
        return false;
    u8 record[kRecordBytes];
    const FileReadResult read = file_persistence_read_file(path, {record, sizeof(record)});
    return read.error == FileReadError::missing || (read.ok() && registered_games_decode({record, read.bytes}, games));
}
bool registered_games_save(const RegisteredGames& games) noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters];
    u8 record[kRecordBytes];
    const u32 bytes = registered_games_encode(games, {record, sizeof(record)});
    if (!bytes)
        return false;
    const DataPathResult built = data_path(path, kAccountProfileMaximumPathCharacters, L"games.bin", true);
    return built.ok() && file_persistence_write_atomic(path, built.length, {record, bytes}).ok();
}
} // namespace maicord
