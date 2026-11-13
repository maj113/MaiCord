#include "maicord/core/shared_library.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/sys/linux/libc.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord {
namespace {

constexpr u32 kPathCapacity = 512;

// Directories searched after the loader's own lookup. The NixOS ones come first: its loader has no default
// directories, and /run/opengl-driver/lib is where the GPU drivers (Mesa, the WSL libraries) live.
constexpr const char* kFallbackDirectories[] = {
    "/run/opengl-driver/lib",
    "/run/current-system/sw/share/nix-ld/lib",
    "/run/current-system/sw/lib",
    "/usr/lib/x86_64-linux-gnu",
    "/usr/lib64",
    "/usr/lib",
    "/lib/x86_64-linux-gnu",
    "/lib64",
    "/lib",
};

bool contains_slash(const char* text) noexcept {
    for (; *text; ++text) {
        if (*text == '/') {
            return true;
        }
    }
    return false;
}

// Opens directory/name; the directory is `length` bytes, not NUL-terminated.
void* open_in(const char* directory, u32 length, const char* name, u32 name_length) noexcept {
    char path[kPathCapacity];
    if (length == 0 || length + 1 + name_length + 1 > kPathCapacity) {
        return nullptr;
    }
    copy_bytes(path, directory, length);
    path[length] = '/';
    copy_bytes(path + length + 1, name, name_length);
    path[length + 1 + name_length] = 0;
    return dlopen(path, kDlNow | kDlLocal);
}

} // namespace

bool shared_library_open(SharedLibrary* library, const char* name) noexcept {
    if (!library || !name || !*name) {
        return false;
    }
    library->handle = nullptr;
    if (contains_slash(name)) {
        library->handle = dlopen(name, kDlNow | kDlLocal);
        return library->handle != nullptr;
    }
    const u32 name_length = c_string_length(name);
    const char* search = environment_get("MAICORD_LIBRARY_PATH");
    while (search && *search && !library->handle) {
        const char* end = search;
        while (*end && *end != ':') {
            ++end;
        }
        library->handle = open_in(search, static_cast<u32>(end - search), name, name_length);
        search = *end ? end + 1 : end;
    }
    if (!library->handle) {
        library->handle = dlopen(name, kDlNow | kDlLocal);
    }
    for (const char* directory : kFallbackDirectories) {
        if (library->handle) {
            break;
        }
        library->handle = open_in(directory, c_string_length(directory), name, name_length);
    }
    return library->handle != nullptr;
}

const char* shared_library_resolve(const SharedLibrary& library, void* table, usize table_bytes,
                                   const char* names) noexcept {
    for (usize offset = 0; offset < table_bytes; offset += sizeof(void*)) {
        void* const address = dlsym(library.handle, names);
        if (!address)
            return names;
        copy_bytes(static_cast<u8*>(table) + offset, &address, sizeof(address));
        names += c_string_length(names) + 1;
    }
    return nullptr;
}

void shared_library_log_missing(const char* component, const char* soname, const char* symbol) noexcept {
    char message[128];
    u32 length = 0;
    const char* const parts[] = {soname, ": no symbol ", symbol};
    for (const char* part : parts)
        for (; *part && length + 1 < sizeof(message); ++part)
            message[length++] = *part;
    message[length] = 0;
    log_write(LogLevel::warning, component, message);
}

void shared_library_close(SharedLibrary* library) noexcept {
    if (library && library->handle) {
        dlclose(library->handle);
        library->handle = nullptr;
    }
}

const char* shared_library_error() noexcept {
    return dlerror();
}

} // namespace maicord
