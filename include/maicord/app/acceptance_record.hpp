#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {

// Durable fixed-size records: the updater's and the plugin catalog's
// acceptance floors (update_acceptance, plugin_index_acceptance) and the
// plugin manager's providers, state and journal files share these file
// rules and this error set.
//
// Paths are trusted LOCAL drive paths ("C:\..."), count excluding the NUL:
// no device, UNC, ADS, dot, trailing-dot/space or reserved-name components.
// Every ancestor must exist as a directory that is not a reparse point on a
// fixed drive. These checks do not secure writable ancestors against
// replacement races; parents must be protected.
inline constexpr u32 kAcceptanceRecordMaximumPathCharacters = 1023;

enum class AcceptanceRecordError : u8 {
    // The updater's UpdateAcceptanceError uses the same ordinals up to release.
    none, invalid_input, path, lock, missing, open, file, size, read,
    format, checksum, state, document, write, flush, commit, release,
    version,  // a record version this client does not read
    capacity, // no free slot or entry
    replay,   // an offer below the persisted floor, or the floor's sequence with another digest
};
struct AcceptanceRecordResult {
    AcceptanceRecordError error;
    DWORD native_error;
    constexpr bool ok() const noexcept { return error == AcceptanceRecordError::none; }
};

// A Windows reserved device name (CON, PRN, AUX, NUL, COM0-9, LPT0-9) before any extension.
bool acceptance_record_reserved_name(const wchar_t* name, u32 count) noexcept;
// The path rules above, leaving room for the 5-character ".lock" sibling.
bool acceptance_record_path_valid(const ConstSlice<wchar_t>& path) noexcept;
// Copies path and appends a NUL (output holds count + 1 characters).
void acceptance_record_terminate(const ConstSlice<wchar_t>& path, wchar_t* output) noexcept;
// Copies the first count characters of path followed by the NUL-terminated ending.
void acceptance_record_sibling(const wchar_t* path, u32 count, const wchar_t* ending, wchar_t* output) noexcept;
// Fixed drive, and every ancestor of the NUL-terminated path an existing
// directory without a reparse point (`path`). path is restored before return.
AcceptanceRecordResult acceptance_record_ancestors(wchar_t* path, u32 count) noexcept;
// An open handle on a regular disk file, not a directory or reparse point (`file`).
AcceptanceRecordResult acceptance_record_disk_file(HANDLE file) noexcept;
// Reads exactly record.count bytes from the regular file at the NUL-terminated
// path without following a final reparse point: `missing` when the file does
// not exist, `size` for any other length. record holds the bytes only on success.
AcceptanceRecordResult acceptance_record_read(const wchar_t* path, Slice<u8> record) noexcept;
// Writes a CREATE_NEW write-through "<path>.new" sibling (never truncating or
// following a stale one), checks write, flush and close, then replaces path
// with MOVEFILE_WRITE_THROUGH. On failure only that owned sibling is deleted.
AcceptanceRecordResult acceptance_record_write(const wchar_t* path, u32 count, ConstSlice<u8> record) noexcept;
// Opens the exclusive "<path>.lock" sibling (created when absent) that
// serializes cooperating processes for a whole verify/commit (`lock`), and
// checks it is a regular disk file (`file`, the handle then closed).
AcceptanceRecordResult acceptance_record_lock(const wchar_t* path, u32 count, HANDLE* lock) noexcept;

} // namespace maicord
