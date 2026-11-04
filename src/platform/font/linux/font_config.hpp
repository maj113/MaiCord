#pragma once

#include "maicord/core/arena.hpp"
#include "maicord/platform/font/system_font_locator.hpp"

namespace maicord::font_locator {

inline constexpr u32 kFontRootCapacity = 64;

// The directories fonts are installed in, as UTF-16 paths in the arena that is passed in.
struct FontRoots {
    const wchar_t* paths[kFontRootCapacity];
    u32 count;
};

// Reads the fontconfig configuration ($FONTCONFIG_FILE, the files in $FONTCONFIG_PATH, else /etc/fonts/fonts.conf)
// and follows its <include> entries, collecting its <dir> entries: that is the only place NixOS names its fonts,
// store paths and profile directories that no fixed list could guess. A <dir prefix="xdg"> is under
// $XDG_DATA_HOME, "~" is the home directory and prefix="relative" is relative to the including file. When no
// configuration can be read, the roots are the usual ones: /usr/share/fonts, /usr/local/share/fonts, the user's
// fonts directory, every $XDG_DATA_DIRS/fonts and the NixOS profile directories. Never fails; the list may be empty.
void font_roots_collect(Arena* arena, FontRoots* roots) noexcept;

} // namespace maicord::font_locator
