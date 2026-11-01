#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// Opt-in game detection (Settings > Activity privacy). Running process names
// are matched locally against a small built-in table and the user's added
// games; process lists never leave the device. Only the matched game's name
// is published, and only while the user shares activity.
struct DetectableGame {
    Utf8View name;
    Utf8View executables; // Lowercase basenames separated by '|'.
    u64 application_id;   // Zero when unknown.
};
ConstSlice<DetectableGame> game_detection_builtin() noexcept;

inline constexpr u32 kRegisteredGameMaximum = 32;
inline constexpr u32 kRegisteredGameNameBytes = 64;
inline constexpr u32 kRegisteredGameExecutableBytes = 64;
struct RegisteredGame {
    char name[kRegisteredGameNameBytes];
    char executable[kRegisteredGameExecutableBytes]; // Lowercase basename.
    u8 name_bytes, executable_bytes;
};
struct RegisteredGames {
    RegisteredGame items[kRegisteredGameMaximum];
    u32 count;
};
// Lowercases ASCII and validates a process basename ("game.exe"); false for
// paths, empty names or names over the executable limit.
bool game_detection_normalize(Utf8View basename, char* output, u8* bytes) noexcept;
// "Stardew Valley.exe" -> "Stardew Valley": the display name of an added game.
u32 game_detection_display_name(Utf8View basename, Slice<char> output) noexcept;
bool registered_games_add(RegisteredGames*, Utf8View name, Utf8View executable) noexcept;
bool registered_games_remove(RegisteredGames*, u32 index) noexcept;

struct GameDetectionMatch {
    Utf8View name;
    u64 application_id;
    u32 index;  // Into the custom list or the built-in table.
    bool custom;
    bool found;
};
// Added games win over built-in ones; within each list the first entry that
// any running process matches is chosen, so the result is stable.
GameDetectionMatch game_detection_match(const RegisteredGames& custom, ConstSlice<DetectableGame> builtin,
                                        ConstSlice<Utf8View> processes) noexcept;

// Running processes as normalized basenames (Toolhelp32 snapshot). With
// windowed_only, only processes owning a visible top-level window are listed
// (the "Add a game" choices). Returns the name count; names borrow storage.
u32 game_detection_snapshot(Slice<char> storage, Slice<Utf8View> names, bool windowed_only) noexcept;

// %LOCALAPPDATA%\MaiCord\games.bin; a missing file loads as empty.
bool registered_games_load(RegisteredGames*) noexcept;
bool registered_games_save(const RegisteredGames&) noexcept;
// Portable record codec used by load/save (and tests).
u32 registered_games_encode(const RegisteredGames&, Slice<u8> output) noexcept;
bool registered_games_decode(ConstSlice<u8> input, RegisteredGames*) noexcept;
} // namespace maicord
