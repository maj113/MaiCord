// data_path.hpp on Linux: the per-user data directory is $XDG_DATA_HOME/maicord, or $HOME/.local/share/maicord when
// XDG_DATA_HOME is unset or not absolute (the XDG base directory rule). Paths stay UTF-16 wchar_t strings (see
// maicord/sys/linux/path.hpp): the separator written is '/', and a relative path may use either separator.
//
// D19 also names a cache directory ($XDG_CACHE_HOME/maicord, else $HOME/.cache/maicord) for what can be created
// again and a state directory ($XDG_STATE_HOME/maicord, else $HOME/.local/state/maicord) for logs and state: the
// same rule and the same builder, under cache_path and state_path. Nothing is moved into them yet; until what is
// re-creatable has been classified, callers keep their files under the data directory, as on Windows everything
// lives under %LOCALAPPDATA%\MaiCord.
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"

#include "../data_path_shared.inc"

namespace maicord {
namespace {

constexpr u32 kRootCapacity = 1024;

enum class Root : u8 { data, cache, state };

bool separator(wchar_t value) noexcept {
    return value == L'/' || value == L'\\';
}

DataPathResult failure(DataPathError error, u32 native_error) noexcept {
    return {0, native_error, error};
}

// An absolute path without control characters.
bool absolute_path(const char* text) noexcept {
    if (!text || text[0] != '/') {
        return false;
    }
    for (; *text; ++text) {
        if (static_cast<u8>(*text) < 0x20) {
            return false;
        }
    }
    return true;
}

// 0 when `text` names a directory (a link followed, as mkdir and open do), -ENOTDIR when it names something else, else
// statx's negated errno.
long directory_check(const char* text) noexcept {
    Statx stat{};
    const long result = sys_statx(kAtCurrentDirectory, text, 0, kStatxType, &stat);
    if (syscall_failed(result)) {
        return result;
    }
    return (stat.mode & kModeTypeMask) == kModeDirectory ? 0 : -kErrorNotDirectory;
}

// The XDG base directory of `root` as UTF-16: its environment variable, else the $HOME-relative default. Zero when
// neither names an absolute path.
u32 xdg_root(Root root, wchar_t* output, u32 capacity) noexcept {
    const char* variable = root == Root::data ? "XDG_DATA_HOME" : root == Root::cache ? "XDG_CACHE_HOME" : "XDG_STATE_HOME";
    const char* suffix = root == Root::data ? "/.local/share" : root == Root::cache ? "/.cache" : "/.local/state";
    const char* base = environment_get(variable);
    if (absolute_path(base)) {
        return path_from_utf8(base, output, capacity);
    }
    const char* home = environment_get("HOME");
    if (!absolute_path(home)) {
        return 0;
    }
    char text[kRootCapacity];
    u32 length = 0;
    while (home[length] && length < kRootCapacity - 16) {
        text[length] = home[length];
        ++length;
    }
    if (home[length]) {
        return 0;
    }
    u32 added = 0;
    do {
        text[length++] = suffix[added];
    } while (suffix[added++]);
    return path_from_utf8(text, output, capacity);
}

} // namespace

u32 data_path_build(const wchar_t* root, wchar_t* output, u32 capacity, const wchar_t* relative) noexcept {
    if (!root || !output)
        return 0;
    u32 length = wide_length(root, capacity);
    while (length && separator(root[length - 1]))
        --length;
    if (length == 0 || length >= capacity)
        return 0;
    copy_bytes(output, root, length * static_cast<u32>(sizeof(wchar_t)));
    output[length] = 0;
    if (!wide_append(output, capacity, &length, L"/maicord"))
        return 0;
    if (relative && relative[0]) {
        const u32 base = length;
        if (!wide_append(output, capacity, &length, L"/") || !wide_append(output, capacity, &length, relative))
            return 0;
        for (u32 i = base; i < length; ++i)
            if (output[i] == L'\\')
                output[i] = L'/';
    }
    return length;
}

namespace {

DataPathResult rooted_path(Root root, wchar_t* output, u32 capacity, const wchar_t* relative,
                           bool create_parents) noexcept {
    wchar_t base[kRootCapacity];
    const u32 root_length = xdg_root(root, base, kRootCapacity);
    if (root_length == 0)
        return failure(DataPathError::path, static_cast<u32>(kErrorInvalid));
    const u32 length = data_path_build(base, output, capacity, relative);
    if (length == 0)
        return failure(DataPathError::path, static_cast<u32>(kErrorInvalid));
    if (create_parents) {
        // The directory that holds the last component, with its missing ancestors (XDG: create them as needed); the
        // path itself is the caller's file or directory. It exists in the common case: one statx says so.
        u32 last = length - 1;
        while (last > 0 && output[last] != L'/')
            --last;
        if (last > 0) {
            output[last] = 0;
            char text[kRootCapacity];
            u32 native_error = 0;
            const bool present = (path_to_utf8(output, text, kRootCapacity) != 0 && directory_check(text) == 0) ||
                                 directory_ensure(output, &native_error);
            output[last] = L'/';
            if (!present)
                return failure(DataPathError::directory, native_error);
        }
    }
    return {length, 0, DataPathError::none};
}

} // namespace

DataPathResult data_path(wchar_t* output, u32 capacity, const wchar_t* relative, bool create_parents) noexcept {
    return rooted_path(Root::data, output, capacity, relative, create_parents);
}

DataPathResult cache_path(wchar_t* output, u32 capacity, const wchar_t* relative, bool create_parents) noexcept {
    return rooted_path(Root::cache, output, capacity, relative, create_parents);
}

DataPathResult state_path(wchar_t* output, u32 capacity, const wchar_t* relative, bool create_parents) noexcept {
    return rooted_path(Root::state, output, capacity, relative, create_parents);
}

bool directory_ensure(const wchar_t* path, u32* native_error) noexcept {
    char text[kRootCapacity];
    if (path_to_utf8(path, text, kRootCapacity) == 0) {
        if (native_error)
            *native_error = static_cast<u32>(kErrorInvalid);
        return false;
    }
    long result = sys_mkdir(text, 0700);
    if (result == -kErrorNoEntry) {
        // A missing ancestor (a fresh user's $XDG_DATA_HOME, which XDG says to create as needed): make each in turn.
        for (u32 i = 1; text[i]; ++i) {
            if (text[i] != '/')
                continue;
            text[i] = 0;
            const long made = sys_mkdir(text, 0700);
            text[i] = '/';
            if (syscall_failed(made) && made != -kErrorExists) {
                result = made;
                break;
            }
        }
        if (result == -kErrorNoEntry)
            result = sys_mkdir(text, 0700);
    }
    if (result == -kErrorExists) {
        // It exists: fine when it is a directory.
        result = directory_check(text);
    }
    if (!syscall_failed(result)) {
        return true;
    }
    if (native_error)
        *native_error = static_cast<u32>(-result);
    return false;
}

u32 executable_path(wchar_t* output, u32 capacity) noexcept {
    char text[kRootCapacity];
    const long bytes = sys_readlink("/proc/self/exe", text, kRootCapacity - 1);
    if (syscall_failed(bytes) || bytes == 0) {
        return 0;
    }
    text[bytes] = 0;
    return path_from_utf8(text, output, capacity);
}

u32 environment_variable(const wchar_t* name, wchar_t* output, u32 capacity) noexcept {
    char variable[kRootCapacity];
    if (!name || !output || capacity == 0 || path_to_utf8(name, variable, sizeof(variable)) == 0) {
        return 0;
    }
    const char* value = environment_get(variable);
    if (!value || !*value) {
        return 0;
    }
    u32 bytes = 0;
    while (value[bytes]) {
        ++bytes;
    }
    const u32 length = path_from_utf8(value, output, capacity);
    if (length == 0) {
        return bytes + 1 > capacity ? bytes + 1 : capacity;
    }
    return length;
}

u32 local_data_root(wchar_t* output, u32 capacity) noexcept {
    return xdg_root(Root::data, output, capacity);
}

} // namespace maicord
