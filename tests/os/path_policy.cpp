#include "maicord/app/account/login/acceptance_record.hpp"
#include "maicord/app/chat/composer/message_attachment_drafts.hpp"
#include "maicord/app/settings/client_theme_storage.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/os/file.hpp"
#include "maicord/os/path.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/app/net/file_persistence.hpp"
#include "../support/test_output.hpp"

// The files seam's path policy (maicord/os/path.hpp) and the shared code that asks it: what is an absolute path on this
// system, where the last component starts, and that the update acceptance record, the attachment name, and the theme
// storage and import (which write, scan and import files under the profile) take a path of the system they run on.
// The same source runs on Windows (drive and UNC paths) and Linux (rooted paths).

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

u32 length_of(const wchar_t* text) noexcept {
    return wide_length(text, 1024);
}

ConstSlice<wchar_t> view(const wchar_t* text) noexcept {
    return {text, length_of(text)};
}

PathRoot root_of(const wchar_t* text, u32 rules = 0) noexcept {
    return path_root(view(text), rules);
}

void test_policy() noexcept {
    check(root_of(L"") == PathRoot::none && root_of(L"relative\\file") == PathRoot::none &&
              root_of(L"relative/file") == PathRoot::none && path_root({}, 0) == PathRoot::none,
          "a relative or empty path has no root");
#if defined(_WIN32)
    check(root_of(L"C:\\dir") == PathRoot::drive && root_of(L"c:\\") == PathRoot::drive &&
              root_of(L"Z:\\a\\b.txt") == PathRoot::drive,
          "a drive path is rooted");
    check(root_of(L"C:/dir") == PathRoot::none && root_of(L"C:/dir", kPathRootSlash) == PathRoot::drive &&
              root_of(L"C:\\dir", kPathRootSlash) == PathRoot::drive,
          "a forward slash after the drive is a root only when the caller allows it");
    check(root_of(L"1:\\dir") == PathRoot::none && root_of(L"C:dir") == PathRoot::none && root_of(L"C:") == PathRoot::none,
          "a drive path needs a letter and a separator");
    check(root_of(L"\\\\server\\share\\f") == PathRoot::none &&
              root_of(L"\\\\server\\share\\f", kPathRootUnc) == PathRoot::unc &&
              root_of(L"\\\\a\\b", kPathRootUnc) == PathRoot::unc && root_of(L"\\\\a\\", kPathRootUnc) == PathRoot::none,
          "a UNC path is rooted when the caller allows it and is long enough");
    check(root_of(L"\\\\?\\C:\\dir", kPathRootUnc) == PathRoot::none &&
              root_of(L"\\\\.\\COM1\\x", kPathRootUnc) == PathRoot::none,
          "a device path is never a root");
    check(root_of(L"/rooted") == PathRoot::none, "a slash-rooted path is not absolute on Windows");
    check(path_separator(L'\\') && !path_separator(L'/') && path_leaf_start(view(L"C:\\dir\\file.txt")) == 7 &&
              path_leaf_start(view(L"C:\\dir/file.txt")) == 3 && path_leaf_start(view(L"file.txt")) == 0 &&
              path_leaf_start(view(L"C:\\dir\\")) == 7,
          "the last component starts after the final backslash");
    check(path_plain(view(L"C:\\a?b")) && !path_plain(view(L"C:\\a\nb")),
          "only control characters make a Windows path not plain here (callers add the reserved ones)");
#else
    check(root_of(L"/") == PathRoot::rooted && root_of(L"/home/user/file") == PathRoot::rooted &&
              root_of(L"/home/user/file", kPathRootSlash | kPathRootUnc) == PathRoot::rooted,
          "a path starting with a slash is rooted");
    check(root_of(L"C:\\dir") == PathRoot::none && root_of(L"C:/dir", kPathRootSlash) == PathRoot::none &&
              root_of(L"\\\\server\\share\\f", kPathRootUnc) == PathRoot::none && root_of(L"\\rooted") == PathRoot::none,
          "a drive, UNC or backslash-rooted path is never absolute on Linux");
    check(path_separator(L'/') && path_separator(L'\\') && !path_separator(L'a') &&
              path_leaf_start(view(L"/dir/file.txt")) == 5 && path_leaf_start(view(L"/dir\\file.txt")) == 5 &&
              path_leaf_start(view(L"file.txt")) == 0 && path_leaf_start(view(L"/dir/")) == 5,
          "the last component starts after the final separator, either kind");
    check(path_plain(view(L"/a?b:c*d\"e")) && !path_plain(view(L"/a\nb")) && !path_plain(view(L"/a\\b")),
          "wildcards and colons are plain Linux names; controls and backslashes are not");
#endif
    wchar_t embedded[]{L'/', L'a', 0, L'b'};
#if defined(_WIN32)
    embedded[0] = L'C';
#endif
    check(!path_plain({embedded, 4}), "a NUL inside a path is not plain");
}

// A path of this system with one directory and a file in it, built the way the system names them.
struct Samples {
    wchar_t absolute[64];
    wchar_t mixed[64];
    wchar_t drive_like[64];
    wchar_t relative[32];
};

void set(wchar_t* output, const wchar_t* text) noexcept {
    copy_bytes(output, text, (length_of(text) + 1) * sizeof(wchar_t));
}

void build_samples(Samples* samples) noexcept {
#if defined(_WIN32)
    set(samples->absolute, L"C:\\Users\\mai\\photo.png");
    set(samples->mixed, L"D:\\a\\b\\report.txt");
    set(samples->drive_like, L"/home/mai/photo.png");
#else
    set(samples->absolute, L"/home/mai/photo.png");
    set(samples->mixed, L"/a\\b/report.txt");
    set(samples->drive_like, L"C:\\Users\\mai\\photo.png");
#endif
    set(samples->relative, L"dir\\photo.png");
}

bool name_is(const wchar_t* path, const char* expected) noexcept {
    MessageAttachmentFile file{};
    file.path = view(path);
    char name[64]{};
    const u32 bytes = message_attachment_file_name(file, {name, sizeof(name)});
    return bytes == c_string_length(expected) && bytes_equal(name, expected, bytes);
}

void test_attachment_names(const Samples& samples) noexcept {
    check(name_is(samples.absolute, "photo.png"), "an attachment from an absolute path is named after its last component");
    check(name_is(samples.mixed, "report.txt"), "an attachment name follows the last separator");
    check(name_is(samples.relative, "photo.png"), "an attachment from a relative path keeps its last component");
}

void test_acceptance(const Samples& samples, const wchar_t* data_file) noexcept {
    check(acceptance_record_path_valid(view(data_file)), "the acceptance record accepts a path under the profile");
    check(acceptance_record_path_valid(view(samples.absolute)), "the acceptance record accepts an absolute path of this system");
    check(!acceptance_record_path_valid(view(samples.drive_like)),
          "the acceptance record refuses the other system's absolute spelling");
    check(!acceptance_record_path_valid(view(samples.relative)) && !acceptance_record_path_valid(view(L"")),
          "the acceptance record refuses a relative path");
    wchar_t control[64];
    set(control, samples.absolute);
    control[length_of(control) - 3] = 7;
    check(!acceptance_record_path_valid(view(control)), "the acceptance record refuses a control character");
}

// The theme storage under the profile: a save and a load of the storage file, an export into the themes directory, a
// scan of it and an import that installs a picked file.
struct ThemeFixture {
    ClientThemeIoCompletion completion;
    ClientThemeCatalog catalog;
    u8 json[kClientThemeImportMaximumBytes];
};
ThemeFixture g_theme;

bool await(ClientThemeIo* io, u64 generation) noexcept {
    for (u32 attempts = 0; attempts < 40; ++attempts) {
        if (wait_one(client_theme_io_completion_event(io), 250) != WAIT_OBJECT_0)
            continue;
        bool ready = false;
        if (!client_theme_io_poll(io, &g_theme.completion, &ready))
            return false;
        if (ready && g_theme.completion.generation >= generation)
            return g_theme.completion.generation == generation;
    }
    return false;
}

// What a failed worker job says, for the log of a failed run.
void report(const char* what) noexcept {
    test_write_text(what);
    test_write_text(": error ");
    test_write_u64(static_cast<u64>(g_theme.completion.result.error));
    test_write_text(" native ");
    test_write_u64(g_theme.completion.result.native_error);
    test_write_text(" document offset ");
    test_write_u64(g_theme.completion.result.document_offset);
    test_write_line("");
}

bool exists(const wchar_t* path) noexcept {
    FileStat stat{};
    return file_path_stat(path, &stat) && stat.kind == FileKind::regular;
}

void join(wchar_t* output, const wchar_t* base, const wchar_t* leaf) noexcept {
    u32 length = length_of(base);
    copy_bytes(output, base, (length + 1) * sizeof(wchar_t));
    output[length++] = kPathSeparator;
    output[length] = 0;
    wide_append(output, 1024, &length, leaf);
}

void test_theme_storage(const wchar_t* directory) noexcept {
    auto preferences = client_preferences_defaults();
    preferences.theme = ClientTheme::ocean;
    auto document = client_theme_document_defaults(ClientThemeBase::light);
    copy_bytes(document.name, "Retained", 8);
    document.name_bytes = 8;
    document.name[document.name_bytes] = 0;
    ClientThemeStorageSnapshot snapshot{};
    check(client_theme_storage_capture(&snapshot, preferences, document, true, false), "a theme snapshot is captured");

    wchar_t storage[1024];
    join(storage, directory, L"themes.bin");
    ClientThemeStorageSnapshot loaded{};
    check(client_theme_storage_load_file(storage, &loaded).error == ClientThemeStorageError::missing,
          "a theme storage file that is not there is missing, not an invalid path");
    check(client_theme_storage_load_file(L"themes.bin", &loaded).error == ClientThemeStorageError::invalid_input,
          "a relative theme storage path is refused");
    check(client_theme_storage_save_file(storage, snapshot).ok() &&
              client_theme_storage_load_file(storage, &loaded).ok() && loaded.custom_present &&
              loaded.builtin_theme == ClientTheme::ocean,
          "the theme storage saves and loads a file under a path of this system");

    ClientThemeIo* io = nullptr;
    check(client_theme_io_create(&io, storage) && io, "the theme IO owner starts with a storage path of this system");
    if (!io)
        return;
    u64 generation = 0;
    check(client_theme_io_submit_load(io, &generation) && await(io, generation) && g_theme.completion.result.ok() &&
              g_theme.completion.snapshot.custom_present,
          "the theme worker loads the storage file");

    // A picked theme file elsewhere in the profile: written from the document, as the picker would hand it over.
    wchar_t picked[1024];
    join(picked, directory, L"picked-theme");
    u32 json_bytes = 0;
    const auto written = client_theme_document_write(snapshot.custom, {reinterpret_cast<char*>(g_theme.json), sizeof(g_theme.json)},
                                                     &json_bytes);
    const auto persisted = file_persistence_write_atomic(picked, length_of(picked), {g_theme.json, json_bytes});
    FileSnapshot imported{};
    bool captured = false;
    const HANDLE input = file_open(picked, {FileAccess::read, FileCreate::existing, kFileShareRead, 0});
    if (input != INVALID_HANDLE_VALUE) {
        u32 native = 0;
        captured = file_snapshot_read(input, &imported, &native);
        file_close(input);
    }
    check(written.ok() && persisted.ok() && captured, "a theme file to import is written under the profile");
    check(!client_theme_io_submit_import(io, view(L"picked-theme"), imported, snapshot, false, &generation),
          "a relative theme import path is refused");
    check(client_theme_io_submit_import(io, view(picked), imported, snapshot, true, &generation) &&
              await(io, generation) && g_theme.completion.result.ok() &&
              g_theme.completion.operation == ClientThemeIoOperation::import_document &&
              g_theme.completion.snapshot.custom_present,
          "a theme file picked from a path of this system imports and installs");
    if (!g_theme.completion.result.ok())
        report("import");

    wchar_t themes[1024], copied[1024], exported[1024];
    join(themes, directory, L"themes");
    join(copied, themes, L"picked-theme.json");
    join(exported, themes, L"Retained.json");
    check(exists(copied), "the picked theme is installed under its own name with .json appended");
    check(client_theme_io_submit_save(io, snapshot, true, &generation) && await(io, generation) &&
              g_theme.completion.result.ok() && exists(exported),
          "saving with export writes the document into the themes directory");

    u32 listed = 0;
    if (client_theme_catalog_scan(themes, &g_theme.catalog).ok())
        for (u32 i = 0; i < g_theme.catalog.count; ++i)
            listed += g_theme.catalog.entries[i].name_bytes == 8 && bytes_equal(g_theme.catalog.entries[i].name, "Retained", 8);
    check(listed == 2 && g_theme.catalog.count == 2, "the themes directory lists both installed documents");
    if (g_theme.catalog.count) {
        wchar_t entry_path[1024];
        u32 units = 0;
        check(client_theme_catalog_path(themes, g_theme.catalog.entries[0], {entry_path, 1024}, &units) &&
                  units == length_of(entry_path) && exists(entry_path),
              "an installed theme's path joins the directory and the file name");
    }
    check(client_theme_io_stop(&io, 5000), "the theme IO owner stops");
    file_remove(copied);
    file_remove(exported);
    file_remove(picked);
    file_remove(storage);
    dir_remove(themes);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_path_policy_entry) {
    using namespace maicord;
    wchar_t directory[1024]{};
    const DataPathResult root = data_path(directory, 1024, L"path-policy-test", true);
    if (!root.ok() || !directory_ensure(directory)) {
        test_write_line("data path");
        test_process_exit(1);
    }
    test_policy();
    Samples samples{};
    build_samples(&samples);
    test_attachment_names(samples);
    wchar_t record[1024];
    join(record, directory, L"floor.record");
    test_acceptance(samples, record);
    test_theme_storage(directory);
    dir_remove(directory);
    test_exit(g_failures, "path_policy");
}
