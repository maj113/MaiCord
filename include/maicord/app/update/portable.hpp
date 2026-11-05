#pragma once

#include "maicord/core/slice.hpp"

#if !defined(_WIN32)

// The Linux self-updating channel (packaging/README.md): only the tarball install carries the marker file
// `.maicord-portable` beside its executable, and only that install may replace its own files. Every package-manager
// install (AUR, Nix, Flatpak, .deb, .rpm) leaves it out, because a program that replaced itself under its package would
// break the package's verification and the next upgrade.

namespace maicord {

inline constexpr char kUpdatePortableMarkerName[] = ".maicord-portable";
// The first line of a valid marker; later lines are key=value fields (a channel, say) and unknown ones are ignored.
inline constexpr char kUpdatePortableMarkerLine[] = "maicord-portable 1";

// `executable` is the real path of the running image (readlink of /proc/self/exe, never the ~/.local/bin link): the marker
// is the file of that name in its directory. Writes the path into output (terminated) and returns its length, or 0 when it
// does not fit or `executable` names no file in a directory.
u32 update_portable_marker_path(ConstSlice<wchar_t> executable, wchar_t* output, u32 capacity) noexcept;

// Whether `executable` belongs to the self-updating channel: the marker exists as a regular file whose first line is
// kUpdatePortableMarkerLine, and the directory lets this process create files (the updater stages the new image beside
// the old one and swaps them).
bool update_portable_install(ConstSlice<wchar_t> executable) noexcept;

} // namespace maicord

#endif
