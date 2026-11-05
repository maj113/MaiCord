// The Linux self-updater's stage and swap (service.hpp). The install is a directory (packaging/README.md): the program,
// fonts/ and licenses/. A stage is a directory of the same names beside the program (<executable>.update), and the swap
// moves each live entry into <executable>.old and each staged one in, so the old version stays whole for a rollback.
#include "maicord/app/update/log.hpp"
#include "maicord/app/update/package.hpp"
#include "maicord/app/update/service.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/os/file.hpp"
#include "tree.hpp"

namespace maicord {
namespace {

constexpr u32 kPathCharacters = 1024;
constexpr u32 kMaximumDepth = 4;
constexpr wchar_t kFonts[] = L"fonts", kLicenses[] = L"licenses", kStagedExecutable[] = L"MaiCord";

struct Path {
    wchar_t text[kPathCharacters];
    u32 count;
};

bool join(Path* out, const wchar_t* base, u32 base_count, const wchar_t* leaf, u32 leaf_count) noexcept {
    if (base_count + 1 + leaf_count >= kPathCharacters) return false;
    copy_bytes(out->text, base, base_count * sizeof(wchar_t));
    out->text[base_count] = L'/';
    copy_bytes(out->text + base_count + 1, leaf, leaf_count * sizeof(wchar_t));
    out->count = base_count + 1 + leaf_count;
    out->text[out->count] = 0;
    return true;
}
u32 length(const wchar_t* text) noexcept {
    u32 n = 0;
    while (text[n]) ++n;
    return n;
}
bool exists(const wchar_t* path) noexcept {
    FileStat stat{};
    return file_path_stat(path, &stat);
}

bool remove_directory(Path* path, u32 depth) noexcept;
// Removes one child of the directory `path` names, below `base` characters of it.
bool remove_entry(Path* path, u32 base, const DirEntry& entry, u32 depth) noexcept {
    const u32 leaf = length(entry.name);
    if (base + 1 + leaf >= kPathCharacters) return false;
    path->text[base] = L'/';
    copy_bytes(path->text + base + 1, entry.name, (leaf + 1) * sizeof(wchar_t));
    path->count = base + 1 + leaf;
    const bool removed = entry.kind == FileKind::directory ? remove_directory(path, depth + 1) : file_remove(path->text);
    path->count = base;
    path->text[base] = 0;
    return removed;
}
bool remove_directory(Path* path, u32 depth) noexcept {
    if (depth > kMaximumDepth) return false;
    const u32 base = path->count;
    // Deleting while a directory is read may skip entries: read it again until it is empty.
    for (u32 pass = 0; pass < 3; ++pass) {
        DirIter iterator{};
        if (!dir_open(&iterator, path->text)) return false;
        DirEntry entry{};
        bool failed = false;
        while (dir_next(&iterator, &entry)) failed = !remove_entry(path, base, entry, depth) || failed;
        dir_close(&iterator);
        if (!failed && dir_remove(path->text)) return true;
    }
    return false;
}

// The three things a swap moves: fonts/, licenses/ and the executable (last: the program comes in first and goes out
// last, so the gap in which the install has no program is the pair of renames at its end).
struct Item {
    Path live, staged, kept;
    // Moved out of the install into the replaced directory / from the stage into the install.
    bool away, in;
};
constexpr u32 kItems = 3, kExecutable = 2;

// The executable's file name in `paths`, as an index into paths.executable.
u32 executable_leaf(const UpdatePaths& paths) noexcept {
    u32 at = paths.executable_count;
    while (at && paths.executable[at - 1] != L'/') --at;
    return at;
}
bool lay_out(const UpdatePaths& paths, Item* items) noexcept {
    const u32 leaf = executable_leaf(paths);
    if (!leaf) return false;
    const u32 directory = leaf - 1;
    const wchar_t* names[kItems]{kFonts, kLicenses, nullptr};
    const wchar_t* live_leaf = paths.executable + leaf;
    const u32 live_leaf_count = paths.executable_count - leaf;
    const u32 staged_count = paths.staged_count, replaced_count = length(paths.replaced);
    for (u32 i = 0; i < kExecutable; ++i) {
        const u32 n = length(names[i]);
        if (!join(&items[i].live, paths.executable, directory, names[i], n)) return false;
        if (!join(&items[i].staged, paths.staged, staged_count, names[i], n)) return false;
        if (!join(&items[i].kept, paths.replaced, replaced_count, names[i], n)) return false;
    }
    Item& exe = items[kExecutable];
    copy_bytes(exe.live.text, paths.executable, (paths.executable_count + 1) * sizeof(wchar_t));
    exe.live.count = paths.executable_count;
    return join(&exe.staged, paths.staged, staged_count, kStagedExecutable, length(kStagedExecutable)) &&
           join(&exe.kept, paths.replaced, replaced_count, live_leaf, live_leaf_count);
}

// Puts back what the swap moved, newest first; the program's restore error says when even a copy failed.
void restore(Item* items, UpdateSwapResult* result) noexcept {
    for (u32 n = kItems; n--;) {
        if (items[n].in && file_replace(items[n].live.text, items[n].staged.text, true)) items[n].in = false;
    }
    for (u32 n = kItems; n--;) {
        if (!items[n].away) continue;
        if (file_replace(items[n].kept.text, items[n].live.text, true)) { items[n].away = false; continue; }
        if (n == kExecutable) {
            result->restore_error = thread_last_error();
            // The last resort, as on Windows: a copy of the running program.
            if (file_copy_new(items[n].kept.text, items[n].live.text)) result->restore_error = 0;
        }
    }
}

} // namespace

bool update_tree_remove(const wchar_t* path) noexcept {
    if (!path || !*path) return false;
    Path text{};
    text.count = length(path);
    if (text.count >= kPathCharacters) return false;
    copy_bytes(text.text, path, (text.count + 1) * sizeof(wchar_t));
    FileStat stat{};
    if (!file_path_stat(text.text, &stat)) return win32_error_is_missing_file(thread_last_error());
    return stat.kind == FileKind::directory ? remove_directory(&text, 0) : file_remove(text.text);
}

void update_paths_cleanup(const UpdatePaths& paths) noexcept {
    const bool replaced = exists(paths.replaced) && update_tree_remove(paths.replaced);
    const bool stale = exists(paths.staged) && update_tree_remove(paths.staged);
    if (replaced || stale) UpdateLog("cleanup").field("replaced", replaced).field("stale_stage", stale).write();
}

// The swap: the live fonts/, licenses/ and executable move into <executable>.old (a leftover one is deleted first), then
// the staged ones move in, the executable first. Only the entries the stage holds are replaced. A failed move puts
// back everything already moved, so the install always has one whole version.
UpdateSwapResult update_swap(const UpdatePaths& paths) noexcept {
    UpdateSwapResult r{};
    Item items[kItems]{};
    if (!lay_out(paths, items)) { r.error = UpdateSwapError::replace; r.native_error = ERROR_FILENAME_EXCED_RANGE; return r; }
    const auto fail = [&](UpdateSwapError error) noexcept {
        r.error = error; r.native_error = thread_last_error();
        restore(items, &r);
        dir_remove(paths.replaced);
        return r;
    };
    // The stage must hold the program; fonts/ and licenses/ move only when it carries them.
    bool staged[kItems]{};
    for (u32 i = 0; i < kItems; ++i) staged[i] = exists(items[i].staged.text);
    if (!staged[kExecutable] || !exists(items[kExecutable].live.text)) {
        r.error = UpdateSwapError::replace; r.native_error = ERROR_FILE_NOT_FOUND; return r;
    }
    if (exists(paths.replaced) && !update_tree_remove(paths.replaced)) {
        r.error = UpdateSwapError::replace; r.native_error = thread_last_error(); return r;
    }
    if (!dir_create(paths.replaced)) { r.error = UpdateSwapError::replace; r.native_error = thread_last_error(); return r; }
    // The old version goes out: fonts/ and licenses/ where the new one brings its own, then the program.
    for (u32 i = 0; i < kItems; ++i) {
        if (!staged[i] || (i != kExecutable && !exists(items[i].live.text))) continue;
        if (!(i == kExecutable ? file_rename_noreplace(items[i].live.text, items[i].kept.text, true)
                               : file_replace(items[i].live.text, items[i].kept.text, true)))
            return fail(UpdateSwapError::replace);
        items[i].away = true;
    }
    // The new one comes in, the program first.
    const u32 order[kItems]{kExecutable, 0, 1};
    for (const u32 i : order) {
        if (!staged[i]) continue;
        if (!(i == kExecutable ? file_rename_noreplace(items[i].staged.text, items[i].live.text, true)
                               : file_replace(items[i].staged.text, items[i].live.text, true)))
            return fail(UpdateSwapError::install);
        items[i].in = true;
    }
    // Nothing is left in the stage directory; a leftover only waits for the next start's cleanup.
    dir_remove(paths.staged);
    return r;
}

} // namespace maicord
