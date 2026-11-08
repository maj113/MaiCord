// The Windows bodies of maicord/os/file.hpp: forwarders to the Win32 calls, so a call site compiles to the
// imports it used to call. Included by file.hpp only.

#include "maicord/core/win32_file_info.hpp"
#include "maicord/core/win32_process.hpp"

namespace maicord {
namespace file_detail {

constexpr DWORD windows_access(const FileOpen& options) noexcept {
    DWORD access = 0;
    switch (options.access) {
    case FileAccess::read:
        access = GENERIC_READ;
        break;
    case FileAccess::write:
        access = GENERIC_WRITE;
        break;
    case FileAccess::read_write:
        access = GENERIC_READ | GENERIC_WRITE;
        break;
    case FileAccess::append:
        access = FILE_APPEND_DATA;
        break;
    case FileAccess::attributes:
        access = FILE_READ_ATTRIBUTES;
        break;
    }
    return (options.flags & kFileDeletable) ? access | DELETE : access;
}

constexpr DWORD windows_creation(FileCreate create) noexcept {
    switch (create) {
    case FileCreate::create_new:
        return CREATE_NEW;
    case FileCreate::create_always:
        return CREATE_ALWAYS;
    case FileCreate::open_always:
        return OPEN_ALWAYS;
    case FileCreate::existing:
        break;
    }
    return OPEN_EXISTING;
}

constexpr DWORD windows_attributes(u32 flags) noexcept {
    DWORD attributes = (flags & kFileTemporary) ? FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY
                                                : FILE_ATTRIBUTE_NORMAL;
    if (flags & kFileSequential)
        attributes |= FILE_FLAG_SEQUENTIAL_SCAN;
    if (flags & kFileRandom)
        attributes |= FILE_FLAG_RANDOM_ACCESS;
    if (flags & kFileWriteThrough)
        attributes |= FILE_FLAG_WRITE_THROUGH;
    if (flags & kFileOverlapped)
        attributes |= FILE_FLAG_OVERLAPPED;
    if (flags & kFileNoFollow)
        attributes |= FILE_FLAG_OPEN_REPARSE_POINT;
    if (flags & kFileDirectory)
        attributes |= FILE_FLAG_BACKUP_SEMANTICS;
    return attributes;
}

constexpr u64 file_time_value(const FILETIME& time) noexcept {
    return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

} // namespace file_detail

__forceinline FileHandle file_open(const wchar_t* path, const FileOpen& options) noexcept {
    return CreateFileW(path, file_detail::windows_access(options), options.share, nullptr,
                       file_detail::windows_creation(options.create), file_detail::windows_attributes(options.flags),
                       nullptr);
}

__forceinline bool file_close(FileHandle file) noexcept {
    return file == nullptr || file == INVALID_HANDLE_VALUE || CloseHandle(file) != 0;
}

__forceinline bool file_read(FileHandle file, void* buffer, u32 bytes, u32* read) noexcept {
    DWORD count = 0;
    const BOOL ok = ReadFile(file, buffer, bytes, &count, nullptr);
    *read = count;
    return ok != 0;
}

inline bool file_read_at(FileHandle file, u64 offset, void* buffer, u32 bytes, u32* read) noexcept {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    *read = 0;
    return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != 0 && file_read(file, buffer, bytes, read);
}

__forceinline bool file_write(FileHandle file, const void* buffer, u32 bytes, u32* written) noexcept {
    DWORD count = 0;
    const BOOL ok = WriteFile(file, buffer, bytes, &count, nullptr);
    *written = count;
    return ok != 0;
}

inline bool file_seek(FileHandle file, i64 offset, FileSeek origin, u64* position = nullptr) noexcept {
    LARGE_INTEGER distance{};
    LARGE_INTEGER moved{};
    distance.QuadPart = offset;
    const DWORD method = origin == FileSeek::begin ? FILE_BEGIN : origin == FileSeek::current ? FILE_CURRENT : FILE_END;
    if (!SetFilePointerEx(file, distance, &moved, method))
        return false;
    if (position)
        *position = static_cast<u64>(moved.QuadPart);
    return true;
}

__forceinline bool file_size(FileHandle file, u64* bytes) noexcept {
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length))
        return false;
    *bytes = static_cast<u64>(length.QuadPart);
    return true;
}

__forceinline bool file_flush(FileHandle file) noexcept {
    return FlushFileBuffers(file) != 0;
}

inline bool file_stat(FileHandle file, FileStat* stat) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info))
        return false;
    FileStat result{};
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        result.kind = FileKind::link;
    else if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        result.kind = FileKind::directory;
    else
        result.kind = GetFileType(file) == FILE_TYPE_DISK ? FileKind::regular : FileKind::other;
    result.links = info.nNumberOfLinks;
    result.bytes = (static_cast<u64>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    result.last_write = file_detail::file_time_value(info.ftLastWriteTime);
    FILE_BASIC_INFO basic{};
    result.change = GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic))
                        ? static_cast<u64>(basic.ChangeTime.QuadPart)
                        : result.last_write;
    // File systems without 128-bit file numbers (FAT) answer with the 64-bit index of the older query.
    if (!GetFileInformationByHandleEx(file, FileIdInfo, &result.id, sizeof(result.id))) {
        result.id = {};
        result.id.VolumeSerialNumber = info.dwVolumeSerialNumber;
        const u64 index = (static_cast<u64>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        for (u32 byte = 0; byte < 8; ++byte)
            result.id.FileId.Identifier[byte] = static_cast<u8>(index >> (byte * 8));
    }
    *stat = result;
    return true;
}

inline bool file_path_stat(const wchar_t* path, FileStat* stat) noexcept {
    const FileHandle file = file_open(path, {FileAccess::attributes, FileCreate::existing,
                                             kFileShareRead | kFileShareWrite | kFileShareDelete,
                                             kFileNoFollow | kFileDirectory});
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const bool ok = file_stat(file, stat);
    const u32 error = thread_last_error();
    file_close(file);
    SetLastError(error);
    return ok;
}

__forceinline bool file_times(FileHandle file, u64* last_write, u64* change) noexcept {
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)))
        return false;
    *last_write = static_cast<u64>(basic.LastWriteTime.QuadPart);
    *change = static_cast<u64>(basic.ChangeTime.QuadPart);
    return true;
}

__forceinline bool file_replace(const wchar_t* from, const wchar_t* to, bool durable) noexcept {
    return MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0)) != 0;
}

__forceinline bool file_rename_noreplace(const wchar_t* from, const wchar_t* to, bool durable) noexcept {
    return MoveFileExW(from, to, durable ? MOVEFILE_WRITE_THROUGH : 0) != 0;
}

__forceinline bool file_remove(const wchar_t* path) noexcept {
    return DeleteFileW(path) != 0;
}

inline bool file_set_last_write(const wchar_t* path, u64 last_write) noexcept {
    const HANDLE file = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const FILETIME time{static_cast<DWORD>(last_write), static_cast<DWORD>(last_write >> 32)};
    const bool ok = SetFileTime(file, nullptr, nullptr, &time) != 0;
    const u32 error = ok ? 0 : thread_last_error();
    CloseHandle(file);
    if (!ok)
        SetLastError(error);
    return ok;
}

__forceinline bool dir_create(const wchar_t* path) noexcept {
    return CreateDirectoryW(path, nullptr) != 0;
}

__forceinline bool dir_remove(const wchar_t* path) noexcept {
    return RemoveDirectoryW(path) != 0;
}

__forceinline bool file_copy_new(const wchar_t* from, const wchar_t* to) noexcept {
    return CopyFileW(from, to, TRUE) != 0;
}

inline bool file_mark_delete_on_close(FileHandle file) noexcept {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFileW = TRUE;
    return SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)) != 0;
}

__forceinline bool file_duplicate_sealed(FileHandle file, FileHandle* sealed) noexcept {
    return DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), sealed, GENERIC_READ | DELETE, FALSE, 0) != 0;
}

// The system calls the pattern "<directory>\*" a listing of the directory.
inline bool dir_open(DirIter* iterator, const wchar_t* directory) noexcept {
    wchar_t pattern[1024];
    u32 length = 0;
    while (length < 1020 && directory[length])
        pattern[length] = directory[length], ++length;
    if (directory[length]) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return false;
    }
    while (length && (pattern[length - 1] == L'\\' || pattern[length - 1] == L'/'))
        --length;
    pattern[length++] = L'\\';
    pattern[length++] = L'*';
    pattern[length] = 0;
    iterator->find = FindFirstFileW(pattern, &iterator->data);
    if (iterator->find == INVALID_HANDLE_VALUE) {
        iterator->find = nullptr;
        return false;
    }
    iterator->pending = 1;
    return true;
}

inline bool dir_next(DirIter* iterator, DirEntry* entry) noexcept {
    if (!iterator->find) {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }
    for (;;) {
        if (iterator->pending)
            iterator->pending = 0;
        else if (!FindNextFileW(iterator->find, &iterator->data))
            return false;
        const WIN32_FIND_DATAW& found = iterator->data;
        if (found.cFileName[0] == L'.' &&
            (found.cFileName[1] == 0 || (found.cFileName[1] == L'.' && found.cFileName[2] == 0)))
            continue;
        u32 length = 0;
        for (; found.cFileName[length]; ++length)
            entry->name[length] = found.cFileName[length];
        entry->name[length] = 0;
        entry->kind = (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? FileKind::link
                      : (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)   ? FileKind::directory
                                                                               : FileKind::regular;
        entry->bytes = (static_cast<u64>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
        entry->last_write = file_detail::file_time_value(found.ftLastWriteTime);
        return true;
    }
}

inline void dir_close(DirIter* iterator) noexcept {
    if (iterator->find)
        FindClose(iterator->find);
    iterator->find = nullptr;
}

inline bool file_map(FileHandle file, bool writable, u64 bytes, FileMap* map) noexcept {
    const HANDLE mapping = CreateFileMappingW(file, nullptr, writable ? PAGE_READWRITE : PAGE_READONLY,
                                              static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
    if (!mapping)
        return false;
    void* data = MapViewOfFile(mapping, writable ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(bytes));
    const u32 error = data ? 0 : thread_last_error();
    CloseHandle(mapping);
    if (!data) {
        SetLastError(error);
        return false;
    }
    u64 mapped = bytes;
    if (mapped == 0 && !file_size(file, &mapped)) {
        const u32 size_error = thread_last_error();
        UnmapViewOfFile(data);
        SetLastError(size_error);
        return false;
    }
    *map = {data, mapped};
    return true;
}

inline void file_unmap(FileMap* map) noexcept {
    if (map->data)
        UnmapViewOfFile(map->data);
    *map = {};
}

inline bool dir_watch_open(DirWatch* watch, const wchar_t* directory) noexcept {
    const HANDLE handle = FindFirstChangeNotificationW(
        directory, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    watch->handle = handle == INVALID_HANDLE_VALUE ? nullptr : handle;
    return watch->handle != nullptr;
}

__forceinline Waitable dir_watch_waitable(const DirWatch& watch) noexcept {
    return watch.handle;
}

inline bool dir_watch_changed(DirWatch* watch) noexcept {
    if (!watch->handle || WaitForSingleObject(watch->handle, 0) != WAIT_OBJECT_0)
        return false;
    FindNextChangeNotification(watch->handle);
    return true;
}

inline void dir_watch_close(DirWatch* watch) noexcept {
    if (watch->handle)
        FindCloseChangeNotification(watch->handle);
    watch->handle = nullptr;
}

} // namespace maicord
