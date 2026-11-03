#pragma once
// Shared by the plugin runtime tests: C string lengths and comparisons, paths, a temporary base directory with
// plugins\ and data\ folders, and the copy of a built fixture DLL (they sit beside the test executable).
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::plugin_fixture {

inline u32 length(const char* text) noexcept {
    u32 count = 0;
    while (text[count]) ++count;
    return count;
}
inline u32 length(const wchar_t* text) noexcept {
    u32 count = 0;
    while (text[count]) ++count;
    return count;
}
inline bool same(Utf8View text, const char* expected) noexcept {
    const u32 count = length(expected);
    return text.bytes == count && bytes_equal(text.data, expected, count);
}
inline void join(wchar_t (&output)[MAX_PATH], const wchar_t* directory, const wchar_t* name) noexcept {
    u32 at = 0;
    for (; directory[at]; ++at) output[at] = directory[at];
    output[at++] = L'\\';
    for (u32 i = 0; name[i]; ++i) output[at++] = name[i];
    output[at] = 0;
}
inline bool exists(const wchar_t* path) noexcept {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}
// The directory of the running test executable.
inline bool test_directory(wchar_t (&output)[MAX_PATH]) noexcept {
    const DWORD count = GetModuleFileNameW(nullptr, output, MAX_PATH);
    if (!count || count >= MAX_PATH) return false;
    u32 at = count;
    while (at && output[at - 1] != L'\\') --at;
    if (!at) return false;
    output[at - 1] = 0;
    return true;
}
inline void remove_tree(const wchar_t* directory) noexcept {
    wchar_t pattern[MAX_PATH];
    join(pattern, directory, L"*");
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW(pattern, &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (found.cFileName[0] == L'.' && (!found.cFileName[1] || (found.cFileName[1] == L'.' && !found.cFileName[2])))
                continue;
            wchar_t child[MAX_PATH];
            join(child, directory, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else DeleteFileW(child);
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    RemoveDirectoryW(directory);
}

struct Root {
    wchar_t base[MAX_PATH], plugins[MAX_PATH], data[MAX_PATH], marker[MAX_PATH];
};
// The folders of a root at base.
inline void make_paths(Root* root, const wchar_t* base) noexcept {
    u32 at = 0;
    for (; base[at] && at < MAX_PATH - 1; ++at) root->base[at] = base[at];
    root->base[at] = 0;
    join(root->plugins, root->base, L"plugins");
    join(root->data, root->base, L"data");
    join(root->marker, root->base, L"marker.bin");
}
// A new temporary directory (named from the three-letter prefix) holding empty plugins and data folders.
inline bool make_root(Root* root, const wchar_t* prefix) noexcept {
    wchar_t temp[MAX_PATH], path[MAX_PATH];
    const DWORD count = GetTempPathW(MAX_PATH, temp);
    if (!count || count >= MAX_PATH || !GetTempFileNameW(temp, prefix, 0, path) || !DeleteFileW(path) ||
        !CreateDirectoryW(path, nullptr))
        return false;
    make_paths(root, path);
    return CreateDirectoryW(root->plugins, nullptr) && CreateDirectoryW(root->data, nullptr);
}
// Copies the built fixture into the root's plugins folder under name.
inline bool install(const Root& root, const wchar_t* built, const wchar_t* name) noexcept {
    wchar_t directory[MAX_PATH], source[MAX_PATH], destination[MAX_PATH];
    if (!test_directory(directory)) return false;
    join(source, directory, built);
    join(destination, root.plugins, name);
    return CopyFileW(source, destination, FALSE) != 0;
}

} // namespace maicord::plugin_fixture
