#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_records.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/os/wait.hpp"

// Files, directories and file mappings (D17): open, read, write, atomic replace, identity, enumeration,
// directory watch and mapped views, with the Win32 semantics the shared code relies on.
//
//   * Paths are UTF-16 `wchar_t` strings with either separator (D16). Windows hands them to the system as they
//     are; Linux converts them to UTF-8 and '/' at the call (a file name that holds a backslash cannot be named).
//   * A failed call returns false (file_open: INVALID_HANDLE_VALUE) and leaves the Win32 error number in
//     thread_last_error(); Linux maps errno onto the numbers shared code compares (maicord/os/linux/error.hpp).
//   * Times are FILETIMEs: 100 ns units since 1601-01-01 UTC, on both systems.
//   * Sharing is advisory on Linux. A restrictive FileOpen::share takes a non-blocking flock on the file and fails
//     with ERROR_SHARING_VIOLATION when another descriptor holds a conflicting one, but nothing stops a process
//     that does not lock from writing or replacing the file. A caller that needs the bytes it checked re-checks
//     the identity (FileStat::id, file_identity.hpp) before it commits. Files are created owner-only (0600).
//
// On Windows a FileHandle is the HANDLE and every function is an inline forwarder to the Win32 call
// (maicord/os/windows/file.inl), so a call site compiles to the same imports. On Linux it is a tagged
// pointer-sized token over a file descriptor (nullptr = none, INVALID_HANDLE_VALUE = failed open); the bodies are
// in src/os/linux/file.cpp and directory.cpp. maicord/os/path.hpp is the path policy that goes with them: which
// paths are absolute (a drive or UNC root on Windows, '/' on Linux) and where a path's last component starts.

namespace maicord {

using FileHandle = HANDLE;

// How a file is accessed.
enum class FileAccess : u8 {
    read,
    write,
    read_write,
    // Every write goes to the end (FILE_APPEND_DATA / O_APPEND).
    append,
    // Neither read nor write: stat the file, or pin a directory (with kFileDirectory) to open things under it.
    attributes,
};

// What happens when the path does and does not exist.
enum class FileCreate : u8 {
    // Fails when the file is missing.
    existing,
    // Fails with ERROR_FILE_EXISTS when the file exists.
    create_new,
    // Creates the file or truncates it.
    create_always,
    // Creates the file or opens it as it is.
    open_always,
};

// FileOpen::share: what other openers may do while this one is open. The values are the FILE_SHARE_* ones.
inline constexpr u32 kFileShareRead = 1;
inline constexpr u32 kFileShareWrite = 2;
inline constexpr u32 kFileShareDelete = 4;

// FileOpen::flags.
// The access pattern, so the system reads ahead (or not).
inline constexpr u32 kFileSequential = 1u << 0;
inline constexpr u32 kFileRandom = 1u << 1;
// Writes reach the storage before the call returns.
inline constexpr u32 kFileWriteThrough = 1u << 2;
// The handle belongs to the network layer's asynchronous file operations (platform/net/network.hpp); Linux
// serves those from a thread pool, so the descriptor stays an ordinary blocking one.
inline constexpr u32 kFileOverlapped = 1u << 3;
// A final link is not followed: the link itself is opened (a reparse point on Windows), or the open fails
// (Linux O_NOFOLLOW) when the path is one.
inline constexpr u32 kFileNoFollow = 1u << 4;
// The path names a directory (Windows needs backup semantics for that).
inline constexpr u32 kFileDirectory = 1u << 5;
// A hidden scratch file (Windows HIDDEN | TEMPORARY attributes; nothing on Linux).
inline constexpr u32 kFileTemporary = 1u << 6;
// The handle may be marked for deletion (file_mark_delete_on_close).
inline constexpr u32 kFileDeletable = 1u << 7;

struct FileOpen {
    FileAccess access;
    FileCreate create;
    // kFileShare* bits.
    u32 share;
    // kFile* bits.
    u32 flags;
};

enum class FileKind : u8 {
    regular,
    directory,
    // A symbolic link or, on Windows, any reparse point (junction, symlink, cloud placeholder).
    link,
    // A device, pipe or socket.
    other,
};

// What the system knows about a file without reading it. `id` is the volume and the file number (FILE_ID_INFO):
// two files are the same file exactly when both match. On Linux the volume is the device number
// (major << 32 | minor) and the first eight bytes of the file number are the inode.
struct FileStat {
    FileKind kind;
    u32 links;
    u64 bytes;
    u64 last_write;
    // Last change of the file or its metadata (Windows ChangeTime, Linux ctime).
    u64 change;
    Win32FileIdInfo id;
};

enum class FileSeek : u8 { begin, current, end };

// One directory entry; dir_next never returns "." or "..".
struct DirEntry {
    wchar_t name[260];
    FileKind kind;
    u64 bytes;
    u64 last_write;
};

// A view of a file's bytes in memory.
struct FileMap {
    void* data;
    u64 bytes;
};

} // namespace maicord

#if defined(_WIN32)
namespace maicord {
// Walks a directory: zero-initialized, dir_open to dir_close.
struct DirIter {
    HANDLE find;
    // The first entry FindFirstFileW returned that dir_next has not handed out yet.
    u32 pending;
    WIN32_FIND_DATAW data;
};
// A directory watched for files created, renamed, resized or written (not recursive): zero-initialized.
struct DirWatch {
    HANDLE handle;
};
} // namespace maicord
#include "maicord/os/windows/file.inl"
#else
namespace maicord {
struct DirIter {
    // The descriptor + 1; 0 when closed.
    u32 token;
    u32 position;
    u32 length;
    u32 reserved;
    alignas(8) u8 records[4096];
};
struct DirWatch {
    // 0 closed, 1 inotify, 2 polling (a file system inotify does not report on, such as 9p, NFS or FUSE).
    u32 mode;
    int notify;
    int directory;
    // Polling: the timer that asks for the next scan, and the signature of the last scan.
    Waitable timer;
    u64 signature;
};

// Opens (and creates, truncates or locks, per `options`) the file. INVALID_HANDLE_VALUE on failure.
FileHandle file_open(const wchar_t* path, const FileOpen& options) noexcept;
// Closes the handle; nullptr and INVALID_HANDLE_VALUE are ignored. False (and the error) when the system reports a
// failed close.
bool file_close(FileHandle file) noexcept;

// Reads up to `bytes` from the current position: fewer only at the end of the file. *read is the count.
bool file_read(FileHandle file, void* buffer, u32 bytes, u32* read) noexcept;
// The same from `offset`; the current position afterwards is unspecified (Windows moves it, Linux does not).
bool file_read_at(FileHandle file, u64 offset, void* buffer, u32 bytes, u32* read) noexcept;
// Writes all `bytes` at the current position (the end, for FileAccess::append). *written is the count.
bool file_write(FileHandle file, const void* buffer, u32 bytes, u32* written) noexcept;
// Moves the position; *position (optional) receives the new one.
bool file_seek(FileHandle file, i64 offset, FileSeek origin, u64* position = nullptr) noexcept;
bool file_size(FileHandle file, u64* bytes) noexcept;
// Writes the file's data and metadata to the storage (FlushFileBuffers / fsync).
bool file_flush(FileHandle file) noexcept;

// Metadata of an open file, and of a path (not following a final link: a link reports FileKind::link).
bool file_stat(FileHandle file, FileStat* stat) noexcept;
bool file_path_stat(const wchar_t* path, FileStat* stat) noexcept;
// Just the write and change times, in one cheap query.
bool file_times(FileHandle file, u64* last_write, u64* change) noexcept;

// Moves `from` over `to`, replacing it atomically. `durable` makes the move reach the storage before the call
// returns (MOVEFILE_WRITE_THROUGH; on Linux fsync of the file and then of the directory).
bool file_replace(const wchar_t* from, const wchar_t* to, bool durable) noexcept;
// Moves `from` to `to` and fails with ERROR_ALREADY_EXISTS when `to` exists.
bool file_rename_noreplace(const wchar_t* from, const wchar_t* to, bool durable) noexcept;
bool file_remove(const wchar_t* path) noexcept;
// Sets the last write time (a FILETIME) of the file at `path`; the access and change times follow the system.
bool file_set_last_write(const wchar_t* path, u64 last_write) noexcept;
// Fails with ERROR_ALREADY_EXISTS when the directory exists.
bool dir_create(const wchar_t* path) noexcept;
bool dir_remove(const wchar_t* path) noexcept;
// Copies the file's bytes to a new file; fails with ERROR_FILE_EXISTS when `to` exists.
bool file_copy_new(const wchar_t* from, const wchar_t* to) noexcept;
// Deletes the file when the handle (opened with kFileDeletable) closes. Linux deletes the name at once and the
// descriptor keeps working.
bool file_mark_delete_on_close(FileHandle file) noexcept;
// A second handle on the same open file that can read it and be marked for deletion, and cannot write: the owner of
// the first may then keep writing while the sealed one is handed on. Linux reopens the descriptor read-only.
bool file_duplicate_sealed(FileHandle file, FileHandle* sealed) noexcept;

// Directory enumeration: dir_next fills the next entry and returns false at the end (thread_last_error() is then
// ERROR_NO_MORE_FILES) or on failure.
bool dir_open(DirIter* iterator, const wchar_t* directory) noexcept;
bool dir_next(DirIter* iterator, DirEntry* entry) noexcept;
void dir_close(DirIter* iterator) noexcept;

// Maps `bytes` of the file from its start (0 = the whole file, which must not be empty). A writable view of more
// bytes than the file holds extends the file. The view stays valid after the handle closes.
bool file_map(FileHandle file, bool writable, u64 bytes, FileMap* map) noexcept;
void file_unmap(FileMap* map) noexcept;

// Watches `directory` (which must exist). False when it cannot.
bool dir_watch_open(DirWatch* watch, const wchar_t* directory) noexcept;
// The waitable that is signaled while a change is pending, for the wait set of the loop. It stays signaled until
// dir_watch_changed consumes it (the Win32 change-notification handle behaves the same), and it belongs to the watch.
Waitable dir_watch_waitable(const DirWatch& watch) noexcept;
// True once per change (or burst of changes) since the last call, and arms the watch for the next one.
bool dir_watch_changed(DirWatch* watch) noexcept;
void dir_watch_close(DirWatch* watch) noexcept;
} // namespace maicord
#endif
