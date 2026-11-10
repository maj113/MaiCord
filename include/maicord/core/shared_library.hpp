#pragma once

#include "maicord/core/base.hpp"

// System libraries loaded at run time (Linux; Windows images use delay-loaded imports and LoadLibrary). A Linux image
// links only libc; every other library (libwayland-client, libEGL, libxkbcommon, ...) is
// opened by soname through this helper, so a missing optional library is an error value, not a loader failure.

namespace maicord {

struct SharedLibrary {
    void* handle;
};

// Opens the library `name` (a soname such as "libEGL.so.1", or a path). Tries, in order: the directories of
// MAICORD_LIBRARY_PATH (colon separated), the loader's own search (LD_LIBRARY_PATH, the cache, the default
// directories), then the fixed fallbacks for systems whose loader finds nothing by itself: /run/opengl-driver/lib,
// /run/current-system/sw/share/nix-ld/lib and /run/current-system/sw/lib (NixOS), then the usual multiarch and
// lib64 directories. False when none loads it; shared_library_error() then describes the last failure.
bool shared_library_open(SharedLibrary* library, const char* name) noexcept;

// The address of `name` in the library, or null.
void* shared_library_symbol(const SharedLibrary& library, const char* name) noexcept;

void shared_library_close(SharedLibrary* library) noexcept;

// Fills a table of function pointers from `library`: `table` is a struct of nothing but function pointers
// (`table_bytes` long) and `names` the symbol of each in member order, NUL-terminated names one after another with an
// empty one after the last (an X-macro list of `#name "\0"` makes them). The first symbol the library lacks ends the
// load and is returned (the later members stay as they were); null when every member is filled. One loop serves every
// table, where a call per member cost 36 bytes each.
const char* shared_library_resolve(const SharedLibrary& library, void* table, usize table_bytes,
                                   const char* names) noexcept;
// The warning for a symbol a library lacks: "<soname>: no symbol <name>", under `component`.
void shared_library_log_missing(const char* component, const char* soname, const char* symbol) noexcept;

// The number of names in such a pool, for a static_assert that it has one name per member of its table.
template <usize N> constexpr usize shared_library_name_count(const char (&names)[N]) noexcept {
    usize count = 0;
    for (usize index = 0; index < N; ++index)
        count += names[index] == 0;
    return count - 1;
}

// The loader's message for the last failed open or lookup on this thread, or null.
const char* shared_library_error() noexcept;

} // namespace maicord
