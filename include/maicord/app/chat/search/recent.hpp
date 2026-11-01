#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/core/slice.hpp"

namespace maicord {

inline constexpr u32 kSearchRecentCapacity = 5;
inline constexpr u32 kSearchRecentTextBytes = 256;
// Magic, version, count, then u16 length + text per entry, then a 32-bit FNV-1a checksum.
inline constexpr u32 kSearchRecentFileBytes = 6 + kSearchRecentCapacity * (2 + kSearchRecentTextBytes) + 4;

// Newest first, de-duplicated by exact text.
struct SearchRecentList {
    char text[kSearchRecentCapacity][kSearchRecentTextBytes];
    u16 bytes[kSearchRecentCapacity];
    u8 count;
    u32 revision;
};

// Trims surrounding spaces; rejects empty, oversized, control or invalid UTF-8 text.
bool search_recent_push(SearchRecentList*, Utf8View text) noexcept;
void search_recent_clear(SearchRecentList*) noexcept;
Utf8View search_recent_entry(const SearchRecentList&, u32 index) noexcept;
u32 search_recent_encode(const SearchRecentList&, Slice<u8> output) noexcept;
// Strict: any malformed, oversized, duplicated or checksum-failing record is rejected whole.
bool search_recent_decode(ConstSlice<u8> input, SearchRecentList*) noexcept;

// %LOCALAPPDATA%\MaiCord\accounts\search-<profile>.recent; the directory is never created here.
bool search_recent_path(u64 profile_id, wchar_t* path, u32 capacity) noexcept;
// A missing file loads an empty list; a corrupt file loads an empty list and returns false.
bool search_recent_load_file(const wchar_t* path, SearchRecentList*) noexcept;
bool search_recent_save_file(const wchar_t* path, const SearchRecentList&) noexcept;

} // namespace maicord
