#pragma once
#include "maicord/platform/file_picker.hpp"
#include "maicord/core/memory.hpp"
#include <Windows.h>
#include <shellapi.h>
#include <shlobj.h>
namespace maicord::file_drop_fixture {
inline HDROP make(ConstSlice<ConstSlice<wchar_t>> paths) noexcept {
    usize bytes = sizeof(DROPFILES) + sizeof(wchar_t);
    for (u32 i = 0; i < paths.count; ++i) bytes += (static_cast<usize>(paths[i].count) + 1) * sizeof(wchar_t);
    const HGLOBAL block = GlobalAlloc(GHND, bytes);
    auto* header = block ? static_cast<DROPFILES*>(GlobalLock(block)) : nullptr;
    if (!header) { if (block) GlobalFree(block); return nullptr; }
    header->pFiles = sizeof(DROPFILES); header->fWide = TRUE;
    auto* text = reinterpret_cast<wchar_t*>(header + 1);
    for (u32 i = 0; i < paths.count; ++i) {
        const auto path = paths[i];
        copy_bytes(text, path.data, path.count * sizeof(wchar_t)); text += path.count + 1;
    }
    GlobalUnlock(block);
    return reinterpret_cast<HDROP>(block);
}
inline bool seed(wchar_t* path, u32* units, FileSnapshot* snapshot) noexcept {
    wchar_t directory[MAX_PATH]{};
    const u32 directory_units = GetTempPathW(MAX_PATH, directory);
    if (!directory_units || directory_units >= MAX_PATH || !GetTempFileNameW(directory, L"mcd", 0, path)) return false;
    const HANDLE file = CreateFileW(path, GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    bool ok = file != INVALID_HANDLE_VALUE;
    if (ok) {
        DWORD written = 0;
        ok = WriteFile(file, "dropdata", 8, &written, nullptr) && written == 8 && file_snapshot_read(file, snapshot);
        CloseHandle(file);
    }
    *units = 0; while (path[*units]) ++*units;
    return ok;
}
inline bool await(NativeFilePicker* picker) noexcept {
    return picker->notification && WaitForSingleObject(picker->notification, 10000) == WAIT_OBJECT_0;
}
inline bool reap(NativeFilePicker* picker) noexcept {
    // Release joins the worker thread; a failed bounded join still preserves worker storage.
    const bool released = native_file_picker_release(picker, 10000);
    if (!released) native_file_picker_release(picker, INFINITE);
    return released;
}
}
