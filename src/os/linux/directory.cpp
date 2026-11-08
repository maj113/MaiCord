// maicord/os/file.hpp on Linux: directory enumeration (getdents64) and the directory watch (inotify, or a timed
// scan on file systems that do not report changes made elsewhere).
#include "maicord/os/file.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/os/linux/error.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "file_common.hpp"

namespace maicord {
using namespace linux_file;
namespace {

// How often a file system that inotify cannot watch is scanned.
constexpr u64 kScanIntervalMilliseconds = 500;

constexpr u32 kWatchClosed = 0;
constexpr u32 kWatchInotify = 1;
constexpr u32 kWatchScan = 2;

bool dot_entry(const char* name) noexcept {
    return name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0));
}

// Starts an enumeration of an open directory descriptor that the iterator then owns.
void adopt_directory(DirIter* iterator, int fd) noexcept {
    iterator->token = static_cast<u32>(fd) + 1;
    iterator->position = 0;
    iterator->length = 0;
}

int iterator_descriptor(const DirIter& iterator) noexcept {
    return static_cast<int>(iterator.token) - 1;
}

// Whether the file system reports changes made by other machines or the host (a mount over 9p, NFS, SMB or FUSE)
// to inotify: it does not.
bool watched_by_scanning(int directory) noexcept {
    StatFs file_system{};
    if (syscall_failed(sys_fstatfs(directory, &file_system))) {
        return false;
    }
    return file_system.type == kMagic9p || file_system.type == kMagicNfs || file_system.type == kMagicFuse ||
           file_system.type == kMagicCifs || file_system.type == kMagicSmb2;
}

// A 64-bit signature of what the directory lists: names, sizes and write times. 0 when it cannot be listed.
u64 scan_signature(int directory) noexcept {
    const long opened = sys_openat(directory, ".", kOpenReadOnly | kOpenDirectory | kOpenCloseOnExec);
    if (syscall_failed(opened)) {
        return 0;
    }
    DirIter iterator{};
    adopt_directory(&iterator, static_cast<int>(opened));
    // Entries come in directory order, which changes with every rename, so the entries are combined without order.
    u64 combined = 0;
    DirEntry entry;
    while (dir_next(&iterator, &entry)) {
        u64 signature = kHash64Seed;
        for (u32 index = 0; entry.name[index]; ++index) {
            signature = hash64_mix(signature, static_cast<u16>(entry.name[index]));
        }
        combined += hash64_mix(hash64_mix(signature, entry.bytes), entry.last_write);
    }
    dir_close(&iterator);
    return combined | 1;
}

} // namespace

bool dir_open(DirIter* iterator, const wchar_t* directory) noexcept {
    char text[kPathCapacity];
    if (!utf8_path(directory, text)) {
        return false;
    }
    const long opened = sys_open(text, kOpenReadOnly | kOpenDirectory);
    if (syscall_failed(opened)) {
        return fail(opened);
    }
    adopt_directory(iterator, static_cast<int>(opened));
    return true;
}

bool dir_next(DirIter* iterator, DirEntry* entry) noexcept {
    if (iterator->token == 0) {
        return fail_with(ERROR_INVALID_HANDLE);
    }
    const int fd = iterator_descriptor(*iterator);
    for (;;) {
        if (iterator->position >= iterator->length) {
            const long count = sys_getdents(fd, iterator->records, sizeof(iterator->records));
            if (count == -kErrorInterrupted) {
                continue;
            }
            if (syscall_failed(count)) {
                return fail(count);
            }
            if (count == 0) {
                return fail_with(ERROR_NO_MORE_FILES);
            }
            iterator->position = 0;
            iterator->length = static_cast<u32>(count);
        }
        const auto* record = reinterpret_cast<const Dirent64*>(iterator->records + iterator->position);
        iterator->position += record->record_length;
        const char* name = reinterpret_cast<const char*>(record) + kDirentNameOffset;
        if (dot_entry(name)) {
            continue;
        }
        // A name that is not UTF-8, holds a backslash or is too long for an entry cannot be named by shared code.
        const char* scan = name;
        while (*scan && *scan != '\\') {
            ++scan;
        }
        if (*scan || path_from_utf8(name, entry->name, sizeof(entry->name) / sizeof(entry->name[0])) == 0) {
            continue;
        }
        Statx statx{};
        const long result = sys_statx(fd, name, kAtSymlinkNoFollow, kStatxType | kStatxSize | kStatxMtime, &statx);
        if (syscall_failed(result)) {
            // Removed since it was listed.
            continue;
        }
        entry->kind = kind_of(statx.mode);
        entry->bytes = statx.size;
        entry->last_write = filetime_value(statx.modify);
        return true;
    }
}

void dir_close(DirIter* iterator) noexcept {
    if (iterator->token != 0) {
        sys_close(iterator_descriptor(*iterator));
    }
    iterator->token = 0;
}

bool dir_watch_open(DirWatch* watch, const wchar_t* directory) noexcept {
    *watch = {};
    char text[kPathCapacity];
    if (!utf8_path(directory, text)) {
        return false;
    }
    const long opened = sys_open(text, kOpenReadOnly | kOpenDirectory);
    if (syscall_failed(opened)) {
        return fail(opened);
    }
    watch->directory = static_cast<int>(opened);
    if (!watched_by_scanning(watch->directory)) {
        const long notify = sys_inotify_init(kInotifyNonBlock | kInotifyCloseOnExec);
        if (!syscall_failed(notify)) {
            const u32 events = kInotifyCreate | kInotifyMovedTo | kInotifyCloseWrite | kInotifyModify |
                               kInotifyAttrib | kInotifyDelete | kInotifyMovedFrom;
            if (!syscall_failed(sys_inotify_add_watch(static_cast<int>(notify), text, events))) {
                watch->notify = static_cast<int>(notify);
                watch->mode = kWatchInotify;
                return true;
            }
            // Out of watches (fs.inotify.max_user_watches): scan instead.
            sys_close(static_cast<int>(notify));
        }
    }
    watch->timer = timer_create_waitable(true, false);
    if (watch->timer == nullptr) {
        const u32 error = thread_last_error();
        sys_close(watch->directory);
        *watch = {};
        return fail_with(error);
    }
    watch->signature = scan_signature(watch->directory);
    timer_arm(watch->timer, kScanIntervalMilliseconds, 0);
    watch->mode = kWatchScan;
    return true;
}

Waitable dir_watch_waitable(const DirWatch& watch) noexcept {
    switch (watch.mode) {
    case kWatchInotify:
        return waitable_borrow_fd(watch.notify);
    case kWatchScan:
        return watch.timer;
    default:
        return nullptr;
    }
}

bool dir_watch_changed(DirWatch* watch) noexcept {
    if (watch->mode == kWatchInotify) {
        // Any event, an overflow included, is a change; the names do not matter to the caller.
        alignas(8) u8 events[4096];
        bool changed = false;
        for (;;) {
            const long count = sys_read(watch->notify, events, sizeof(events));
            if (count == -kErrorInterrupted) {
                continue;
            }
            if (syscall_failed(count) || count == 0) {
                break;
            }
            changed = true;
        }
        return changed;
    }
    if (watch->mode == kWatchScan && wait_one(watch->timer, 0) == WAIT_OBJECT_0) {
        timer_arm(watch->timer, kScanIntervalMilliseconds, 0);
        const u64 signature = scan_signature(watch->directory);
        const bool changed = signature != watch->signature;
        watch->signature = signature;
        return changed;
    }
    return false;
}

void dir_watch_close(DirWatch* watch) noexcept {
    if (watch->mode != kWatchClosed) {
        if (watch->mode == kWatchInotify) {
            sys_close(watch->notify);
        }
        if (watch->timer != nullptr) {
            waitable_close(watch->timer);
        }
        sys_close(watch->directory);
    }
    *watch = {};
}

} // namespace maicord
