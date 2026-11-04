#pragma once

#include "maicord/platform/font/system_font_locator.hpp"

namespace maicord::font_locator {

// The locator's own index of the installed fonts, used when fontconfig cannot be loaded (font_scan.cpp). The index
// is built by the first call: the font directories are walked and every face's family, weight and slant are read
// from the file with the text engine's own parser.
bool scan_find(SystemFontLocator* locator, const SystemFontRequest& request, SystemFontFile* output) noexcept;
u32 scan_role(SystemFontLocator* locator, SystemFontRole role, SystemFontName* output) noexcept;
void scan_release(SystemFontLocator* locator) noexcept;

} // namespace maicord::font_locator
