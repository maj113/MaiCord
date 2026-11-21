#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/platform/window/placement.hpp"

namespace maicord {

// window.bin in the state directory (state_path: %LOCALAPPDATA%\MaiCord on Windows, $XDG_STATE_HOME/maicord on Linux):
// the main window's placement at the last close. It is state, not a preference: the client writes it when the window
// closes, apart from preferences.bin, so a damaged file costs only the placement.
//
// The record is 48 bytes, little-endian: the magic "MAIWINDW", the version and the record size (16 bytes), left, top,
// right, bottom (i32), the DPI (u32), the flags at 36 (bit 0: maximized; other bits zero), zero bytes 37..39, and a
// checksum of bytes 0..39 at 40.
inline constexpr u32 kWindowPlacementFileVersion = 1, kWindowPlacementRecordBytes = 48;

enum class WindowPlacementFileResult : u8 {
    ok,
    missing,    // no file: the first start, or a profile from a client before window.bin
    unreadable, // the path or the read failed
    damaged,    // the size, magic, version, checksum or a value is wrong
    unwritable, // the directory or the write failed
};

// Encode fails (false) for an invalid placement or a record that is not kWindowPlacementRecordBytes long. Decode keeps
// `*placement` unchanged when it fails.
bool window_placement_encode(const WindowPlacement& placement, Slice<u8> record) noexcept;
bool window_placement_decode(ConstSlice<u8> record, WindowPlacement* placement) noexcept;

// Load keeps `*placement` unchanged unless it returns ok. Save replaces the file atomically. Neither creates the parent
// directory.
WindowPlacementFileResult window_placement_load_file(const wchar_t* path, WindowPlacement* placement) noexcept;
WindowPlacementFileResult window_placement_save_file(const wchar_t* path, const WindowPlacement& placement) noexcept;

// The user's window.bin. Save creates the state directory when it is missing.
WindowPlacementFileResult window_placement_load(WindowPlacement* placement) noexcept;
WindowPlacementFileResult window_placement_save(const WindowPlacement& placement) noexcept;

} // namespace maicord
