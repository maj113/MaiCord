#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
struct UpdateHttpRequest;
struct UpdateHttpResponse;
struct HttpsExchangeOptions;
enum class UpdateTransportError : u8;

// Opt-in game detection (Settings > Registered Games). Running processes are matched locally against Discord's
// detectable list, which the client fetches as the official one does, and the user's added games; process lists
// never leave the device. Only the shown game's name and application id are published, and only while the user
// shares activity.
//
// Executables are '|'-separated entries, each a lowercase path suffix with '/' separators ("win64/tslgame.exe")
// that names a process when it ends the process's image path at a segment boundary. ">name" names the file alone;
// "name?text" also needs `text` in the command line (Discord's `arguments`); a leading '*' marks a launcher
// (`is_launcher`), recognized but never shown as played.

// The detectable list as MaiCord keeps it: one image built on a worker from Discord's JSON
// (cdn.discordapp.com/detectables/games-v1.json and GET /games/detectable/exclusions), saved as detectables.bin and
// read in place by scans. After the header: the game rows, the keys sorted by (hash, game), then text: each game's
// name and executable list, then the exclusion executables and the exclusion patterns ('|'-separated).
inline constexpr u32 kDetectableEtagBytes = 96;
struct DetectableImageHeader {
    u64 games_checked_ms, exclusions_checked_ms; // Unix ms of each list's last answer (200 or 304)
    u8 magic[4];
    u32 bytes, game_count, key_count, text_bytes, games_text_bytes, exclusions_bytes, patterns_bytes;
    u8 games_etag_bytes, exclusions_etag_bytes;
    char games_etag[kDetectableEtagBytes], exclusions_etag[kDetectableEtagBytes];
};
static_assert(sizeof(DetectableImageHeader) == 248);
struct DetectableGameRow {
    u64 application_id;
    u32 name, executables; // text offsets
    u16 name_bytes, executables_bytes;
    u32 reserved;
};
// One per executable file name of a game: the FNV-1a hash of the name (the last path segment).
struct DetectableKey {
    u32 hash, game;
};
struct DetectableList {
    const DetectableImageHeader* header; // null: no list yet
    const DetectableGameRow* games;
    const DetectableKey* keys;
    const char* text;
};
// Validates an image (sizes, offsets, key order) and views it in place; false leaves *list empty.
bool detectable_list_view(ConstSlice<u8> image, DetectableList* list) noexcept;
struct DetectableGame {
    Utf8View name, executables;
    u64 application_id;
};
DetectableGame detectable_list_game(const DetectableList&, u32 index) noexcept;
// The key hash of an executable file name (FNV-1a over its bytes).
u32 detectable_hash(Utf8View file) noexcept;
// Discord's exclusion patterns are regular expressions; MaiCord reads the subset of literal characters, escaped
// punctuation, `.*`, a leading `^` and a trailing `$` (the build drops any other). A match is case-sensitive:
// paths and patterns are lowercase.
bool detectable_pattern_supported(Utf8View pattern) noexcept;
bool detectable_pattern_match(Utf8View pattern, Utf8View text) noexcept;

// One image's sources: a fresh JSON body, or (empty) the previous image's part, which a 304 keeps.
struct DetectableBuild {
    Utf8View games_json, exclusions_json;
    DetectableList previous;
    Utf8View games_etag, exclusions_etag;
    u64 games_checked_ms, exclusions_checked_ms;
};
// Room for the larger JSON's structural index and for the rows, keys and text before they are packed.
struct DetectableBuildScratch {
    Slice<u32> structurals;
    Slice<DetectableGameRow> rows;
    Slice<DetectableKey> keys;
    Slice<char> text;
};
// Keeps a game's "win32" and "linux" executables (Wine and Proton run the Windows ones) and drops games without
// any. Returns the image bytes written to `image`, 0 when a JSON body is malformed or something does not fit.
u32 detectable_list_build(const DetectableBuild&, const DetectableBuildScratch&, Slice<u8> image) noexcept;

// The refresh both lists get, the official client's: If-None-Match with the saved ETag, kept a day.
inline constexpr u64 kDetectableRefreshMs = 24ull * 60 * 60 * 1000;
inline constexpr u32 kDetectableGamesMaximumBytes = 64u * 1024 * 1024;
inline constexpr u32 kDetectableExclusionsMaximumBytes = 64u * 1024;
using DetectableExchange = UpdateTransportError (*)(void* context, const UpdateHttpRequest&,
                                                    const HttpsExchangeOptions&, UpdateHttpResponse*) noexcept;
struct DetectableRefresh {
    DetectableExchange exchange;
    void* context;
    ConstSlice<u8> previous; // the saved image, or empty
    u64 now_ms;              // Unix ms
};
// Fetches both lists and builds the next image into its own pages (free with pages_release). Null when the games
// list neither answered nor was saved before, or the build failed; a list that fails keeps its saved part.
struct DetectableRefreshResult {
    u8* image;
    u32 bytes;
};
DetectableRefreshResult detectable_refresh_run(const DetectableRefresh&) noexcept;

// A running process as a scan sees it. `path` is the image path and `arguments` the command line (read only
// for processes an entry with arguments names), both lowercase ASCII with '/' for '\\'. `windowed`: the process
// owns a visible top-level window; a scan that cannot tell (Wayland) sets it.
struct GameProcess {
    Utf8View path, arguments;
    u32 pid;
    bool windowed;
};
// True when an entry of `executables` names the process; *launcher reports that only launcher entries did.
bool game_executables_match(Utf8View executables, const GameProcess& process, bool* launcher) noexcept;
// Whether any game of the list (launchers included) names the process.
bool detectable_list_names(const DetectableList&, const GameProcess& process) noexcept;
// Discord's executable exclusions (installers, crash reporters, generic launchers): an executable name anywhere in
// the path, or a pattern (the regular expressions Discord sends, in the subset of literals, `.*`, `^` and `$`).
// They keep a program out of "Add it!" and stop an added one showing.
bool game_detection_blocked(const DetectableList&, Utf8View path) noexcept;
// OBS, XSplit, Bebo, Streamlabs and Twitch Studio: the official client's streamer tools.
bool game_detection_streamer_tool(const GameProcess& process) noexcept;

inline constexpr u32 kRegisteredGameMaximum = 32;
inline constexpr u32 kRegisteredGameNameBytes = 128;
inline constexpr u32 kRegisteredGameExecutableBytes = 255;
inline constexpr u32 kRegisteredGameNone = ~0u;
enum RegisteredGameFlag : u8 {
    registered_game_added = 1,         // A program the user added: its executable names it.
    registered_game_detection_off = 2, // "Toggle detection" is off: never shown.
};
// Discord's games seen and overrides in one list: added programs, and detected games once shown.
struct RegisteredGame {
    u64 application_id; // A detected game's; zero for an added program.
    u64 last_played_ms; // Unix ms; zero before it is first shown.
    char name[kRegisteredGameNameBytes];
    char executable[kRegisteredGameExecutableBytes]; // An added program's normalized image path suffix.
    u8 name_bytes, executable_bytes, flags;
};
struct RegisteredGames {
    RegisteredGame items[kRegisteredGameMaximum];
    u32 count;
};
// Lowercases ASCII and turns '\\' into '/'; a path over the limit keeps its last whole segments. False for an
// empty or invalid path.
bool game_detection_normalize(Utf8View path, char* output, u8* bytes) noexcept;
// "C:\Games\stardew valley.exe" -> "Stardew valley": the display name of an added program.
u32 game_detection_display_name(Utf8View path, Slice<char> output) noexcept;
bool registered_games_add(RegisteredGames*, Utf8View name, Utf8View path) noexcept;
bool registered_games_remove(RegisteredGames*, u32 index) noexcept;
// The entry of a detected game, created when it is first shown; when the list is full the detected game played
// longest ago gives way (added programs and games turned off stay). kRegisteredGameNone when nothing can.
u32 registered_games_seen(RegisteredGames*, Utf8View name, u64 application_id) noexcept;

struct GameDetectionMatch {
    Utf8View name;
    u64 application_id;
    u32 pid;
    u32 index; // Into the added list (custom) or the detectable list.
    bool custom, found;
};
// The game a scan shows. Launchers, windowless processes, games turned off and blocked added programs never
// show. The process owning the foreground window wins, then the game shown before (`previous_pid`), then added
// programs before listed games, each in list order.
GameDetectionMatch game_detection_select(const RegisteredGames& custom, const DetectableList& list,
                                         ConstSlice<GameProcess> processes, u32 previous_pid,
                                         u32 foreground_pid) noexcept;
// Whether a process with this file name (lowercase) could be named by an added program, a listed game or a
// streamer tool, and whether naming it needs its command line: the scan reads paths for these alone.
struct GameCandidate {
    bool named, arguments;
};
GameCandidate game_detection_candidate(const RegisteredGames& custom, const DetectableList& list,
                                       Utf8View file) noexcept;
// What a scan keeps of a command line (lowercase, any length): the argument texts of the entries naming `file`
// that it contains, each followed by '\n'. Matching against it is the same as against the whole line.
u32 game_detection_arguments(const DetectableList& list, Utf8View file, Utf8View command_line,
                              Slice<char> output) noexcept;

// One scan (src/app/account/game_detection_system.cpp): the candidate processes, with paths, command lines where
// needed and window ownership resolved, and the process owning the foreground window (0 when unknown). Nothing
// is allocated but the command-line buffer, kept at the longest line read until game_detection_release.
inline constexpr u32 kGameScanProcesses = 16;
struct GameScan {
    GameProcess processes[kGameScanProcesses];
    u32 count, foreground_pid;
    char text[kGameScanProcesses * 512];
};
void game_detection_scan(const RegisteredGames& custom, const DetectableList& list, GameScan* scan) noexcept;
void game_detection_release() noexcept;
// Programs to offer in "Add it!": image paths of processes owning a visible titled top-level window (Wayland: Wine
// processes and programs under /home), without duplicates. Paths borrow storage.
u32 game_detection_choices(Slice<char> storage, Slice<Utf8View> paths) noexcept;

// detectables.bin in the app data folder, into its own pages (free with pages_release); null when missing.
u8* detectable_image_load(u32* bytes) noexcept;
// The refresh worker: one thread fetching both lists over HTTPS (detectable_refresh_run) and saving the image
// atomically. start is false while one runs or when it cannot start; take returns its image once it finished
// (joining it; *finished reports that it did, the image null when it failed); cancel stops and joins it.
bool detectable_refresh_start(u64 now_ms) noexcept;
DetectableRefreshResult detectable_refresh_take(bool* finished) noexcept;
void detectable_refresh_cancel() noexcept;

// %LOCALAPPDATA%\MaiCord\games.bin; a missing file loads as empty.
bool registered_games_load(RegisteredGames*) noexcept;
bool registered_games_save(const RegisteredGames&) noexcept;
// Portable record codec used by load/save (and tests); decodes the older added-programs-only record too.
u32 registered_games_encode(const RegisteredGames&, Slice<u8> output) noexcept;
bool registered_games_decode(ConstSlice<u8> input, RegisteredGames*) noexcept;
} // namespace maicord
