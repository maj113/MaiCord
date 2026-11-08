// maicord/os/file.hpp on Linux: files over raw system calls. A FileHandle is the token fd + 1 (nullptr is "none",
// INVALID_HANDLE_VALUE a failed open), paths are converted from UTF-16 to UTF-8 at each call, and every failure
// leaves the Win32 error number in thread_last_error().
//
// FileOpen::share is advisory (flock): see the header. Opening for create_always without a lock truncates with
// O_TRUNC; with one, the file is truncated only after the lock is held, so a refused open never destroys data.
#include "maicord/os/file.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/os/linux/error.hpp"
#include "maicord/platform/desktop/file_identity.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "file_common.hpp"

namespace maicord {
namespace linux_file {

bool fail(long result) noexcept {
    set_thread_last_error(win32_error_from_result(result));
    return false;
}

bool fail_with(u32 error) noexcept {
    set_thread_last_error(error);
    return false;
}

bool utf8_path(const wchar_t* path, char (&text)[kPathCapacity]) noexcept {
    if (path != nullptr && path_to_utf8(path, text, kPathCapacity) != 0) {
        return true;
    }
    return fail_with(path != nullptr && path[0] != 0 ? ERROR_BAD_PATHNAME : ERROR_PATH_NOT_FOUND);
}

u64 filetime_value(const StatxTimestamp& time) noexcept {
    return static_cast<u64>((time.seconds + kUnixEpochSeconds) * 10'000'000 + time.nanoseconds / 100);
}

FileKind kind_of(u16 mode) noexcept {
    switch (mode & kModeTypeMask) {
    case kModeRegular:
        return FileKind::regular;
    case kModeDirectory:
        return FileKind::directory;
    case kModeLink:
        return FileKind::link;
    default:
        return FileKind::other;
    }
}

} // namespace linux_file

using namespace linux_file;

namespace {

constexpr u32 kModeOwner = 0600;
constexpr u32 kChunkLimit = 1u << 30;
constexpr int kNotSupported = 95; // ENOTSUP / EOPNOTSUPP

FileHandle make_handle(int fd) noexcept {
    return reinterpret_cast<FileHandle>(static_cast<u64>(fd) + 1);
}

bool valid(FileHandle file) noexcept {
    return file != nullptr && file != INVALID_HANDLE_VALUE;
}

int descriptor(FileHandle file) noexcept {
    return static_cast<int>(reinterpret_cast<u64>(file) - 1);
}

bool invalid_handle() noexcept {
    return fail_with(ERROR_INVALID_HANDLE);
}

FileStat stat_from(const Statx& statx) noexcept {
    FileStat stat{};
    stat.kind = kind_of(statx.mode);
    stat.links = statx.links;
    stat.bytes = statx.size;
    stat.last_write = filetime_value(statx.modify);
    stat.change = filetime_value(statx.change);
    stat.id.VolumeSerialNumber = (static_cast<u64>(statx.device_major) << 32) | statx.device_minor;
    for (u32 byte = 0; byte < 8; ++byte) {
        stat.id.FileId.Identifier[byte] = static_cast<u8>(statx.inode >> (byte * 8));
    }
    return stat;
}

// statx of an open descriptor (also an O_PATH one).
long statx_of(int fd, Statx* statx) noexcept {
    return sys_statx(fd, "", kAtEmptyPath, kStatxBasicStats, statx);
}

// fsync of the file at `path`, which no descriptor of ours has open.
long sync_path(const char* path, int flags) noexcept {
    const long fd = sys_open(path, flags);
    if (syscall_failed(fd)) {
        return fd;
    }
    const long result = sys_fsync(static_cast<int>(fd));
    sys_close(static_cast<int>(fd));
    return result;
}

// fsync of the directory that holds `path`. A file system that cannot sync a directory is not an error.
long sync_parent(const char* path) noexcept {
    char parent[kPathCapacity];
    u32 length = 0;
    while (path[length]) {
        ++length;
    }
    while (length > 0 && path[length - 1] != '/') {
        --length;
    }
    if (length == 0) {
        parent[0] = '.';
        parent[1] = 0;
    } else {
        // "/name" has the parent "/".
        const u32 keep = length > 1 ? length - 1 : 1;
        copy_bytes(parent, path, keep);
        parent[keep] = 0;
    }
    const long result = sync_path(parent, kOpenReadOnly | kOpenDirectory);
    return result == -kErrorInvalid || result == -kNotSupported ? 0 : result;
}

// file_replace (`replace`) and file_rename_noreplace. `durable`: the new bytes reach the storage before the name
// does, and the name before the call returns.
bool move_file(const wchar_t* from, const wchar_t* to, bool durable, bool replace) noexcept {
    char source[kPathCapacity];
    char target[kPathCapacity];
    if (!utf8_path(from, source) || !utf8_path(to, target)) {
        return false;
    }
    long moved = durable ? sync_path(source, kOpenReadOnly) : 0;
    if (!syscall_failed(moved) && replace) {
        moved = sys_rename(source, target);
    } else if (!syscall_failed(moved)) {
        moved = sys_renameat2(source, target, kRenameNoReplace);
        if (moved == -kErrorInvalid || moved == -kNotSupported) {
            // A file system without RENAME_NOREPLACE: link() refuses an existing name too.
            moved = sys_link(source, target);
            if (!syscall_failed(moved)) {
                moved = sys_unlink(source);
            }
        }
    }
    if (!syscall_failed(moved) && durable) {
        moved = sync_parent(target);
    }
    return !syscall_failed(moved) || fail(moved);
}

// Whether a FileOpen's sharing takes a lock: every access but attributes that does not let others write.
bool locks(const FileOpen& options) noexcept {
    return options.access != FileAccess::attributes && (options.share & kFileShareWrite) == 0;
}

// Takes the lock a FileOpen's sharing asks for. False (with the sharing violation set) only when another
// descriptor holds a conflicting lock; a file system without locks just gets none.
bool take_share_lock(int fd, const FileOpen& options) noexcept {
    if (!locks(options)) {
        return true;
    }
    const bool writes = options.access != FileAccess::read;
    const long result = sys_flock(fd, (writes ? kLockExclusive : kLockShared) | kLockNonBlock);
    if (result == -kErrorAgain) {
        return fail_with(ERROR_SHARING_VIOLATION);
    }
    return true;
}

} // namespace

FileHandle file_open(const wchar_t* path, const FileOpen& options) noexcept {
    char text[kPathCapacity];
    if (!utf8_path(path, text)) {
        return INVALID_HANDLE_VALUE;
    }
    int flags = 0;
    switch (options.access) {
    case FileAccess::read:
        flags = kOpenReadOnly;
        break;
    case FileAccess::write:
        flags = kOpenWriteOnly;
        break;
    case FileAccess::read_write:
        flags = kOpenReadWrite;
        break;
    case FileAccess::append:
        flags = kOpenWriteOnly | kOpenAppend;
        break;
    case FileAccess::attributes:
        flags = kOpenPath;
        break;
    }
    const bool truncate_after_lock = options.create == FileCreate::create_always && locks(options);
    switch (options.create) {
    case FileCreate::create_new:
        flags |= kOpenCreate | kOpenExclusive;
        break;
    case FileCreate::create_always:
        flags |= kOpenCreate | (truncate_after_lock ? 0 : kOpenTruncate);
        break;
    case FileCreate::open_always:
        flags |= kOpenCreate;
        break;
    case FileCreate::existing:
        break;
    }
    if (options.flags & kFileNoFollow) {
        flags |= kOpenNoFollow;
    }
    if (options.flags & kFileDirectory) {
        flags |= kOpenDirectory;
    }
    if (options.flags & kFileWriteThrough) {
        flags |= kOpenDataSync;
    }
    const long opened = sys_open(text, flags, kModeOwner);
    if (syscall_failed(opened)) {
        // CreateFileW reports an existing file under create_new as ERROR_FILE_EXISTS, not ERROR_ALREADY_EXISTS.
        if (opened == -kErrorExists) {
            fail_with(ERROR_FILE_EXISTS);
        } else {
            fail(opened);
        }
        return INVALID_HANDLE_VALUE;
    }
    const int fd = static_cast<int>(opened);
    if (options.access != FileAccess::attributes) {
        if (!take_share_lock(fd, options)) {
            sys_close(fd);
            return INVALID_HANDLE_VALUE;
        }
        if (truncate_after_lock) {
            const long truncated = sys_ftruncate(fd, 0);
            if (syscall_failed(truncated)) {
                fail(truncated);
                sys_close(fd);
                return INVALID_HANDLE_VALUE;
            }
        }
        if (options.flags & kFileSequential) {
            sys_fadvise(fd, kAdviseSequential);
        } else if (options.flags & kFileRandom) {
            sys_fadvise(fd, kAdviseRandom);
        }
    }
    return make_handle(fd);
}

bool file_close(FileHandle file) noexcept {
    if (!valid(file)) {
        return true;
    }
    const long result = sys_close(descriptor(file));
    return syscall_failed(result) ? fail(result) : true;
}

namespace {

// file_read from the current position, or file_read_at from `offset` when `at` is set.
bool read_all(FileHandle file, bool at, u64 offset, void* buffer, u32 bytes, u32* read) noexcept {
    *read = 0;
    if (!valid(file)) {
        return invalid_handle();
    }
    auto* destination = static_cast<u8*>(buffer);
    u32 total = 0;
    while (total < bytes) {
        const u32 chunk = minimum(bytes - total, kChunkLimit);
        const long result = at ? sys_pread(descriptor(file), destination + total, chunk, offset + total)
                               : sys_read(descriptor(file), destination + total, chunk);
        if (result == -kErrorInterrupted) {
            continue;
        }
        if (syscall_failed(result)) {
            *read = total;
            return fail(result);
        }
        if (result == 0) {
            break;
        }
        total += static_cast<u32>(result);
    }
    *read = total;
    return true;
}

} // namespace

bool file_read(FileHandle file, void* buffer, u32 bytes, u32* read) noexcept {
    return read_all(file, false, 0, buffer, bytes, read);
}

bool file_read_at(FileHandle file, u64 offset, void* buffer, u32 bytes, u32* read) noexcept {
    return read_all(file, true, offset, buffer, bytes, read);
}

bool file_write(FileHandle file, const void* buffer, u32 bytes, u32* written) noexcept {
    *written = 0;
    if (!valid(file)) {
        return invalid_handle();
    }
    const auto* source = static_cast<const u8*>(buffer);
    u32 total = 0;
    while (total < bytes) {
        const u32 chunk = minimum(bytes - total, kChunkLimit);
        const long result = sys_write(descriptor(file), source + total, chunk);
        if (result == -kErrorInterrupted) {
            continue;
        }
        if (syscall_failed(result)) {
            *written = total;
            return fail(result);
        }
        if (result == 0) {
            *written = total;
            return fail_with(ERROR_WRITE_FAULT);
        }
        total += static_cast<u32>(result);
    }
    *written = total;
    return true;
}

bool file_seek(FileHandle file, i64 offset, FileSeek origin, u64* position) noexcept {
    if (!valid(file)) {
        return invalid_handle();
    }
    const int whence = origin == FileSeek::begin ? kSeekSet : origin == FileSeek::current ? kSeekCurrent : kSeekEnd;
    const long result = sys_seek(descriptor(file), offset, whence);
    if (syscall_failed(result)) {
        return fail(result);
    }
    if (position != nullptr) {
        *position = static_cast<u64>(result);
    }
    return true;
}

bool file_size(FileHandle file, u64* bytes) noexcept {
    if (!valid(file)) {
        return invalid_handle();
    }
    Statx statx{};
    const long result = statx_of(descriptor(file), &statx);
    if (syscall_failed(result)) {
        return fail(result);
    }
    *bytes = statx.size;
    return true;
}

bool file_flush(FileHandle file) noexcept {
    if (!valid(file)) {
        return invalid_handle();
    }
    const long result = sys_fsync(descriptor(file));
    return !syscall_failed(result) || fail(result);
}

bool file_stat(FileHandle file, FileStat* stat) noexcept {
    if (!valid(file)) {
        return invalid_handle();
    }
    Statx statx{};
    const long result = statx_of(descriptor(file), &statx);
    if (syscall_failed(result)) {
        return fail(result);
    }
    *stat = stat_from(statx);
    return true;
}

bool file_path_stat(const wchar_t* path, FileStat* stat) noexcept {
    char text[kPathCapacity];
    if (!utf8_path(path, text)) {
        return false;
    }
    Statx statx{};
    const long result = sys_statx(kAtCurrentDirectory, text, kAtSymlinkNoFollow, kStatxBasicStats, &statx);
    if (syscall_failed(result)) {
        return fail(result);
    }
    *stat = stat_from(statx);
    return true;
}

bool file_times(FileHandle file, u64* last_write, u64* change) noexcept {
    if (!valid(file)) {
        return invalid_handle();
    }
    Statx statx{};
    const long result = sys_statx(descriptor(file), "", kAtEmptyPath, kStatxMtime | kStatxCtime, &statx);
    if (syscall_failed(result)) {
        return fail(result);
    }
    *last_write = filetime_value(statx.modify);
    *change = filetime_value(statx.change);
    return true;
}

bool file_replace(const wchar_t* from, const wchar_t* to, bool durable) noexcept {
    return move_file(from, to, durable, true);
}

bool file_rename_noreplace(const wchar_t* from, const wchar_t* to, bool durable) noexcept {
    return move_file(from, to, durable, false);
}

bool file_remove(const wchar_t* path) noexcept {
    char text[kPathCapacity];
    if (!utf8_path(path, text)) {
        return false;
    }
    const long result = sys_unlink(text);
    if (result == -kErrorIsDirectory) {
        // DeleteFileW refuses a directory with an access error.
        return fail_with(ERROR_ACCESS_DENIED);
    }
    return !syscall_failed(result) || fail(result);
}

bool file_set_last_write(const wchar_t* path, u64 last_write) noexcept {
    char text[kPathCapacity];
    if (!utf8_path(path, text)) {
        return false;
    }
    const u64 whole = last_write / 10'000'000;
    const i64 seconds = static_cast<i64>(whole) - kUnixEpochSeconds;
    const long result = sys_set_modification_time(text, seconds, static_cast<i64>(last_write % 10'000'000) * 100);
    return !syscall_failed(result) || fail(result);
}

bool dir_create(const wchar_t* path) noexcept {
    char text[kPathCapacity];
    if (!utf8_path(path, text)) {
        return false;
    }
    const long result = sys_mkdir(text, 0700);
    return !syscall_failed(result) || fail(result);
}

bool dir_remove(const wchar_t* path) noexcept {
    char text[kPathCapacity];
    if (!utf8_path(path, text)) {
        return false;
    }
    const long result = sys_remove_directory(text);
    return !syscall_failed(result) || fail(result);
}

bool file_copy_new(const wchar_t* from, const wchar_t* to) noexcept {
    char source[kPathCapacity];
    char target[kPathCapacity];
    if (!utf8_path(from, source) || !utf8_path(to, target)) {
        return false;
    }
    const long input = sys_open(source, kOpenReadOnly);
    if (syscall_failed(input)) {
        return fail(input);
    }
    Statx statx{};
    long result = statx_of(static_cast<int>(input), &statx);
    long output = result;
    if (!syscall_failed(result)) {
        // The copy keeps the permission bits: an updater copies executables.
        output = sys_open(target, kOpenWriteOnly | kOpenCreate | kOpenExclusive, statx.mode & 07777u);
        if (output == -kErrorExists) {
            sys_close(static_cast<int>(input));
            return fail_with(ERROR_FILE_EXISTS);
        }
    }
    if (syscall_failed(output)) {
        sys_close(static_cast<int>(input));
        return fail(output);
    }
    u8 buffer[32768];
    for (;;) {
        const long count = sys_read(static_cast<int>(input), buffer, sizeof(buffer));
        if (count == -kErrorInterrupted) {
            continue;
        }
        result = count;
        if (count <= 0) {
            break;
        }
        for (long done = 0; done < count;) {
            const long wrote = sys_write(static_cast<int>(output), buffer + done, static_cast<usize>(count - done));
            if (wrote == -kErrorInterrupted) {
                continue;
            }
            if (syscall_failed(wrote) || wrote == 0) {
                result = syscall_failed(wrote) ? wrote : -kErrorNoSpace;
                break;
            }
            done += wrote;
        }
        if (syscall_failed(result)) {
            break;
        }
    }
    sys_close(static_cast<int>(input));
    sys_close(static_cast<int>(output));
    if (syscall_failed(result)) {
        sys_unlink(target);
        return fail(result);
    }
    return true;
}

namespace {

// "/proc/self/fd/<fd>" as a path string.
void proc_fd_path(int fd, char (&path)[32]) noexcept {
    copy_bytes(path, "/proc/self/fd/", 14);
    path[14 + decimal_write(path + 14, static_cast<u32>(fd))] = 0;
}

} // namespace

bool file_mark_delete_on_close(FileHandle file) noexcept {
    if (!valid(file)) {
        return invalid_handle();
    }
    // Unlinks the name the file has now, as the kernel reports it.
    char link[32];
    proc_fd_path(descriptor(file), link);
    char text[kPathCapacity];
    long result = sys_readlink(link, text, kPathCapacity - 1);
    if (syscall_failed(result)) {
        return fail(result);
    }
    text[result] = 0;
    result = sys_unlink(text);
    return !syscall_failed(result) || fail(result);
}

bool file_duplicate_sealed(FileHandle file, FileHandle* sealed) noexcept {
    if (!valid(file)) {
        return invalid_handle();
    }
    // The descriptor's own magic link opens the same file, whatever its name is now.
    char link[32];
    proc_fd_path(descriptor(file), link);
    const long fd = sys_open(link, kOpenReadOnly);
    if (syscall_failed(fd)) {
        return fail(fd);
    }
    *sealed = make_handle(static_cast<int>(fd));
    return true;
}

bool file_map(FileHandle file, bool writable, u64 bytes, FileMap* map) noexcept {
    if (!valid(file)) {
        return invalid_handle();
    }
    const int fd = descriptor(file);
    Statx statx{};
    long result = statx_of(fd, &statx);
    if (syscall_failed(result)) {
        return fail(result);
    }
    const u64 length = bytes != 0 ? bytes : statx.size;
    if (length == 0) {
        return fail_with(ERROR_FILE_INVALID);
    }
    if (length > statx.size) {
        if (!writable) {
            return fail_with(ERROR_INVALID_PARAMETER);
        }
        result = sys_ftruncate(fd, length);
        if (syscall_failed(result)) {
            return fail(result);
        }
    }
    result = sys_mmap_file(fd, length, writable ? kProtectRead | kProtectWrite : kProtectRead,
                           writable ? kMapShared : kMapPrivate);
    if (syscall_failed(result)) {
        return fail(result);
    }
    *map = {reinterpret_cast<void*>(result), length};
    return true;
}

void file_unmap(FileMap* map) noexcept {
    if (map->data != nullptr) {
        sys_munmap(map->data, map->bytes);
    }
    *map = {};
}

// platform/desktop/file_identity.hpp: the identity of an open regular file.
bool file_snapshot_read(HANDLE file, FileSnapshot* output, u32* native_error) noexcept {
    if (native_error) {
        *native_error = 0;
    }
    if (!output || !valid(file)) {
        if (native_error) {
            *native_error = ERROR_INVALID_PARAMETER;
        }
        return false;
    }
    Statx statx{};
    const long result = statx_of(descriptor(file), &statx);
    if (syscall_failed(result)) {
        if (native_error) {
            *native_error = win32_error_from_result(result);
        }
        return false;
    }
    const FileKind kind = kind_of(statx.mode);
    if (kind != FileKind::regular) {
        if (native_error) {
            *native_error = kind == FileKind::directory ? ERROR_DIRECTORY : ERROR_FILE_INVALID;
        }
        return false;
    }
    // The volume is the kernel's 32-bit device number (major in the top twelve bits, minor in the low twenty), and
    // `reserved` stays zero as on Windows: callers read a nonzero one as an identity that was never filled in.
    *output = {{statx.inode, filetime_value(statx.modify), (statx.device_major << 20) | (statx.device_minor & 0xFFFFF), 0},
               statx.size};
    return true;
}

bool file_identity_equal(const FileIdentity& a, const FileIdentity& b) noexcept {
    return a.index == b.index && a.volume == b.volume && a.reserved == b.reserved && a.last_write == b.last_write;
}

} // namespace maicord
