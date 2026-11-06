#pragma once


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

// Resolves the functions of a table one by one, remembering the first name that is missing (and resolving no more).
struct SharedLibraryResolver {
    const SharedLibrary& library;
    const char* missing;

    template <typename Function> void operator()(Function* slot, const char* name) noexcept {
        if (missing)
            return;
        void* address = shared_library_symbol(library, name);
        if (!address) {
            missing = name;
            return;
        }
        *slot = reinterpret_cast<Function>(address);
    }
};

// The loader's message for the last failed open or lookup on this thread, or null.
const char* shared_library_error() noexcept;

} // namespace maicord
