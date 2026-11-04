#pragma once

#include "maicord/core/slice.hpp"

// The families the interface uses when the user has not chosen one: the platform's, not a constant. Windows keeps
// its fixed ones (Segoe UI and the others, named where they were always written); on Linux they are resolved once,
// at the first use, as installed families the text catalog can load:
//
//   interface     the portal's or the desktop's interface font when it is readable and installed (GNOME's
//                 org.gnome.desktop.interface font-name in the user's dconf database, GTK's settings.ini, KDE's
//                 kdeglobals), else fontconfig's sans-serif, else the first of a built-in list (Inter, Noto Sans, ...)
//   monospace     the desktop's monospace font on the same terms, else fontconfig's monospace
//   serif         fontconfig's serif
//   emoji         Noto Color Emoji (as installed), found through fontconfig's emoji family
//
// The roles Windows names with a specific typeface (Bahnschrift, Arial Rounded MT Bold, Segoe Print) map to the
// closest generic family: display and rounded to the interface font, handwriting to fontconfig's cursive, or the
// interface font where that has no installed family.

#if !defined(_WIN32)

namespace maicord {

enum class DefaultFont : u8 {
    ui,
    display,
    serif,
    rounded,
    monospace,
    handwriting,
    emoji,
    count,
};

// The family name of `role` as a NUL-terminated UTF-16 string that stays valid for the life of the process. The
// first call resolves every role (a few milliseconds, with fontconfig opened and closed once); the call is safe from
// any thread.
const wchar_t* default_font_family(DefaultFont role) noexcept;

// The same name as a counted UTF-16 view.
ConstSlice<u16> default_font_name(DefaultFont role) noexcept;

// Supplies the interface and monospace fonts the desktop settings portal reports (org.gnome.desktop.interface
// font-name and monospace-font-name; a GTK font description such as "Cantarell 11", UTF-8), which take precedence
// over what the configuration files say. Only effective before the first default_font_family call; false after it.
bool default_fonts_set_portal(const char* interface_font, const char* monospace_font) noexcept;

} // namespace maicord

#endif
