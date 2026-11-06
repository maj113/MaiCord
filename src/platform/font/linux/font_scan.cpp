// The locator's fallback when libfontconfig cannot be loaded: it walks the font directories (font_config.cpp names
// them) and indexes every face by the family name, weight and slant the text engine itself reads from the file, so
// the catalog that loads the face later agrees with the index about which family it belongs to.
#include "maicord/core/arena.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/os/file.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/text/fonts/family.hpp"

#include "font_config.hpp"
#include "font_names.hpp"
#include "font_scan.hpp"

namespace maicord {

// One face of one font file.
struct ScanFace {
    u32 path;  // offset of the file's UTF-8 path in the string arena
    u32 name;  // offset of the family name's UTF-16 units in the string arena
    u16 name_units;
    u16 index;
    u16 weight;
    u8 italic;
    u8 latin; // the face maps A, a and 0
};

struct SystemFontScan {
    Arena faces;
    Arena strings;
    ScanFace* entries;
    u32 count;
};

namespace font_locator {
namespace {

constexpr u32 kFaceLimit = 32768;
constexpr usize kStringReserve = 12u * 1024u * 1024u;
// Each level of the walk keeps a directory iterator (4 KiB) and a path on the stack.
constexpr u32 kDepthLimit = 6;
constexpr u32 kFileLimit = 100000;
constexpr u32 kPathCapacity = kSystemFontPathCapacity;

struct Walk {
    SystemFontScan* scan;
    u32 files;
};

bool has_extension(const wchar_t* name, u32 units) noexcept {
    if (units < 5 || name[units - 4] != L'.')
        return false;
    const wchar_t* extension = name + units - 3;
    const wchar_t a = extension[0] | 0x20;
    const wchar_t b = extension[1] | 0x20;
    const wchar_t c = extension[2] | 0x20;
    return (a == L't' && b == L't' && (c == L'f' || c == L'c')) || (a == L'o' && b == L't' && (c == L'f' || c == L'c'));
}

bool join(wchar_t (&path)[kPathCapacity], const wchar_t* parent, const wchar_t* name) noexcept {
    u32 length = 0;
    path[0] = 0;
    if (!wide_append(path, kPathCapacity, &length, parent))
        return false;
    if (length && path[length - 1] != L'/' && !wide_append(path, kPathCapacity, &length, L"/"))
        return false;
    return wide_append(path, kPathCapacity, &length, name);
}

// Stores the NUL-terminated UTF-8 form of the UTF-16 path; its offset in the string arena.
bool store_path(SystemFontScan* scan, const wchar_t* path, u32* offset) noexcept {
    char utf8[4 * kPathCapacity];
    // path_to_utf8 takes the converted length it may write: a stack buffer of the maximum.
    const u32 bytes = path_to_utf8(path, utf8, sizeof(utf8));
    if (bytes == 0)
        return false;
    auto* stored = static_cast<u8*>(arena_allocate(&scan->strings, bytes + 1, 1));
    if (!stored)
        return false;
    copy_bytes(stored, utf8, bytes + 1);
    *offset = static_cast<u32>(stored - scan->strings.base);
    return true;
}

bool store_name(SystemFontScan* scan, const u16* units, u32 count, u32* offset) noexcept {
    auto* stored = static_cast<u16*>(arena_allocate(&scan->strings, count * sizeof(u16), alignof(u16)));
    if (!stored)
        return false;
    copy_bytes(stored, units, count * static_cast<u32>(sizeof(u16)));
    *offset = static_cast<u32>(reinterpret_cast<u8*>(stored) - scan->strings.base);
    return true;
}

// Indexes every face of one file.
void add_file(Walk& walk, const wchar_t* path) noexcept {
    const FileHandle file = file_open(path, {FileAccess::read, FileCreate::existing,
                                             kFileShareRead | kFileShareWrite | kFileShareDelete, kFileRandom});
    if (file == INVALID_HANDLE_VALUE)
        return;
    struct Close {
        FileHandle file;
        ~Close() noexcept { file_close(file); }
    } close{file};
    FileStat stat{};
    if (!file_stat(file, &stat) || stat.kind != FileKind::regular || stat.bytes < 64 || stat.bytes > 0xFFFFFFFFull)
        return;
    FileMap map{};
    if (!file_map(file, false, 0, &map))
        return;
    struct Unmap {
        FileMap* map;
        ~Unmap() noexcept { file_unmap(map); }
    } unmap{&map};
    const ConstSlice<u8> bytes{static_cast<const u8*>(map.data), static_cast<u32>(map.bytes)};
    u32 path_offset = 0;
    bool stored_path = false;
    SfntFace first{};
    if (sfnt_open(bytes, 0, &first) != SfntStatus::ok)
        return;
    for (u32 index = 0; index < first.face_count && index < 0xFFFF; ++index) {
        if (walk.scan->count >= kFaceLimit)
            return;
        SfntFace face{};
        if (sfnt_open(bytes, index, &face) != SfntStatus::ok)
            continue;
        u16 name[512];
        NativeFontFamily family{};
        if (native_font_family(face, {name, 512}, &family) != SfntStatus::ok || family.units == 0 ||
            family.units > kSystemFontNameCapacity)
            continue;
        SfntCmap cmap{};
        bool latin = false;
        if (sfnt_cmap_open(face, &cmap) == SfntStatus::ok) {
            u16 a = 0, b = 0, c = 0;
            latin = sfnt_cmap_glyph(cmap, 'A', 0, &a) == SfntStatus::ok && a != 0 &&
                    sfnt_cmap_glyph(cmap, 'a', 0, &b) == SfntStatus::ok && b != 0 &&
                    sfnt_cmap_glyph(cmap, '0', 0, &c) == SfntStatus::ok && c != 0;
        }
        if (!stored_path) {
            if (!store_path(walk.scan, path, &path_offset))
                return;
            stored_path = true;
        }
        ScanFace entry{};
        if (!store_name(walk.scan, name, family.units, &entry.name))
            return;
        entry.path = path_offset;
        entry.name_units = static_cast<u16>(family.units);
        entry.index = static_cast<u16>(index);
        entry.weight = static_cast<u16>(family.properties.weight);
        entry.italic = family.properties.style != 0;
        entry.latin = latin;
        auto* slot = static_cast<ScanFace*>(arena_allocate(&walk.scan->faces, sizeof(ScanFace), alignof(ScanFace)));
        if (!slot)
            return;
        *slot = entry;
        ++walk.scan->count;
    }
}

void walk_directory(Walk& walk, const wchar_t* directory, u32 depth) noexcept;

// An entry that may be a link: a directory when it opens as one, a font file otherwise.
void visit(Walk& walk, const wchar_t* path, const wchar_t* name, FileKind kind, u32 depth) noexcept {
    if (kind == FileKind::directory) {
        walk_directory(walk, path, depth + 1);
        return;
    }
    if (kind == FileKind::link) {
        DirIter probe{};
        if (dir_open(&probe, path)) {
            dir_close(&probe);
            walk_directory(walk, path, depth + 1);
            return;
        }
    } else if (kind != FileKind::regular) {
        return;
    }
    u32 units = 0;
    while (name[units])
        ++units;
    if (!has_extension(name, units) || ++walk.files > kFileLimit)
        return;
    add_file(walk, path);
}

void walk_directory(Walk& walk, const wchar_t* directory, u32 depth) noexcept {
    if (depth > kDepthLimit || walk.files > kFileLimit)
        return;
    DirIter iterator{};
    if (!dir_open(&iterator, directory))
        return;
    DirEntry entry;
    while (dir_next(&iterator, &entry)) {
        wchar_t path[kPathCapacity];
        if (!join(path, directory, entry.name))
            continue;
        visit(walk, path, entry.name, entry.kind, depth);
    }
    dir_close(&iterator);
}

SystemFontScan* build(SystemFontLocator* locator) noexcept {
    if (locator->scan)
        return locator->scan;
    if (locator->scan_failed)
        return nullptr;
    auto* scan = static_cast<SystemFontScan*>(pages_map(sizeof(SystemFontScan), true));
    if (!scan) {
        locator->scan_failed = true;
        return nullptr;
    }
    if (!arena_initialize(&scan->faces, kFaceLimit * sizeof(ScanFace), 0, 64 * 1024, ArenaTag::platform) ||
        !arena_initialize(&scan->strings, kStringReserve, 0, 256 * 1024, ArenaTag::platform)) {
        arena_release(&scan->faces);
        pages_unmap(scan, sizeof(SystemFontScan));
        locator->scan_failed = true;
        return nullptr;
    }
    scan->entries = reinterpret_cast<ScanFace*>(scan->faces.base);
    Arena roots_arena{};
    if (arena_initialize(&roots_arena, 4u * 1024u * 1024u, 0, 64 * 1024, ArenaTag::platform)) {
        FontRoots* roots = static_cast<FontRoots*>(arena_allocate_zero(&roots_arena, sizeof(FontRoots), alignof(FontRoots)));
        if (roots) {
            font_roots_collect(&roots_arena, roots);
            Walk walk{scan, 0};
            for (u32 i = 0; i < roots->count; ++i)
                walk_directory(walk, roots->paths[i], 0);
        }
        arena_release(&roots_arena);
    }
    locator->scan = scan;
    log_u64(LogLevel::info, "fonts", "scanned_faces", scan->count);
    return scan;
}

const wchar_t* as_wide(const u16* units) noexcept {
    return reinterpret_cast<const wchar_t*>(units);
}

const ScanFace* best_face(const SystemFontScan& scan, ConstSlice<u16> family, u32 weight, bool italic) noexcept {
    const ScanFace* best = nullptr;
    u32 best_distance = 0xFFFFFFFFu;
    for (u32 i = 0; i < scan.count; ++i) {
        const ScanFace& face = scan.entries[i];
        const u16* name = reinterpret_cast<const u16*>(scan.strings.base + face.name);
        if (!ordinal_equal(as_wide(name), face.name_units, as_wide(family.data), static_cast<int>(family.count), true))
            continue;
        const u32 distance = (face.weight > weight ? face.weight - weight : weight - face.weight) +
                             ((face.italic != 0) != italic ? 1000u : 0u);
        if (distance < best_distance) {
            best = &face;
            best_distance = distance;
        }
    }
    return best;
}

bool fill(const SystemFontScan& scan, const ScanFace& face, SystemFontFile* output) noexcept {
    SystemFontFile located{};
    const char* path = reinterpret_cast<const char*>(scan.strings.base + face.path);
    if (path_from_utf8(path, located.path, kSystemFontPathCapacity) == 0)
        return false;
    located.index = face.index;
    *output = located;
    return true;
}

// The families that make good interface, serif and monospace defaults, in order of preference.
constexpr const char* const kUi[] = {"Inter",     "Noto Sans",     "Adwaita Sans", "Cantarell",
                                     "Ubuntu Sans", "Ubuntu",       "DejaVu Sans",  "Liberation Sans"};
constexpr const char* const kSerif[] = {"Noto Serif", "DejaVu Serif", "Liberation Serif"};
constexpr const char* const kMonospace[] = {"Noto Sans Mono", "DejaVu Sans Mono", "Liberation Mono", "Ubuntu Mono",
                                            "JetBrains Mono"};
constexpr const char* const kEmoji[] = {"Noto Color Emoji"};

bool listed(const SystemFontScan& scan, const char* family, SystemFontName* name) noexcept {
    SystemFontName candidate{};
    if (!utf8_to_name(family, &candidate))
        return false;
    if (!best_face(scan, {candidate.units, candidate.count}, 400, false))
        return false;
    *name = candidate;
    return true;
}

template <usize Count> u32 first_listed(const SystemFontScan& scan, const char* const (&families)[Count],
                                        SystemFontName* name) noexcept {
    for (const char* family : families)
        if (listed(scan, family, name))
            return name->count;
    return 0;
}

} // namespace

bool scan_find(SystemFontLocator* locator, const SystemFontRequest& request, SystemFontFile* output) noexcept {
    const SystemFontScan* scan = build(locator);
    if (!scan)
        return false;
    const ScanFace* face = best_face(*scan, request.family, request.weight, request.italic);
    return face && fill(*scan, *face, output);
}

u32 scan_role(SystemFontLocator* locator, SystemFontRole role, SystemFontName* output) noexcept {
    const SystemFontScan* scan = build(locator);
    if (!scan)
        return 0;
    SystemFontName name{};
    u32 count = 0;
    switch (role) {
    case SystemFontRole::ui:
        count = first_listed(*scan, kUi, &name);
        if (count == 0) {
            // The first regular face that has Latin letters.
            const ScanFace* best = nullptr;
            for (u32 i = 0; i < scan->count; ++i) {
                const ScanFace& face = scan->entries[i];
                if (!face.latin || face.italic)
                    continue;
                const u32 distance = face.weight > 400 ? face.weight - 400u : 400u - face.weight;
                if (!best || distance < (best->weight > 400 ? best->weight - 400u : 400u - best->weight))
                    best = &face;
            }
            if (best) {
                const u16* units = reinterpret_cast<const u16*>(scan->strings.base + best->name);
                for (u32 i = 0; i < best->name_units; ++i)
                    name.units[i] = units[i];
                name.count = best->name_units;
                count = name.count;
            }
        }
        break;
    case SystemFontRole::serif:
        count = first_listed(*scan, kSerif, &name);
        break;
    case SystemFontRole::monospace:
        count = first_listed(*scan, kMonospace, &name);
        break;
    case SystemFontRole::emoji:
        count = first_listed(*scan, kEmoji, &name);
        break;
    default:
        break;
    }
    if (count)
        *output = name;
    return count;
}

void scan_release(SystemFontLocator* locator) noexcept {
    SystemFontScan* scan = locator->scan;
    if (!scan)
        return;
    locator->scan = nullptr;
    arena_release(&scan->faces);
    arena_release(&scan->strings);
    pages_unmap(scan, sizeof(SystemFontScan));
}

} // namespace font_locator
} // namespace maicord
