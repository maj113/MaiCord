#pragma once

#include "maicord/core/shared_library.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/platform/font/fontconfig.hpp"

// The system font locator of the Linux build (the counterpart of the GDI locator in src/ui/text): it turns a family
// name, a weight and a slant into the file that holds the face and the face's index in that file, names the
// families the platform prefers for a role (interface, monospace, emoji) and the families that cover a scalar the
// requested family lacks.
//
// Lookups go to fontconfig when libfontconfig.so.1 can be loaded: the library is opened by the first lookup and
// closed by system_font_locator_idle(), which the caller invokes after a batch of lookups (the library holds about
// 3 MB and is needed for a few milliseconds a session; the results are the caller's to cache). Without fontconfig
// the locator scans the font directories itself, the ones fontconfig's own configuration names (<dir> entries of
// fonts.conf, which is how NixOS lists its store paths) and the usual ones, and matches by the family names read from
// the font files. Nothing here allocates from a heap: the scan index lives in pages the locator maps.
//
// Not thread safe: one owner (the text catalog) calls it.

namespace maicord {

inline constexpr u32 kSystemFontPathCapacity = 1024;
inline constexpr u32 kSystemFontNameCapacity = 64;

enum class SystemFontRole : u8 {
    ui,
    serif,
    monospace,
    cursive,
    emoji,
    count,
};

// A family name in UTF-16 (no terminator).
struct SystemFontName {
    u16 units[kSystemFontNameCapacity];
    u32 count;
};

struct SystemFontRequest {
    ConstSlice<u16> family;
    u16 weight; // 1 to 1000, as OpenType
    bool italic;
};

struct SystemFontFile {
    wchar_t path[kSystemFontPathCapacity];
    u32 index; // the face's index inside a collection, 0 for a single face
};

struct SystemFontScan;

struct SystemFontLocator {
    SharedLibrary library;
    fontconfig::Api api;
    SystemFontScan* scan;
    // 0: fontconfig not tried yet, 1: open, 2: closed after use (a lookup opens it again), 3: unavailable.
    u8 state;
    bool scan_failed;
};

// The file and face of the best face of exactly the family `request.family` (compared ignoring case, against every
// name the family is known by). False when the family is not installed, so a missing family never silently becomes
// another one: the caller falls back by its own policy.
bool system_font_locator_find(SystemFontLocator* locator, const SystemFontRequest& request,
                              SystemFontFile* output) noexcept;

// The installed family the platform prefers for `role`, as a name system_font_locator_find() resolves. Zero when
// the platform names none; `output` is written only on success.
u32 system_font_locator_role(SystemFontLocator* locator, SystemFontRole role, SystemFontName* output) noexcept;

// At most `capacity` installed families that cover `scalar`, best first: the order fontconfig sorts the system's
// fonts for the user's language when it is available. Zero from the scan, which has no coverage information.
u32 system_font_locator_fallbacks(SystemFontLocator* locator, u32 scalar, SystemFontName* output,
                                  u32 capacity) noexcept;

// Closes fontconfig (and with it the memory it took). The next lookup opens it again.
void system_font_locator_idle(SystemFontLocator* locator) noexcept;

// Closes everything. The locator may be initialized again by zeroing it.
void system_font_locator_release(SystemFontLocator* locator) noexcept;

// The user's language as fontconfig spells it ("en", "ja", "zh-cn", "zh-tw", "de"), from LC_ALL, LC_MESSAGES and
// LANG; "en" when none names one. Returns the length. The same value picks the CJK order of the fallback table.
u32 system_font_language(char (&output)[16]) noexcept;

} // namespace maicord
