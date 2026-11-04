#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/os/file.hpp"
#include "maicord/text/fonts/catalog.hpp"
#include "maicord/text/fonts/store.hpp"
#include "maicord/text/opentype/sfnt.hpp"
#include "../../../src/text/fonts/path_stat.hpp"
#include "../../support/test_output.hpp"

// The font store, the catalog check and the handle stamp over real files (src/text/fonts/store.cpp, catalog.cpp,
// handle_stat.cpp): what acquiring maps, how files and faces share, what a refused file leaves behind, that no
// descriptor or handle outlives the last face, and that a replaced file no longer validates. The same expectations
// hold on Windows and Linux; the font is the first-party one in tests/generated/tt_outline_fixture.bin.

namespace maicord {
namespace {

#include "../../generated/tt_outline_fixture.inc"

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

constexpr u32 kFileSlots = 4;
constexpr u32 kFaceSlots = 8;
NativeFontFileSlot g_files[kFileSlots];
NativeFontFaceSlot g_faces[kFaceSlots];
NativeFontStore g_store{{g_files, kFileSlots}, {g_faces, kFaceSlots}};

struct Scratch {
    wchar_t directory[512];
    u32 length;
};
Scratch g_scratch;

void file_path(const wchar_t* name, wchar_t (&path)[512]) noexcept {
    u32 length = g_scratch.length;
    copy_bytes(path, g_scratch.directory, length * static_cast<u32>(sizeof(wchar_t)));
    path[length] = 0;
    wide_append(path, 512, &length, L"\\");
    wide_append(path, 512, &length, name);
}

bool write_file(const wchar_t* name, ConstSlice<u8> bytes) noexcept {
    wchar_t path[512];
    file_path(name, path);
    const FileHandle file = file_open(path, {FileAccess::write, FileCreate::create_always, 0, 0});
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    u32 written = 0;
    const bool ok = file_write(file, bytes.data, bytes.count, &written) && written == bytes.count;
    file_close(file);
    return ok;
}

// The descriptors (Linux) or handles (Windows) this process holds open.
u32 open_handles() noexcept {
#if defined(_WIN32)
    DWORD count = 0;
    GetProcessHandleCount(GetCurrentProcess(), &count);
    return count;
#else
    // The listing's own descriptor is open at every count, so the difference is exact.
    u32 count = 0;
    DirIter iterator{};
    if (dir_open(&iterator, L"/proc/self/fd")) {
        DirEntry entry;
        while (dir_next(&iterator, &entry)) {
            ++count;
        }
        dir_close(&iterator);
    }
    return count;
#endif
}

bool stats(u32 files, u32 faces, u32 references) noexcept {
    const NativeFontStoreStats found = native_font_stats(g_store);
    return found.files == files && found.faces == faces && found.references == references &&
           (files ? found.mapped_bytes > 0 : found.mapped_bytes == 0);
}

bool same(NativeFontHandle a, NativeFontHandle b) noexcept {
    return a.slot == b.slot && a.generation == b.generation;
}

bool has_glyph(const NativeFontFace* face) noexcept {
    u16 glyph = 0;
    return face != nullptr && sfnt_cmap_glyph(face->cmap, 'a', 0, &glyph) == SfntStatus::ok && glyph != 0;
}

// A collection of two faces, each a copy of `font` with its table offsets moved.
u32 build_collection(ConstSlice<u8> font, u8* output, u32 capacity) noexcept {
    const u32 offsets[]{20, (20 + font.count + 3) & ~3u};
    const u32 size = offsets[1] + font.count;
    if (size > capacity) {
        return 0;
    }
    byte_store_be32(output, sfnt_tag('t', 't', 'c', 'f'));
    byte_store_be32(output + 4, 0x10000);
    byte_store_be32(output + 8, 2);
    const u32 tables = byte_load_be16(font.data + 4);
    for (u32 index = 0; index < 2; ++index) {
        byte_store_be32(output + 12 + 4 * index, offsets[index]);
        copy_bytes(output + offsets[index], font.data, font.count);
        for (u32 table = 0; table < tables; ++table) {
            u8* field = output + offsets[index] + 12 + table * 16 + 8;
            byte_store_be32(field, byte_load_be32(field) + offsets[index]);
        }
    }
    return size;
}

u8 g_collection[32768];
const wchar_t* const kPrimary = L"primary.ttf";
const wchar_t* const kCopy = L"copy.ttf";
const wchar_t* const kCollection = L"faces.ttc";

void test_acquire_and_share() noexcept {
    wchar_t primary[512];
    wchar_t copy[512];
    wchar_t collection[512];
    file_path(kPrimary, primary);
    file_path(kCopy, copy);
    file_path(kCollection, collection);
    const u32 before = open_handles();
    NativeFontHandle a{};
    NativeFontHandle again{};
    NativeFontHandle b{};
    check(native_font_acquire(&g_store, primary, 0, &a).ok() && stats(1, 1, 1), "acquire maps one file");
    check(open_handles() == before + 1, "the file stays open while a face is held");
    check(has_glyph(native_font_get(g_store, a)), "the face reads its cmap");
    const NativeFontFace* view = native_font_get(g_store, a);
    check(native_font_acquire(&g_store, primary, 0, &again).ok() && same(a, again) && stats(1, 1, 2),
          "the same path and face is the same handle with one more reference");
    check(open_handles() == before + 1, "a second acquire holds no second file");
    check(native_font_retain(&g_store, a) && stats(1, 1, 3) && native_font_release(&g_store, again) &&
              native_font_release(&g_store, a) && stats(1, 1, 1) && native_font_get(g_store, a) == view,
          "retain and release count references");

    check(native_font_acquire(&g_store, copy, 0, &b).ok() && !same(a, b) && stats(2, 2, 2),
          "another file with the same bytes is another file");
    check(has_glyph(native_font_get(g_store, b)), "the second file reads too");
    check(native_font_release(&g_store, b) && !native_font_get(g_store, b) && stats(1, 1, 1), "release unmaps a file");
    check(!native_font_release(&g_store, b) && !native_font_retain(&g_store, b), "a released handle is stale");

    NativeFontHandle first{};
    NativeFontHandle second{};
    check(native_font_acquire(&g_store, collection, 0, &first).ok() && native_font_acquire(&g_store, collection, 1, &second).ok() &&
              stats(2, 3, 3),
          "two faces of a collection");
    check(native_font_get(g_store, first)->geometry.face.file.data == native_font_get(g_store, second)->geometry.face.file.data &&
              open_handles() == before + 2,
          "they share one mapping and one file");
    check(has_glyph(native_font_get(g_store, second)), "the second face reads");
    check(native_font_release(&g_store, first) && stats(2, 2, 2) && has_glyph(native_font_get(g_store, second)),
          "one face of a collection outlives the other");
    check(native_font_release(&g_store, second) && native_font_release(&g_store, a) && stats(0, 0, 0) &&
              open_handles() == before,
          "the last release closes everything");
}

void test_refusals() noexcept {
    wchar_t path[512];
    const u32 before = open_handles();
    NativeFontHandle handle{};
    file_path(L"missing.ttf", path);
    NativeFontResult result = native_font_acquire(&g_store, path, 0, &handle);
    check(result.error == NativeFontError::io && result.native_error == ERROR_FILE_NOT_FOUND && !handle.slot,
          "a missing file is an I/O error");
    file_path(L"bad.ttf", path);
    result = native_font_acquire(&g_store, path, 0, &handle);
    check(result.error == NativeFontError::font && !handle.slot, "a file that is not a font");
    file_path(L"empty.ttf", path);
    result = native_font_acquire(&g_store, path, 0, &handle);
    check(result.error == NativeFontError::font && result.font_status == SfntStatus::unsupported && !handle.slot,
          "an empty file");
    result = native_font_acquire(&g_store, g_scratch.directory, 0, &handle);
    check(result.error == NativeFontError::io && !handle.slot, "a directory");
    file_path(kCollection, path);
    result = native_font_acquire(&g_store, path, 2, &handle);
    check(result.error == NativeFontError::font && result.font_status == SfntStatus::face_out_of_range && !handle.slot,
          "a face past the collection");
    check(native_font_acquire(nullptr, path, 0, &handle).error == NativeFontError::invalid_input, "no store");
    check(native_font_acquire(&g_store, L"", 0, &handle).error == NativeFontError::invalid_input, "no path");
    check(native_font_acquire(&g_store, path, 0, nullptr).error == NativeFontError::invalid_input, "no output");
    check(stats(0, 0, 0) && open_handles() == before, "refusals leave nothing open or mapped");
    for (u32 round = 0; round < 64; ++round) {
        file_path(L"bad.ttf", path);
        native_font_acquire(&g_store, path, 0, &handle);
        file_path(kPrimary, path);
        NativeFontHandle held{};
        if (!native_font_acquire(&g_store, path, 0, &held).ok() || !native_font_release(&g_store, held)) {
            check(false, "a repeated acquire and release");
            break;
        }
    }
    check(stats(0, 0, 0) && open_handles() == before, "64 rounds leak nothing");
}

struct Growth {
    u32 calls;
    bool succeed;
};

bool grow(void* context, bool files, bool faces) noexcept {
    auto& growth = *static_cast<Growth*>(context);
    ++growth.calls;
    if (!growth.succeed) {
        return false;
    }
    if (files && g_store.files.count < kFileSlots) {
        ++g_store.files.count;
    }
    if (faces && g_store.faces.count < kFaceSlots) {
        ++g_store.faces.count;
    }
    return true;
}

void test_growth() noexcept {
    wchar_t primary[512];
    wchar_t copy[512];
    file_path(kPrimary, primary);
    file_path(kCopy, copy);
    const u32 before = open_handles();
    g_store.files.count = 1;
    g_store.faces.count = 1;
    Growth growth{0, false};
    NativeFontHandle a{};
    NativeFontHandle b{};
    check(native_font_acquire(&g_store, primary, 0, &a, grow, &growth).ok() && growth.calls == 0, "room, no growth");
    check(native_font_acquire(&g_store, copy, 0, &b, grow, &growth).error == NativeFontError::capacity && !b.slot &&
              growth.calls == 1 && stats(1, 1, 1),
          "a refused growth is a capacity error");
    check(open_handles() == before + 1, "the refused file is closed again");
    growth.succeed = true;
    check(native_font_acquire(&g_store, copy, 0, &b, grow, &growth).ok() && growth.calls == 2 && stats(2, 2, 2),
          "growth makes room");
    check(native_font_release(&g_store, a) && native_font_release(&g_store, b) && stats(0, 0, 0) && open_handles() == before,
          "release");
    g_store.files.count = kFileSlots;
    g_store.faces.count = kFaceSlots;
}

void test_sharing_lock() noexcept {
    wchar_t path[512];
    file_path(kPrimary, path);
    NativeFontHandle held{};
    check(native_font_acquire(&g_store, path, 0, &held).ok(), "acquire for the lock test");
    // The store holds the file read-locked: a writer that insists on sole write access is refused.
    check(file_open(path, {FileAccess::write, FileCreate::existing, kFileShareRead, 0}) == INVALID_HANDLE_VALUE &&
              thread_last_error() == ERROR_SHARING_VIOLATION,
          "a writer is refused while a face is held");
    check(native_font_release(&g_store, held) && stats(0, 0, 0), "release after the lock test");
    const FileHandle writer = file_open(path, {FileAccess::write, FileCreate::existing, kFileShareRead, 0});
    check(writer != INVALID_HANDLE_VALUE, "a writer opens once the faces are gone");
    file_close(writer);
}

void test_stamp_and_validate() noexcept {
    wchar_t path[512];
    file_path(kPrimary, path);
    FileStat stat{};
    check(file_path_stat(path, &stat) && stat.kind == FileKind::regular, "stat the font file");
    const FileHandle file = file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead, 0});
    NativeFontFileStamp stamp{};
    check(font_path_stat::read_handle(file, &stamp), "a regular file has a stamp");
    check(stamp.bytes == stat.bytes && bytes_equal(&stamp.identity, &stat.id, sizeof(stat.id)) &&
              bytes_equal(&stamp.write, &stat.last_write, 8) && bytes_equal(&stamp.change, &stat.change, 8),
          "the stamp is the file's metadata");
    file_close(file);

    const FileHandle directory = file_open(g_scratch.directory, {FileAccess::attributes, FileCreate::existing,
                                                                 kFileShareRead | kFileShareWrite | kFileShareDelete,
                                                                 kFileDirectory});
    NativeFontFileStamp untouched{};
    untouched.bytes = 123;
    check(directory != INVALID_HANDLE_VALUE && !font_path_stat::read_handle(directory, &untouched) && untouched.bytes == 123,
          "a directory has no stamp and the output stays as it was");
#if !defined(_WIN32)
    u32 error = 0;
    // Windows answers a directory handle's size and identity queries; Linux has no second query.
    check(!font_path_stat::read_fallback(directory, &untouched, &error) && error != 0, "and the fallback says why");
#endif
    file_close(directory);

    const u16 name[]{u'T'};
    NativeFontCatalog catalog{stamp, path, {name, 1}, nullptr, nullptr, 0, 0, 0};
    NativeFontHandle handle{};
    check(native_font_acquire(&g_store, path, 0, &handle).ok(), "acquire for validation");
    check(native_font_catalog_validate(catalog, g_store, handle).ok(), "the catalog matches its file");
    NativeFontCatalog other_face = catalog;
    other_face.face_index = 1;
    check(native_font_catalog_validate(other_face, g_store, handle).native_error == ERROR_RETRY, "another face index");
    NativeFontCatalog other_size = catalog;
    ++other_size.stamp.bytes;
    check(native_font_catalog_validate(other_size, g_store, handle).native_error == ERROR_RETRY, "another size");
    NativeFontCatalog other_write = catalog;
    other_write.stamp.write.dwLowDateTime ^= 1;
    check(native_font_catalog_validate(other_write, g_store, handle).native_error == ERROR_RETRY, "another write time");
    NativeFontCatalog other_change = catalog;
    other_change.stamp.change.dwHighDateTime ^= 1;
    check(native_font_catalog_validate(other_change, g_store, handle).native_error == ERROR_RETRY, "another change time");
    NativeFontCatalog other_file = catalog;
    other_file.stamp.identity.FileId.Identifier[0] ^= 1;
    check(native_font_catalog_validate(other_file, g_store, handle).native_error == ERROR_RETRY, "another file");
    check(native_font_catalog_validate(catalog, g_store, {0, 1}).error == NativeFontError::invalid_input, "a stale handle");
    check(native_font_release(&g_store, handle), "release after validation");

    // A new file under the same name has another identity: the old stamp no longer validates.
    wchar_t replacement[512];
    file_path(L"replacement.ttf", replacement);
    ConstSlice<u8> font{tt_outline_fixture + 20, byte_load_le32(tt_outline_fixture + 12)};
    check(write_file(L"replacement.ttf", font) && file_replace(replacement, path, false), "replace the font file");
    check(native_font_acquire(&g_store, path, 0, &handle).ok(), "acquire the replacement");
    check(native_font_catalog_validate(catalog, g_store, handle).native_error == ERROR_RETRY,
          "a replaced file does not match the old stamp");
    check(native_font_release(&g_store, handle), "release the replacement");
}

void remove_scratch() noexcept {
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

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_font_store_files_entry) {
    using namespace maicord;
    const ConstSlice<u8> font{tt_outline_fixture + 20, byte_load_le32(tt_outline_fixture + 12)};
    if (font.count >= 16000 || font.count + 20 >= sizeof(tt_outline_fixture)) {
        test_write_line("font fixture");
        test_process_exit(1);
    }
    const DataPathResult root = data_path(g_scratch.directory, 512, L"font-store-test", true);
    if (!root.ok()) {
        test_write_line("data path");
        test_process_exit(1);
    }
    g_scratch.length = root.length;
    if (!dir_create(g_scratch.directory) && thread_last_error() != ERROR_ALREADY_EXISTS) {
        test_write_line("scratch directory");
        test_process_exit(1);
    }
    remove_scratch();
    const u8 invalid[]{0, 0, 0, 0};
    const u32 collection_bytes = build_collection(font, g_collection, sizeof(g_collection));
    check(collection_bytes != 0 && write_file(kPrimary, font) && write_file(kCopy, font) &&
              write_file(kCollection, {g_collection, collection_bytes}) && write_file(L"bad.ttf", {invalid, 4}) &&
              write_file(L"empty.ttf", {invalid, 0}),
          "the font files are written");
    test_acquire_and_share();
    test_refusals();
    test_growth();
    test_sharing_lock();
    test_stamp_and_validate();
    remove_scratch();
    dir_remove(g_scratch.directory);
    test_exit(g_failures, "font_store_files");
}
