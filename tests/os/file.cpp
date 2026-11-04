#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/os/file.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/desktop/file_identity.hpp"
#include "../support/test_output.hpp"

// Files, directories and mappings (maicord/os/file.hpp): open modes and sharing, reads, writes and sizes, metadata
// and identity, atomic replace, enumeration, copies, mapped views, delete on close and the directory watch. The same
// expectations hold on Windows (the Win32 calls) and Linux (system calls). Nothing here measures time: a wait for a
// change uses a generous limit.

namespace maicord {
namespace {

constexpr u32 kLimit = 5000;
u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

// The scratch directory and the names made under it, written with Windows separators on both systems.
struct Scratch {
    wchar_t directory[512];
    u32 length;
};
Scratch g_scratch;

// <scratch>\<name> in `path`.
void file_path(const wchar_t* name, wchar_t (&path)[512]) noexcept {
    u32 length = g_scratch.length;
    copy_bytes(path, g_scratch.directory, length * static_cast<u32>(sizeof(wchar_t)));
    path[length] = 0;
    wide_append(path, 512, &length, L"\\");
    wide_append(path, 512, &length, name);
}

constexpr FileOpen kReadShared{FileAccess::read, FileCreate::existing, kFileShareRead, 0};
constexpr FileOpen kCreateNew{FileAccess::write, FileCreate::create_new, 0, 0};

bool write_text(const wchar_t* name, const char* text) noexcept {
    wchar_t path[512];
    file_path(name, path);
    const FileHandle file = file_open(path, {FileAccess::write, FileCreate::create_always, 0, 0});
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    u32 written = 0;
    const u32 bytes = c_string_length(text);
    const bool ok = file_write(file, text, bytes, &written) && written == bytes;
    file_close(file);
    return ok;
}

// The whole content of the file into `output` (capacity 64); the byte count, or ~0u when it cannot be read.
u32 read_text(const wchar_t* name, char (&output)[64]) noexcept {
    wchar_t path[512];
    file_path(name, path);
    const FileHandle file = file_open(path, kReadShared);
    if (file == INVALID_HANDLE_VALUE) {
        return ~0u;
    }
    u32 count = 0;
    const bool ok = file_read(file, output, sizeof(output), &count);
    file_close(file);
    return ok ? count : ~0u;
}

bool content_is(const wchar_t* name, const char* expected) noexcept {
    char text[64];
    const u32 count = read_text(name, text);
    return count == c_string_length(expected) && bytes_equal(text, expected, count);
}

bool exists(const wchar_t* name) noexcept {
    wchar_t path[512];
    file_path(name, path);
    FileStat stat{};
    return file_path_stat(path, &stat);
}

bool remove_file(const wchar_t* name) noexcept {
    wchar_t path[512];
    file_path(name, path);
    return file_remove(path);
}

// Empties the scratch directory of what an earlier run left.
void clear_scratch() noexcept {
    DirIter iterator{};
    if (!dir_open(&iterator, g_scratch.directory)) {
        return;
    }
    DirEntry entry;
    while (dir_next(&iterator, &entry)) {
        wchar_t path[512];
        file_path(entry.name, path);
        if (entry.kind == FileKind::directory) {
            dir_remove(path);
        } else {
            file_remove(path);
        }
    }
    dir_close(&iterator);
}

void test_open_modes() noexcept {
    wchar_t path[512];
    file_path(L"modes.bin", path);
    const FileHandle created = file_open(path, kCreateNew);
    check(created != INVALID_HANDLE_VALUE && created != nullptr, "create_new creates");
    u32 count = 0;
    check(file_write(created, "hello", 5, &count) && count == 5, "write");
    file_close(created);
    check(file_open(path, kCreateNew) == INVALID_HANDLE_VALUE && thread_last_error() == ERROR_FILE_EXISTS,
          "create_new refuses an existing file with ERROR_FILE_EXISTS");

    wchar_t missing[512];
    file_path(L"missing.bin", missing);
    check(file_open(missing, kReadShared) == INVALID_HANDLE_VALUE && thread_last_error() == ERROR_FILE_NOT_FOUND,
          "existing refuses a missing file with ERROR_FILE_NOT_FOUND");
    check(file_open(nullptr, kReadShared) == INVALID_HANDLE_VALUE, "a null path fails");

    check(content_is(L"modes.bin", "hello"), "reads what was written");
    const FileHandle kept = file_open(path, {FileAccess::write, FileCreate::open_always, 0, 0});
    check(kept != INVALID_HANDLE_VALUE, "open_always opens an existing file");
    file_close(kept);
    check(content_is(L"modes.bin", "hello"), "open_always keeps the content");
    const FileHandle truncated = file_open(path, {FileAccess::write, FileCreate::create_always, 0, 0});
    check(truncated != INVALID_HANDLE_VALUE, "create_always opens an existing file");
    file_close(truncated);
    check(content_is(L"modes.bin", ""), "create_always truncates");
    const FileHandle made = file_open(missing, {FileAccess::write, FileCreate::open_always, 0, 0});
    check(made != INVALID_HANDLE_VALUE && exists(L"missing.bin"), "open_always creates a missing file");
    file_close(made);
    check(remove_file(L"missing.bin") && !exists(L"missing.bin"), "remove");
    check(!remove_file(L"missing.bin") && thread_last_error() == ERROR_FILE_NOT_FOUND,
          "removing a missing file fails with ERROR_FILE_NOT_FOUND");

    for (u32 round = 0; round < 2; ++round) {
        const FileHandle append = file_open(path, {FileAccess::append, FileCreate::open_always,
                                                    kFileShareRead | kFileShareWrite, 0});
        check(append != INVALID_HANDLE_VALUE && file_write(append, "ab", 2, &count) && count == 2, "append writes");
        file_close(append);
    }
    check(content_is(L"modes.bin", "abab"), "append goes to the end");
}

void test_read_write_seek() noexcept {
    wchar_t path[512];
    file_path(L"rw.bin", path);
    const FileHandle file = file_open(path, {FileAccess::read_write, FileCreate::create_always, 0, 0});
    check(file != INVALID_HANDLE_VALUE, "open read_write");
    u32 count = 0;
    check(file_write(file, "0123456789", 10, &count) && count == 10, "write 10");
    u64 position = 0;
    check(file_seek(file, 0, FileSeek::current, &position) && position == 10, "position after writing");
    check(file_seek(file, 2, FileSeek::begin, &position) && position == 2, "seek begin");
    char text[16]{};
    check(file_read(file, text, 4, &count) && count == 4 && bytes_equal(text, "2345", 4), "read from the position");
    check(file_seek(file, -3, FileSeek::end, &position) && position == 7, "seek end");
    check(file_read(file, text, 16, &count) && count == 3 && bytes_equal(text, "789", 3), "read stops at the end");
    check(file_read(file, text, 16, &count) && count == 0, "read at the end returns nothing");
    check(file_read_at(file, 1, text, 3, &count) && count == 3 && bytes_equal(text, "123", 3), "read_at");
    check(file_read_at(file, 100, text, 3, &count) && count == 0, "read_at past the end");
    check(file_write_at(file, 4, "ab", 2, &count) && count == 2 && file_read_at(file, 0, text, 10, &count) &&
              count == 10 && bytes_equal(text, "0123ab6789", 10),
          "write_at overwrites in place");
    u64 bytes = 0;
    check(file_size(file, &bytes) && bytes == 10, "size");
    check(file_set_size(file, 4) && file_size(file, &bytes) && bytes == 4, "truncate");
    check(file_set_size(file, 8) && file_size(file, &bytes) && bytes == 8, "extend");
    check(file_read_at(file, 0, text, 8, &count) && count == 8 && bytes_equal(text, "0123", 4) &&
              test_filled(reinterpret_cast<const u8*>(text) + 4, 4, 0),
          "an extension reads as zeros");
    check(file_flush(file), "flush");
    file_close(file);
    file_close(nullptr);
    file_close(INVALID_HANDLE_VALUE);
    check(!file_read(INVALID_HANDLE_VALUE, text, 1, &count), "reading an invalid handle fails");
}

void test_sharing() noexcept {
    wchar_t path[512];
    file_path(L"share.bin", path);
    write_text(L"share.bin", "data");
    const FileHandle first = file_open(path, kReadShared);
    const FileHandle second = file_open(path, kReadShared);
    check(first != INVALID_HANDLE_VALUE && second != INVALID_HANDLE_VALUE, "two readers share");
    check(file_open(path, {FileAccess::write, FileCreate::existing, kFileShareRead, 0}) == INVALID_HANDLE_VALUE &&
              thread_last_error() == ERROR_SHARING_VIOLATION,
          "a writer is refused while readers deny writing");
    check(file_open(path, {FileAccess::write, FileCreate::create_always, kFileShareRead, 0}) == INVALID_HANDLE_VALUE,
          "create_always is refused too");
    file_close(first);
    file_close(second);
    check(content_is(L"share.bin", "data"), "a refused create_always leaves the data");

    const FileHandle exclusive = file_open(path, {FileAccess::write, FileCreate::existing, 0, 0});
    check(exclusive != INVALID_HANDLE_VALUE, "an exclusive writer opens");
    check(file_open(path, kReadShared) == INVALID_HANDLE_VALUE && thread_last_error() == ERROR_SHARING_VIOLATION,
          "a reader is refused while a writer is exclusive");
    file_close(exclusive);
    const FileHandle again = file_open(path, kReadShared);
    check(again != INVALID_HANDLE_VALUE, "the file opens again after the writer closed");
    file_close(again);
}

void test_stat_and_identity() noexcept {
    wchar_t path[512];
    wchar_t other_path[512];
    file_path(L"stat.bin", path);
    file_path(L"stat2.bin", other_path);
    write_text(L"stat.bin", "twelve bytes");
    write_text(L"stat2.bin", "x");
    const FileHandle file = file_open(path, kReadShared);
    const FileHandle same = file_open(path, kReadShared);
    const FileHandle other = file_open(other_path, kReadShared);
    FileStat stat{};
    FileStat same_stat{};
    FileStat other_stat{};
    check(file_stat(file, &stat) && file_stat(same, &same_stat) && file_stat(other, &other_stat), "stat");
    check(stat.kind == FileKind::regular && stat.bytes == 12 && stat.links == 1, "kind, size and links");
    check(stat.last_write != 0 && stat.change != 0, "times");
    check(bytes_equal(&stat.id, &same_stat.id, sizeof(stat.id)), "the same file has the same id");
    check(!bytes_equal(&stat.id, &other_stat.id, sizeof(stat.id)), "another file has another id");
    u64 last_write = 0;
    u64 change = 0;
    check(file_times(file, &last_write, &change) && last_write == stat.last_write && change == stat.change,
          "times match stat");
    FileStat by_path{};
    check(file_path_stat(path, &by_path) && by_path.kind == FileKind::regular && by_path.bytes == 12 &&
              bytes_equal(&by_path.id, &stat.id, sizeof(stat.id)),
          "path stat agrees with handle stat");
    FileSnapshot snapshot{};
    FileSnapshot same_snapshot{};
    u32 error = 0;
    check(file_snapshot_read(file, &snapshot, &error) && file_snapshot_read(same, &same_snapshot, &error) &&
              snapshot.bytes == 12 && file_identity_equal(snapshot.identity, same_snapshot.identity),
          "snapshot identity");
    FileSnapshot other_snapshot{};
    check(file_snapshot_read(other, &other_snapshot, &error) &&
              !file_identity_equal(snapshot.identity, other_snapshot.identity),
          "snapshot identity differs between files");
    file_close(file);
    file_close(same);
    file_close(other);

    FileStat directory{};
    check(file_path_stat(g_scratch.directory, &directory) && directory.kind == FileKind::directory, "a directory");
    const FileHandle pinned = file_open(g_scratch.directory, {FileAccess::attributes, FileCreate::existing,
                                                              kFileShareRead | kFileShareWrite | kFileShareDelete,
                                                              kFileDirectory});
    check(pinned != INVALID_HANDLE_VALUE && file_stat(pinned, &stat) && stat.kind == FileKind::directory,
          "a pinned directory");
    check(!file_snapshot_read(pinned, &snapshot, &error) && error == ERROR_DIRECTORY, "a directory has no snapshot");
    file_close(pinned);
    wchar_t missing[512];
    file_path(L"nothing-here", missing);
    check(!file_path_stat(missing, &stat) && thread_last_error() == ERROR_FILE_NOT_FOUND, "a missing path");
}

void test_replace() noexcept {
    wchar_t temporary[512];
    wchar_t target[512];
    file_path(L"replace.tmp", temporary);
    file_path(L"replace.bin", target);
    write_text(L"replace.bin", "old");
    write_text(L"replace.tmp", "new one");
    check(file_replace(temporary, target, false) && content_is(L"replace.bin", "new one") && !exists(L"replace.tmp"),
          "replace overwrites");
    write_text(L"replace.tmp", "durable");
    check(file_replace(temporary, target, true) && content_is(L"replace.bin", "durable") && !exists(L"replace.tmp"),
          "a durable replace overwrites");
    write_text(L"replace.tmp", "fresh");
    check(!file_rename_noreplace(temporary, target, false) && thread_last_error() == ERROR_ALREADY_EXISTS &&
              content_is(L"replace.bin", "durable") && exists(L"replace.tmp"),
          "rename_noreplace refuses an existing target");
    remove_file(L"replace.bin");
    check(file_rename_noreplace(temporary, target, true) && content_is(L"replace.bin", "fresh") &&
              !exists(L"replace.tmp"),
          "rename_noreplace moves to a free name");
    check(!file_replace(temporary, target, false), "replace of a missing source fails");
}

void test_directories() noexcept {
    wchar_t sub[512];
    file_path(L"sub", sub);
    check(dir_create(sub), "dir_create");
    check(!dir_create(sub) && thread_last_error() == ERROR_ALREADY_EXISTS, "dir_create of an existing directory");
    write_text(L"sub\\a.txt", "abc");
    write_text(L"sub\\b.txt", "de");
    wchar_t inner[512];
    file_path(L"sub\\inner", inner);
    check(dir_create(inner), "nested directory");

    u32 files = 0;
    u32 directories = 0;
    u32 dots = 0;
    u64 bytes = 0;
    bool names_right = true;
    DirIter iterator{};
    check(dir_open(&iterator, sub), "dir_open");
    DirEntry entry;
    while (dir_next(&iterator, &entry)) {
        dots += entry.name[0] == L'.' && (entry.name[1] == 0 || (entry.name[1] == L'.' && entry.name[2] == 0));
        if (entry.kind == FileKind::regular) {
            ++files;
            bytes += entry.bytes;
            names_right = names_right && entry.name[1] == L'.' && entry.name[5] == 0 && entry.last_write != 0;
        } else if (entry.kind == FileKind::directory) {
            ++directories;
            names_right = names_right && entry.name[0] == L'i' && entry.name[5] == 0;
        }
    }
    check(thread_last_error() == ERROR_NO_MORE_FILES, "the listing ends with ERROR_NO_MORE_FILES");
    dir_close(&iterator);
    check(files == 2 && directories == 1 && bytes == 5 && dots == 0 && names_right, "listing");
    check(!dir_next(&iterator, &entry), "a closed iterator yields nothing");

    check(!dir_remove(sub), "a directory with entries is not removed");
    remove_file(L"sub\\a.txt");
    remove_file(L"sub\\b.txt");
    check(dir_remove(inner) && dir_remove(sub), "empty directories are removed");
    wchar_t missing[512];
    file_path(L"no-directory", missing);
    check(!dir_open(&iterator, missing), "dir_open of a missing directory");

    // More entries than one read of the system returns.
    check(dir_create(sub), "dir_create again");
    constexpr u32 kMany = 300;
    u32 created = 0;
    for (u32 index = 0; index < kMany; ++index) {
        wchar_t name[128];
        u32 length = 0;
        wide_append(name, 128, &length, L"sub\\entry-with-a-long-enough-name-to-fill-the-buffer-");
        wide_append_hex(name, 128, &length, index);
        wchar_t path[512];
        file_path(name, path);
        const FileHandle file = file_open(path, kCreateNew);
        created += file != INVALID_HANDLE_VALUE;
        file_close(file);
    }
    u32 listed = 0;
    u64 sum = 0;
    check(dir_open(&iterator, sub), "dir_open many");
    while (dir_next(&iterator, &entry)) {
        ++listed;
        u64 index = 0;
        for (u32 digit = 0; entry.name[digit]; ++digit) {
            const wchar_t value = entry.name[digit];
            if (digit >= 49) {
                index = index * 16 + (value >= L'a' ? value - L'a' + 10 : value - L'0');
            }
        }
        sum += index;
    }
    dir_close(&iterator);
    check(created == kMany && listed == kMany && sum == static_cast<u64>(kMany) * (kMany - 1) / 2, "a long listing");
    DirIter cleanup{};
    if (dir_open(&cleanup, sub)) {
        while (dir_next(&cleanup, &entry)) {
            wchar_t path[512];
            wchar_t name[300] = L"sub\\";
            u32 length = 4;
            wide_append(name, 300, &length, entry.name);
            file_path(name, path);
            file_remove(path);
        }
        dir_close(&cleanup);
    }
    check(dir_remove(sub), "the long listing is cleaned up");
}

void test_copy() noexcept {
    wchar_t from[512];
    wchar_t to[512];
    file_path(L"copy-from.bin", from);
    file_path(L"copy-to.bin", to);
    write_text(L"copy-from.bin", "copied bytes");
    check(file_copy_new(from, to) && content_is(L"copy-to.bin", "copied bytes") && content_is(L"copy-from.bin", "copied bytes"),
          "copy_new copies");
    check(!file_copy_new(from, to) && thread_last_error() == ERROR_FILE_EXISTS, "copy_new refuses an existing target");
    remove_file(L"copy-to.bin");
    wchar_t missing[512];
    file_path(L"copy-missing.bin", missing);
    check(!file_copy_new(missing, to) && !exists(L"copy-to.bin"), "copy_new of a missing source leaves nothing");
    remove_file(L"copy-from.bin");
}

void test_mapping() noexcept {
    wchar_t path[512];
    file_path(L"map.bin", path);
    write_text(L"map.bin", "mapped file content");
    FileMap view{};
    {
        const FileHandle file = file_open(path, kReadShared);
        check(file_map(file, false, 0, &view) && view.bytes == 19 && bytes_equal(view.data, "mapped file content", 19),
              "map the whole file");
        FileMap prefix{};
        check(file_map(file, false, 6, &prefix) && prefix.bytes == 6 && bytes_equal(prefix.data, "mapped", 6),
              "map a prefix");
        file_unmap(&prefix);
        check(prefix.data == nullptr && prefix.bytes == 0, "unmap clears the view");
        FileMap too_long{};
        check(!file_map(file, false, 100, &too_long), "a read-only view cannot extend the file");
        file_close(file);
    }
    check(bytes_equal(view.data, "mapped file content", 19), "the view outlives the handle");
    file_unmap(&view);

    wchar_t empty[512];
    file_path(L"map-empty.bin", empty);
    const FileHandle empty_file = file_open(empty, {FileAccess::read_write, FileCreate::create_always, 0, 0});
    FileMap none{};
    check(!file_map(empty_file, false, 0, &none), "an empty file cannot be mapped");
    FileMap writable{};
    check(file_map(empty_file, true, 16, &writable) && writable.bytes == 16, "a writable view extends the file");
    u64 bytes = 0;
    check(file_size(empty_file, &bytes) && bytes == 16, "the file has the extended size");
    auto* data = static_cast<u8*>(writable.data);
    check(test_filled(data, 16, 0), "the extension is zero");
    copy_bytes(data, "marker", 6);
    file_unmap(&writable);
    file_close(empty_file);
    char text[64];
    check(read_text(L"map-empty.bin", text) == 16 && bytes_equal(text, "marker", 6) &&
              test_filled(reinterpret_cast<const u8*>(text) + 6, 10, 0),
          "writes through a view reach the file");
    remove_file(L"map-empty.bin");
    remove_file(L"map.bin");
}

void test_delete_on_close() noexcept {
    wchar_t path[512];
    file_path(L"temporary.bin", path);
    const FileHandle file = file_open(path, {FileAccess::read_write, FileCreate::create_new, kFileShareRead,
                                             kFileDeletable});
    check(file != INVALID_HANDLE_VALUE, "open a deletable file");
    u32 count = 0;
    check(file_write(file, "scratch", 7, &count), "write");
    check(file_mark_delete_on_close(file), "mark for deletion");
    char text[16]{};
    check(file_read_at(file, 0, text, 7, &count) && count == 7 && bytes_equal(text, "scratch", 7),
          "the open handle still reads");
    file_close(file);
    check(!exists(L"temporary.bin"), "the file is gone after the close");
}

void test_final_path() noexcept {
    wchar_t path[512];
    file_path(L"final-path.bin", path);
    write_text(L"final-path.bin", "x");
    const FileHandle file = file_open(path, kReadShared);
    wchar_t resolved[1024];
    u32 length = 0;
    check(file_final_path(file, resolved, 1024, &length) && length > 14, "final path");
    const wchar_t tail[] = L"final-path.bin";
    bool matches = length >= 14;
    for (u32 index = 0; matches && index < 14; ++index) {
        matches = resolved[length - 14 + index] == tail[index];
    }
    check(matches && resolved[length] == 0, "the final path ends with the file name");
    wchar_t small[4];
    check(!file_final_path(file, small, 4, &length), "a small buffer fails");
    file_close(file);
    remove_file(L"final-path.bin");
}

void test_directory_watch() noexcept {
    DirWatch watch{};
    wchar_t missing[512];
    file_path(L"no-watch-directory", missing);
    check(!dir_watch_open(&watch, missing), "a missing directory cannot be watched");
    check(dir_watch_open(&watch, g_scratch.directory), "watch the scratch directory");
    const Waitable waitable = dir_watch_waitable(watch);
    check(waitable != nullptr, "the watch has a waitable");

    write_text(L"watched.bin", "first");
    check(wait_one(waitable, kLimit) == WAIT_OBJECT_0, "a created file signals the watch");
    check(dir_watch_changed(&watch), "the change is reported");
    // The watch re-arms: the next change signals again (a burst may leave a trailing signal, so wait for the file).
    remove_file(L"watched.bin");
    check(wait_one(waitable, kLimit) == WAIT_OBJECT_0, "a removed file signals the watch again");
    check(dir_watch_changed(&watch), "the second change is reported");
    dir_watch_close(&watch);
    check(dir_watch_waitable(watch) == nullptr && !dir_watch_changed(&watch), "a closed watch is empty");
    dir_watch_close(&watch);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_os_file_entry) {
    using namespace maicord;
    const DataPathResult root = data_path(g_scratch.directory, 512, L"file-seam-test", true);
    if (!root.ok()) {
        test_write_line("data path");
        test_process_exit(1);
    }
    g_scratch.length = root.length;
    if (!dir_create(g_scratch.directory) && thread_last_error() != ERROR_ALREADY_EXISTS) {
        test_write_line("scratch directory");
        test_process_exit(1);
    }
    clear_scratch();
    test_open_modes();
    test_read_write_seek();
    test_sharing();
    test_stat_and_identity();
    test_replace();
    test_directories();
    test_copy();
    test_mapping();
    test_delete_on_close();
    test_final_path();
    test_directory_watch();
    clear_scratch();
    test_exit(g_failures, "os_file");
}
