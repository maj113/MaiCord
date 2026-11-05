#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/slice.hpp"

// The path policy of the files seam (include/maicord/os/file.hpp): what shared code asks to know whether a UTF-16
// path names an absolute local location and where its last component starts, answered per system so no caller carries
// a drive letter test of its own.
//
//   * Windows: an absolute path is "X:\" (or "X:/" where the caller allows the slash) or a UNC "\\server\share" that
//     is not a "\\?\" or "\\.\" device path; the separator is the backslash.
//   * Linux: an absolute path is rooted with '/'; a UNC or drive spelling is never absolute. The file seam reads a
//     backslash as a separator too (a name that holds one cannot be made), so a path that carries one is not plain.
//
// The reserved characters of a Windows component (wildcards, quotes, the colon, device names) stay with the callers
// whose rules differ; `path_plain` is the one rule Linux paths follow.

namespace maicord {

enum class PathRoot : u8 {
    none,   // not an absolute path
    drive,  // Windows "X:\"
    unc,    // Windows "\\server\share"
    rooted, // Linux "/"
};

// PathRoot rules: what else counts as a root on Windows (Linux ignores them).
inline constexpr u32 kPathRootSlash = 1u << 0; // "X:/" as well as "X:\"
inline constexpr u32 kPathRootUnc = 1u << 1;   // "\\server\share" (five characters or more, no device path)

// The kind of root the path starts with. Only the root is looked at: the length, the components and the
// characters are the caller's checks. A null or empty path has none.
inline PathRoot path_root(ConstSlice<wchar_t> path, u32 rules = 0) noexcept {
#if defined(_WIN32)
    if (!path.data)
        return PathRoot::none;
    if (path.count >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && (path[2] == L'\\' || ((rules & kPathRootSlash) && path[2] == L'/')))
        return PathRoot::drive;
    if ((rules & kPathRootUnc) && path.count >= 5 && path[0] == L'\\' && path[1] == L'\\' && path[2] != L'?' &&
        path[2] != L'.')
        return PathRoot::unc;
    return PathRoot::none;
#else
    (void)rules;
    return path.data && path.count && path[0] == L'/' ? PathRoot::rooted : PathRoot::none;
#endif
}

// A character that separates the components of a path this system's code builds or receives: the backslash on
// Windows; on Linux '/' and also the backslash, which the file seam reads as a separator too (shared code joins with
// either spelling).
inline constexpr bool path_separator(wchar_t value) noexcept {
#if defined(_WIN32)
    return value == L'\\';
#else
    return value == L'/' || value == L'\\';
#endif
}

// The index where the last component starts: after the final separator, or 0 when there is none.
inline u32 path_leaf_start(ConstSlice<wchar_t> path) noexcept {
    u32 start = path.count;
    while (start && !path_separator(path[start - 1]))
        --start;
    return start;
}

// True when no character of the path is NUL or a control character, and, on Linux, none is a backslash. On Windows
// this is the control-character half of every caller's own list.
inline bool path_plain(ConstSlice<wchar_t> path) noexcept {
    for (u32 i = 0; i < path.count; ++i) {
        const wchar_t value = path[i];
        if (value < 32)
            return false;
#if !defined(_WIN32)
        if (value == L'\\')
            return false;
#endif
    }
    return true;
}

} // namespace maicord
