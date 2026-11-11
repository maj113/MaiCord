#include "maicord/app/plugins/http.hpp"
#include "maicord/app/plugins/loader.hpp"
#include "maicord/app/plugins/text.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/path.hpp"
#include "maicord/platform/crypto/hash_stream.hpp"
#if defined(_WIN32)
#include "maicord/core/scratch_pages.hpp"
#else
#include "maicord/os/text.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/libc.hpp"
#endif

namespace maicord {
namespace {

#if defined(_WIN32)
// PE32+ header offsets (little endian). The headers are read as bytes: a loader
// that parses untrusted files gains nothing from overlaying SDK structs.
constexpr u32 kHeaderBytes = 4096;
constexpr u32 kNtSignature = 0x00004550;
constexpr u16 kMachineAmd64 = 0x8664;
constexpr u16 kFileExecutable = 0x0002;
constexpr u16 kFileDll = 0x2000;
constexpr u16 kOptionalMagic64 = 0x20b;
constexpr u32 kOptionalHeaderFixed = 112;
constexpr u32 kSectionHeaderBytes = 40;
constexpr u32 kSectionExecute = 0x20000000;
constexpr u32 kSectionWrite = 0x80000000;
constexpr u32 kDirectoryExport = 0;
constexpr u32 kDirectoryTls = 9;
constexpr u32 kExportDirectoryBytes = 40;
constexpr u32 kExportNameBytes = sizeof(MC_DESCRIPTOR_EXPORT);
#endif

PluginLoadResult failure(PluginLoadError error, DWORD win32_error = 0) noexcept {
    return {error, PluginDescriptorProblem::none, 0, win32_error};
}
PluginLoadResult problem(PluginDescriptorProblem which, u16 index = 0) noexcept {
    return {PluginLoadError::descriptor, which, index, 0};
}
PluginLoadResult success() noexcept {
    return {};
}

// The longest file name a plugin may have, in characters.
constexpr u32 kLeafMaximumCharacters = 128;

#if defined(_WIN32)
using SystemError = DWORD;
#else
using SystemError = i32;
#endif

// Whether the file's bytes, read once into memory, hash to expected (32 bytes of SHA-256).
bool digest_equals(const u8* bytes, u64 count, const u8* expected) noexcept {
    HashStream hash = nullptr;
    if (hash_stream_begin(&hash, HashAlgorithm::sha256) < 0) return false;
    u8 digest[32];
    const bool match = hash_stream_update(hash, bytes, static_cast<u32>(count)) >= 0 &&
                       hash_stream_finish(hash, digest, sizeof(digest)) >= 0 && bytes_equal(digest, expected, sizeof(digest));
    hash_stream_destroy(hash);
    return match;
}

#if defined(_WIN32)
struct Section {
    u32 rva, virtual_bytes, raw_offset, raw_bytes, flags;
};

// The file offset of a byte range that lies in one section's raw data.
bool rva_to_offset(const Section* sections, u32 count, u32 rva, u32 bytes, u64* offset) noexcept {
    for (u32 i = 0; i < count; ++i) {
        const Section& s = sections[i];
        if (rva < s.rva || rva - s.rva > s.raw_bytes || bytes > s.raw_bytes - (rva - s.rva)) continue;
        *offset = static_cast<u64>(s.raw_offset) + (rva - s.rva);
        return true;
    }
    return false;
}
const Section* section_of(const Section* sections, u32 count, u32 rva) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (rva >= sections[i].rva && rva - sections[i].rva < sections[i].virtual_bytes) return &sections[i];
    return nullptr;
}
#endif

// Descriptor-side string and pointer checks, relative to the mapped image.
struct Image {
    const u8* base;
    u32 bytes;
    const PluginImage* pe;

    bool contains(const void* pointer, u64 length) const noexcept {
        const u64 address = reinterpret_cast<usize>(pointer), start = reinterpret_cast<usize>(base);
        return address >= start && address - start <= bytes && length <= bytes - (address - start);
    }
    bool code(const void* function) const noexcept {
        if (!contains(function, 1)) return false;
        const u32 rva = static_cast<u32>(reinterpret_cast<usize>(function) - reinterpret_cast<usize>(base));
        for (u32 i = 0; i < pe->executable_count; ++i)
            if (rva >= pe->executable[i].begin && rva < pe->executable[i].end) return true;
        return false;
    }
    // A string that lives in the image: UTF-8 without control characters.
    bool text(const mc_str& string, u32 minimum, u32 maximum, bool newlines = false) const noexcept {
        if (string.reserved || string.bytes < minimum || string.bytes > maximum) return false;
        if (!string.bytes) return true;
        return contains(string.data, string.bytes) && plugin_text_clean(string.data, string.bytes, newlines ? kPluginTextLines : 0);
    }
};

bool key_ok(const mc_str& key, const Image& image) noexcept {
    if (!image.text(key, 1, MC_SETTING_KEY_MAX)) return false;
    for (u32 i = 0; i < key.bytes; ++i) {
        const char c = key.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

u32 setting_bytes(u32 type) noexcept {
    switch (type) {
    case MC_SET_TOGGLE: return 1;
    case MC_SET_INT:
    case MC_SET_SLIDER:
    case MC_SET_CHOICE: return 4;
    case MC_SET_COLOUR:
    case MC_SET_KEYBIND: return 4;
    case MC_SET_CHANNEL: return 8;
    case MC_SET_USER_LIST: return sizeof(mc_ids);
    case MC_SET_TEXT:
    case MC_SET_SECRET:
    case MC_SET_TEXT_LIST: return sizeof(mc_str);
    default: return 0;
    }
}
u32 setting_alignment(u32 type) noexcept {
    return setting_bytes(type) >= 8 ? 8 : setting_bytes(type);
}

bool setting_ok(const mc_setting_def& s, u32 index, const mc_setting_def* all, u32 count, u32 block_bytes,
                const Image& image) noexcept {
    const u32 type = s.type;
    if ((s.flags & ~(MC_SET_HIDDEN | MC_SET_DANGER | MC_SET_MULTILINE)) ||
        !key_ok(s.key, image) || !image.text(s.label, 1, MC_SETTING_LABEL_MAX) ||
        !image.text(s.description, 0, MC_SETTING_TEXT_MAX, true) || !image.text(s.hint, 0, MC_SETTING_LABEL_MAX))
        return false;
    // visible_if names another TOGGLE (visible_value 0) or a CHOICE and one of its items.
    if (s.visible_if) {
        if (s.visible_if > count || s.visible_if == index + 1) return false;
        const mc_setting_def& gate = all[s.visible_if - 1];
        if (gate.type == MC_SET_TOGGLE ? s.visible_value != 0
                                       : gate.type != MC_SET_CHOICE || s.visible_value >= gate.item_count)
            return false;
    } else if (s.visible_value) {
        return false;
    }
    const bool lists = type == MC_SET_CHOICE || type == MC_SET_TEXT_LIST;
    if (s.item_count > MC_SETTING_ITEM_MAX || (s.item_count && (!lists || !image.contains(s.items,
            static_cast<u64>(s.item_count) * sizeof(mc_str)))) || (!s.item_count && s.items)) return false;
    const u32 maximum_text = s.max_bytes ? s.max_bytes : MC_SETTING_TEXT_MAX;
    if (maximum_text > MC_SETTING_TEXT_MAX ||
        (s.max_bytes && type != MC_SET_TEXT && type != MC_SET_SECRET && type != MC_SET_TEXT_LIST))
        return false;
    if (type == MC_SET_HEADER) return !s.offset && !s.default_text.bytes && !s.default_int && !s.minimum &&
                                      !s.maximum && !s.step;
    const u32 bytes = setting_bytes(type);
    if (!bytes || s.offset % setting_alignment(type) || s.offset > block_bytes || bytes > block_bytes - s.offset)
        return false;
    // Settings must not share bytes: a write for one would corrupt another.
    for (u32 j = 0; j < index; ++j) {
        const u32 other = setting_bytes(all[j].type);
        if (other && s.offset < all[j].offset + other && all[j].offset < s.offset + bytes) return false;
        if (all[j].key.bytes == s.key.bytes && bytes_equal(all[j].key.data, s.key.data, s.key.bytes)) return false;
    }
    switch (type) {
    case MC_SET_TOGGLE:
        return s.default_int == 0 || s.default_int == 1;
    case MC_SET_INT:
        return s.minimum <= s.maximum && s.default_int >= s.minimum && s.default_int <= s.maximum;
    case MC_SET_SLIDER:
        return s.minimum < s.maximum && s.step > 0 && s.default_int >= s.minimum && s.default_int <= s.maximum &&
               static_cast<i64>(s.step) <= static_cast<i64>(s.maximum) - s.minimum;
    case MC_SET_CHOICE:
        if (!s.item_count || s.default_int < 0 || static_cast<u32>(s.default_int) >= s.item_count) return false;
        for (u32 i = 0; i < s.item_count; ++i)
            if (!image.text(s.items[i], 1, MC_SETTING_LABEL_MAX)) return false;
        return true;
    case MC_SET_TEXT:
        return image.text(s.default_text, 0, maximum_text, s.flags & MC_SET_MULTILINE);
    case MC_SET_SECRET: // no default, never multi-line
        return !s.default_text.bytes && !s.default_text.data && !(s.flags & MC_SET_MULTILINE);
    case MC_SET_COLOUR:
        return s.default_int >= 0 && s.default_int <= 0xFFFFFF;
    case MC_SET_KEYBIND: return plugin_keybind_valid(static_cast<u32>(s.default_int));
    case MC_SET_CHANNEL:
    case MC_SET_USER_LIST:
        return !s.default_int;
    default: { // MC_SET_TEXT_LIST
        u32 total = 0;
        for (u32 i = 0; i < s.item_count; ++i) {
            if (!image.text(s.items[i], 1, maximum_text)) return false;
            total += s.items[i].bytes;
        }
        return total <= MC_SETTING_LIST_BYTES_MAX;
    }
    }
}

// A descriptor array: count within its limit, inside the image, and null exactly when empty.
bool table_ok(const Image& image, const void* table, u32 count, u32 maximum, u32 element) noexcept {
    return count <= maximum && (count ? image.contains(table, static_cast<u64>(count) * element) : !table);
}

bool hook_ok(const mc_hook& hook, const Image& image) noexcept {
    if (!image.code(reinterpret_cast<const void*>(hook.fn.draft))) return false;
    switch (hook.kind) {
    case MC_HOOK_MESSAGE_CLICK:
        return !hook.flags && !(hook.filter & ~u64{MC_CLICK_FILTER_LEFT | MC_CLICK_FILTER_DOUBLE |
                                                   MC_CLICK_FILTER_MIDDLE});
    case MC_HOOK_EVENTS: return !hook.flags && hook.filter && !(hook.filter & ~MC_EVENT_KINDS);
    case MC_HOOK_DECORATE_MESSAGE:
        return !(hook.flags & ~(MC_NEED_CONTENT | MC_NEED_AUTHOR)) &&
               !(hook.filter & ~u64{MC_FILTER_GUILD_ONLY | MC_FILTER_DM_ONLY | MC_FILTER_HAS_MEDIA |
                                    MC_FILTER_NOT_OWN}) &&
               (hook.filter & (MC_FILTER_GUILD_ONLY | MC_FILTER_DM_ONLY)) !=
                   (MC_FILTER_GUILD_ONLY | MC_FILTER_DM_ONLY);
    case MC_HOOK_DECORATE_USER:
        return !hook.flags && !(hook.filter & ~u64{(1u << MC_USER_MEMBER_ROW) | (1u << MC_USER_MESSAGE_AUTHOR) |
                                                   (1u << MC_USER_PROFILE)});
    default: return hook.kind >= 1 && hook.kind <= MC_HOOK_LAST && !hook.flags && !hook.filter;
    }
}

// Command and argument names: 1..MC_COMMAND_NAME_MAX bytes of [a-z0-9_-].
bool command_name_ok(const mc_str& name, const Image& image) noexcept {
    if (!image.text(name, 1, MC_COMMAND_NAME_MAX)) return false;
    for (u32 i = 0; i < name.bytes; ++i) {
        const char c = name.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}
bool same_text(const mc_str& a, const mc_str& b) noexcept {
    return a.bytes == b.bytes && bytes_equal(a.data, b.data, a.bytes);
}

bool argument_ok(const mc_arg_def& a, bool required_allowed, const Image& image) noexcept {
    if (a.reserved || (a.flags & ~MC_ARG_REQUIRED) || (!required_allowed && (a.flags & MC_ARG_REQUIRED)) ||
        a.type < MC_ARG_TEXT || a.type > MC_ARG_CHANNEL || !command_name_ok(a.name, image) ||
        !image.text(a.description, 1, MC_COMMAND_DESCRIPTION_MAX))
        return false;
    const bool bounded = a.minimum || a.maximum;
    if (a.type == MC_ARG_INT ? bounded && a.minimum > a.maximum
                             : bounded && (a.type != MC_ARG_TEXT || a.minimum < 0 || a.minimum > a.maximum))
        return false;
    if (!table_ok(image, a.choices, a.choice_count, MC_ARG_CHOICES_MAX, sizeof(mc_str)) ||
        (a.choice_count && a.type != MC_ARG_TEXT))
        return false;
    for (u32 i = 0; i < a.choice_count; ++i)
        if (!image.text(a.choices[i], 1, MC_COMMAND_DESCRIPTION_MAX)) return false;
    return true;
}

bool command_ok(const mc_command_def& c, const mc_command_def* earlier, u32 index, const Image& image) noexcept {
    if ((c.flags & ~MC_COMMAND_GUILD_ONLY) || !command_name_ok(c.name, image) ||
        !image.text(c.description, 1, MC_COMMAND_DESCRIPTION_MAX) ||
        !image.code(reinterpret_cast<const void*>(c.run)) ||
        !table_ok(image, c.args, c.arg_count, MC_COMMAND_ARGS_MAX, sizeof(mc_arg_def)))
        return false;
    for (u32 j = 0; j < index; ++j)
        if (same_text(earlier[j].name, c.name)) return false;
    bool required_allowed = true; // required arguments come first
    for (u32 i = 0; i < c.arg_count; ++i) {
        if (!argument_ok(c.args[i], required_allowed, image)) return false;
        required_allowed = (c.args[i].flags & MC_ARG_REQUIRED) != 0;
        for (u32 j = 0; j < i; ++j)
            if (same_text(c.args[j].name, c.args[i].name)) return false;
    }
    return true;
}

bool menu_ok(const mc_menu_def& m, const mc_menu_def* all, u32 index, const Image& image) noexcept {
    const bool submenu = (m.flags & MC_MENU_SUBMENU) != 0;
    if (m.reserved || (m.flags & ~(MC_MENU_CHECKBOX | MC_MENU_DANGER | MC_MENU_SUBMENU)) ||
        (submenu && (m.flags & MC_MENU_CHECKBOX)) || !m.targets || (m.targets & ~MC_MENU_TARGETS) ||
        !image.text(m.label, 1, MC_MENU_LABEL_MAX) ||
        (m.visible && !image.code(reinterpret_cast<const void*>(m.visible))) ||
        (submenu ? m.select != nullptr : !image.code(reinterpret_cast<const void*>(m.select))))
        return false;
    if (!m.parent) return true;
    // One level: the parent is an earlier top-level submenu whose targets include the child's.
    if (submenu || m.parent > index) return false;
    const mc_menu_def& parent = all[m.parent - 1];
    return (parent.flags & MC_MENU_SUBMENU) && !parent.parent && !(m.targets & ~parent.targets);
}

bool button_ok(const mc_button_def& b, const Image& image) noexcept {
    return b.surface >= MC_SURFACE_CHAT_BAR && b.surface <= MC_SURFACE_SERVER_LIST_BOTTOM &&
           !(b.flags & ~(MC_BUTTON_TOGGLE | MC_BUTTON_DANGER)) && image.text(b.label, 1, MC_BUTTON_LABEL_MAX) &&
           image.text(b.icon, 0, MC_BUTTON_LABEL_MAX) && image.code(reinterpret_cast<const void*>(b.click)) &&
           (!b.visible || image.code(reinterpret_cast<const void*>(b.visible)));
}

#if defined(_WIN32)
// ---- The pinned directory and the file opened beside it -------------------------------------------

// CON, PRN, AUX, NUL, COM1-9 and LPT1-9 in any case: Windows device names, with or without an extension.
bool reserved_device_name(const wchar_t* name, u32 stem) noexcept {
    const auto is = [name](const char* word) noexcept {
        return ascii_lower(name[0]) == word[0] && ascii_lower(name[1]) == word[1] && ascii_lower(name[2]) == word[2];
    };
    if (stem == 3) return is("con") || is("prn") || is("aux") || is("nul");
    return stem == 4 && name[3] >= '1' && name[3] <= '9' && (is("com") || is("lpt"));
}
// One Windows path component naming an ordinary file or directory: nonempty, no leading or trailing dot,
// no trailing space, no reserved device name and no control, delimiter, wildcard, '~' or surrogate unit.
bool path_component_valid(ConstSlice<wchar_t> part) noexcept {
    if (!part.count || part[0] == L'.' || part[part.count - 1] == L'.' || part[part.count - 1] == L' ') return false;
    u32 stem = 0;
    while (stem < part.count && part[stem] != L'.') ++stem;
    if (reserved_device_name(part.data, stem)) return false;
    for (u32 i = 0; i < part.count; ++i) {
        const wchar_t c = part[i];
        if (c < 32 || c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' ||
            c == L'>' || c == L'|' || c == L'~' || (c >= 127 && c <= 159) || (c >= 0xd800 && c <= 0xdfff))
            return false;
    }
    return true;
}
// A drive-rooted path whose every component is an ordinary name, at most kPluginPathMaximumAncestors deep.
bool directory_valid(ConstSlice<wchar_t> path) noexcept {
    if (!path.data || path.count < 3 || path.count >= kPluginPathCapacity || path_root(path) != PathRoot::drive)
        return false;
    u32 start = 3, ancestors = 1;
    for (u32 i = 3; i <= path.count; ++i)
        if (i == path.count || path[i] == L'\\') {
            if (i == 3 && path.count == 3) return true;
            if (!path_component_valid({path.data + start, i - start}) || ++ancestors > kPluginPathMaximumAncestors)
                return false;
            start = i + 1;
        }
    return true;
}
// Closes the held ancestors, deepest first. False (with the error) leaves the rest held for a retry.
bool directory_release(PluginDirectory* directory, DWORD* error) noexcept {
    while (directory->ancestor_count) {
        if (!CloseHandle(directory->ancestors[directory->ancestor_count - 1])) {
            *error = GetLastError();
            return false;
        }
        directory->ancestors[--directory->ancestor_count] = nullptr;
    }
    reset_value(directory);
    return true;
}
// Every directory component is opened without following its own reparse point, then held while descendants
// are opened: a checked ancestor cannot be renamed or replaced between the hash and the DLL load.
bool directory_pin(PluginDirectory* directory, ConstSlice<wchar_t> path, DWORD* error) noexcept {
    if (!directory_valid(path)) return false;
    copy_bytes(directory->path, path.data, path.count * 2u);
    directory->path[path.count] = 0;
    directory->path_characters = path.count;
    for (u32 end = 3; end <= path.count; ++end) {
        if (end != 3 && end != path.count && path[end] != L'\\') continue;
        if (end == 3 && path.count == 3 && directory->ancestor_count) continue;
        const wchar_t saved = directory->path[end];
        directory->path[end] = 0;
        HANDLE handle = CreateFileW(directory->path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        directory->path[end] = saved;
        if (handle == INVALID_HANDLE_VALUE) {
            *error = GetLastError();
            return false;
        }
        directory->ancestors[directory->ancestor_count++] = handle;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info)) {
            *error = GetLastError();
            return false;
        }
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
    }
    return true;
}

// A DLL opened beside the pinned directory; the handle denies writes and deletes while it is open.
struct PinnedFile {
    HANDLE file;
    wchar_t path[kPluginPathCapacity];
    u32 path_characters;
    u64 bytes;
    u8* view; // the file's bytes, read once (file_view)
};
bool file_open(const PluginDirectory& directory, ConstSlice<wchar_t> leaf, PinnedFile* out, DWORD* error) noexcept {
    if (!directory.ancestor_count || !leaf.data || leaf.count >= kLeafMaximumCharacters || !path_component_valid(leaf))
        return false;
    const u32 separator = directory.path_characters == 3 ? 0 : 1;
    if (directory.path_characters + separator + leaf.count >= kPluginPathCapacity) return false;
    copy_bytes(out->path, directory.path, directory.path_characters * 2u);
    u32 at = directory.path_characters;
    if (separator) out->path[at++] = L'\\';
    for (u32 i = 0; i < leaf.count; ++i) out->path[at++] = leaf[i];
    out->path[at] = 0;
    out->path_characters = at;
    out->file = CreateFileW(out->path, GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (out->file == INVALID_HANDLE_VALUE) {
        out->file = nullptr;
        *error = GetLastError();
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(out->file) != FILE_TYPE_DISK || !GetFileInformationByHandle(out->file, &info)) {
        *error = GetLastError();
        return false;
    }
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    // Reject DOS drive aliases and any normalized target that left the exact pinned drive-rooted path.
    wchar_t final_path[kPluginPathCapacity + 4];
    const DWORD final_count = GetFinalPathNameByHandleW(out->file, final_path, kPluginPathCapacity + 4,
                                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!final_count) {
        *error = GetLastError();
        return false;
    }
    if (final_count != out->path_characters + 4 || final_path[0] != L'\\' || final_path[1] != L'\\' ||
        final_path[2] != L'?' || final_path[3] != L'\\')
        return false;
    for (u32 i = 0; i < out->path_characters; ++i)
        if (ascii_lower(final_path[i + 4]) != ascii_lower(out->path[i])) return false;
    const u64 bytes = (static_cast<u64>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (!bytes) return false;
    out->bytes = bytes;
    return true;
}
// Reads the whole file once: these bytes are hashed and checked, and the system loader maps the same ones (the open
// handle keeps the file from being written). One byte more is asked for, so a file longer than it was is refused.
PluginLoadResult file_view(PinnedFile* file, ConstSlice<wchar_t>) noexcept {
    file->view = static_cast<u8*>(commit_pages(file->bytes + 1));
    if (!file->view) return failure(PluginLoadError::read, GetLastError());
    DWORD read = 0;
    if (!ReadFile(file->file, file->view, static_cast<DWORD>(file->bytes + 1), &read, nullptr))
        return failure(PluginLoadError::read, GetLastError());
    return read == file->bytes ? success() : failure(PluginLoadError::read);
}
PluginLoadResult image_map(const PinnedFile& file, u64, HMODULE* module, const u8** base) noexcept {
    *module = LoadLibraryExW(file.path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!*module) return failure(PluginLoadError::load, GetLastError());
    *base = reinterpret_cast<const u8*>(*module);
    return success();
}
bool image_free(HMODULE module, DWORD* error) noexcept {
    if (FreeLibrary(module)) return true;
    *error = GetLastError();
    return false;
}
void file_close(PinnedFile* file, PluginImage*) noexcept {
    if (file->view) pages_release(file->view);
    // A failed handle close cannot be acted on here: the image is already mapped or already refused.
    if (file->file) CloseHandle(file->file);
}

// The PE headers, on the file's bytes: an x64 DLL with no entry point and no TLS, whose sections ascend, lie in the
// file and are never writable and executable, and whose one export is the descriptor, by name, as data. Fills the
// image's executable ranges and size, and *descriptor with the export's RVA.
PluginLoadResult image_check(const u8* file, u64 file_bytes, PluginImage* output, u64* descriptor) noexcept {
    if (file_bytes < 64) return failure(PluginLoadError::not_pe);
    const u8* headers = file;
    const u32 header_bytes = file_bytes < kHeaderBytes ? static_cast<u32>(file_bytes) : kHeaderBytes;
    if (byte_load_le16(headers) != 0x5A4D) return failure(PluginLoadError::not_pe);
    const u32 nt = byte_load_le32(headers + 0x3C);
    if (nt < 64 || nt > header_bytes || header_bytes - nt < 24 + kOptionalHeaderFixed ||
        byte_load_le32(headers + nt) != kNtSignature) return failure(PluginLoadError::not_pe);
    const u8* file_header = headers + nt + 4;
    const u16 characteristics = byte_load_le16(file_header + 18);
    const u32 section_count = byte_load_le16(file_header + 2), optional_bytes = byte_load_le16(file_header + 16);
    if (byte_load_le16(file_header) != kMachineAmd64) return failure(PluginLoadError::machine);
    if (!(characteristics & kFileDll) || !(characteristics & kFileExecutable)) return failure(PluginLoadError::not_pe);
    const u8* optional = headers + nt + 24;
    if (optional_bytes < kOptionalHeaderFixed || optional_bytes > header_bytes - nt - 24 ||
        byte_load_le16(optional) != kOptionalMagic64) return failure(PluginLoadError::machine);
    const u32 directory_count = byte_load_le32(optional + 108);
    if (directory_count > 16 || optional_bytes < kOptionalHeaderFixed + directory_count * 8)
        return failure(PluginLoadError::not_pe);
    if (byte_load_le32(optional + 16) != 0) return failure(PluginLoadError::entry_point);
    if (directory_count > kDirectoryTls) {
        const u8* tls = optional + kOptionalHeaderFixed + kDirectoryTls * 8;
        if (byte_load_le32(tls) || byte_load_le32(tls + 4)) return failure(PluginLoadError::tls);
    }

    const u32 section_alignment = byte_load_le32(optional + 32), file_alignment = byte_load_le32(optional + 36);
    const u32 image_bytes = byte_load_le32(optional + 56), headers_bytes = byte_load_le32(optional + 60);
    if (!is_power_of_two(section_alignment) || section_alignment < 0x1000 || !is_power_of_two(file_alignment) ||
        file_alignment < 0x200 || file_alignment > 0x10000 || image_bytes < section_alignment ||
        image_bytes > kPluginImageMaximumBytes || image_bytes % section_alignment || headers_bytes > file_bytes ||
        headers_bytes > image_bytes)
        return failure(PluginLoadError::sections);
    const u64 table = static_cast<u64>(nt) + 24 + optional_bytes;
    if (!section_count || section_count > kPluginMaximumSections ||
        table + static_cast<u64>(section_count) * kSectionHeaderBytes > header_bytes)
        return failure(PluginLoadError::sections);

    Section sections[kPluginMaximumSections];
    u32 executable = 0, previous_end = headers_bytes;
    for (u32 i = 0; i < section_count; ++i) {
        const u8* record = headers + table + i * kSectionHeaderBytes;
        Section& s = sections[i];
        s.virtual_bytes = byte_load_le32(record + 8);
        s.rva = byte_load_le32(record + 12);
        s.raw_bytes = byte_load_le32(record + 16);
        s.raw_offset = byte_load_le32(record + 20);
        s.flags = byte_load_le32(record + 36);
        if (!s.virtual_bytes) s.virtual_bytes = s.raw_bytes;
        if (!s.virtual_bytes || s.rva < previous_end || s.rva % section_alignment ||
            static_cast<u64>(s.rva) + s.virtual_bytes > image_bytes ||
            static_cast<u64>(s.raw_offset) + s.raw_bytes > file_bytes ||
            (s.raw_bytes && s.raw_offset < headers_bytes) ||
            ((s.flags & kSectionExecute) && (s.flags & kSectionWrite)))
            return failure(PluginLoadError::sections);
        previous_end = s.rva + s.virtual_bytes;
        if (s.flags & kSectionExecute) output->executable[executable++] = {s.rva, s.rva + s.virtual_bytes};
    }

    // Exactly one export, by name, and it is data in a section the loader never executes.
    if (directory_count <= kDirectoryExport) return failure(PluginLoadError::exports);
    const u8* export_directory = optional + kOptionalHeaderFixed + kDirectoryExport * 8;
    const u32 export_rva = byte_load_le32(export_directory), export_size = byte_load_le32(export_directory + 4);
    u64 at = 0;
    if (!export_rva || export_size < kExportDirectoryBytes ||
        !rva_to_offset(sections, section_count, export_rva, kExportDirectoryBytes, &at))
        return failure(PluginLoadError::exports);
    const u8* table_bytes = file + at;
    if (byte_load_le32(table_bytes + 20) != 1 || byte_load_le32(table_bytes + 24) != 1)
        return failure(PluginLoadError::exports);
    u64 function_at = 0, name_at = 0, ordinal_at = 0, text_at = 0;
    if (!rva_to_offset(sections, section_count, byte_load_le32(table_bytes + 28), 4, &function_at) ||
        !rva_to_offset(sections, section_count, byte_load_le32(table_bytes + 32), 4, &name_at) ||
        !rva_to_offset(sections, section_count, byte_load_le32(table_bytes + 36), 2, &ordinal_at) ||
        byte_load_le16(file + ordinal_at) != 0 ||
        !rva_to_offset(sections, section_count, byte_load_le32(file + name_at), kExportNameBytes, &text_at) ||
        !bytes_equal(file + text_at, MC_DESCRIPTOR_EXPORT, kExportNameBytes))
        return failure(PluginLoadError::exports);
    const u32 function_rva = byte_load_le32(file + function_at);
    // A function RVA inside the export directory is a forwarder string.
    if (function_rva >= export_rva && function_rva - export_rva < export_size) return failure(PluginLoadError::exports);
    const Section* data = section_of(sections, section_count, function_rva);
    if (!data || (data->flags & kSectionExecute) || sizeof(mc_descriptor) > data->virtual_bytes - (function_rva - data->rva))
        return failure(PluginLoadError::exports);

    output->image_bytes = image_bytes;
    output->executable_count = executable;
    *descriptor = function_rva;
    return success();
}

#else
// ---- Linux: the pinned directory, the sealed copy and the ELF checks ------------------------------

// ELF64 little-endian fields (System V x86-64), read as bytes for the same reason as the PE headers: nothing here
// overlays a struct on an untrusted file.
constexpr u32 kElfHeaderBytes = 64;
constexpr u32 kProgramHeaderBytes = 56;
constexpr u32 kProgramHeaderMaximum = 32;
constexpr u32 kSymbolBytes = 24;
constexpr u32 kRelocationBytes = 24;
constexpr u32 kSymbolMaximum = 4096;
constexpr u32 kRelocationMaximumBytes = 4u * 1024u * 1024u;
constexpr u32 kDynamicMaximumBytes = 4096;
constexpr u32 kPageBytes = 4096;
constexpr u32 kAlignMaximum = 0x200000;
constexpr u32 kPtLoad = 1, kPtDynamic = 2, kPtInterp = 3, kPtTls = 7, kPtGnuStack = 0x6474e551;
constexpr u32 kPfExecute = 1, kPfWrite = 2, kPfRead = 4;
constexpr u32 kRelativeRelocation = 8; // R_X86_64_RELATIVE: the one relocation a plugin may have
constexpr u32 kSectionSpecial = 0xff00; // st_shndx values from here on are not section indexes (SHN_ABS, SHN_COMMON, ...)
constexpr u64 kDtNull = 0, kDtNeeded = 1, kDtPltRelSize = 2, kDtHash = 4, kDtStrtab = 5, kDtSymtab = 6, kDtRela = 7,
              kDtRelaSize = 8, kDtRelaEntry = 9, kDtStrSize = 10, kDtSymEntry = 11, kDtInit = 12, kDtFini = 13,
              kDtRel = 17, kDtRelSize = 18, kDtRelEntry = 19, kDtPltRel = 20, kDtTextRel = 22, kDtJmpRel = 23,
              kDtInitArray = 25, kDtFiniArray = 26, kDtFlags = 30, kDtPreinitArray = 32, kDtRelrSize = 35, kDtRelr = 36,
              kDtRelrEntry = 37, kDtGnuHash = 0x6ffffef5, kDtDepAudit = 0x6ffffefb, kDtAudit = 0x6ffffefc,
              kDtVerNeed = 0x6ffffffe, kDtAuxiliary = 0x7ffffffd, kDtFilter = 0x7fffffff;
constexpr u64 kDfTextRel = 4;

u64 page_up(u64 value) noexcept {
    return (value + (kPageBytes - 1)) & ~u64{kPageBytes - 1};
}
u64 page_down(u64 value) noexcept {
    return value & ~u64{kPageBytes - 1};
}

// Descriptors are held in a PluginDirectory as the descriptor plus one, so that zero is no handle.
HANDLE descriptor_token(int descriptor) noexcept {
    return reinterpret_cast<HANDLE>(static_cast<usize>(descriptor) + 1);
}
int token_descriptor(HANDLE token) noexcept {
    return static_cast<int>(reinterpret_cast<usize>(token) - 1);
}

// "/proc/self/fd/<descriptor>": the name under which an open file is opened again, or loaded.
u32 descriptor_path(int descriptor, char (&output)[32]) noexcept {
    constexpr char kPrefix[] = "/proc/self/fd/";
    u32 at = 0;
    for (; at + 1 < sizeof(kPrefix); ++at) output[at] = kPrefix[at];
    char digits[12];
    u32 count = 0;
    for (u32 value = static_cast<u32>(descriptor); count == 0 || value; value /= 10) digits[count++] = static_cast<char>('0' + value % 10);
    while (count) output[at++] = digits[--count];
    output[at] = 0;
    return at;
}

// A directory that may be an ancestor of the plugins: a directory of root or the user that others cannot rename
// entries in (not writable by others unless sticky; writable by the group only when the user's own, as with a private
// group per user). The plugins directory itself, owned: the user's alone and writable by nobody else.
bool directory_trusted(int directory, bool owned, i32* error) noexcept {
    constexpr u32 kNeeded = kStatxType | kStatxMode | kStatxUser;
    Statx stat{};
    const long result = sys_statx(directory, "", kAtEmptyPath, kNeeded, &stat);
    if (result < 0) {
        *error = static_cast<i32>(-result);
        return false;
    }
    *error = kErrorAccess;
    const u32 user = static_cast<u32>(sys_geteuid());
    if ((stat.mask & kNeeded) != kNeeded || (stat.mode & kModeTypeMask) != kModeDirectory) return false;
    if (owned) return stat.user == user && !(stat.mode & (kModeGroupWrite | kModeOtherWrite));
    if (stat.user != 0 && stat.user != user) return false;
    if ((stat.mode & kModeOtherWrite) && !(stat.mode & kModeSticky)) return false;
    return !(stat.mode & kModeGroupWrite) || (stat.mode & kModeSticky) || stat.user == user;
}
// Closes the held ancestors, deepest first. False (with the error) leaves the rest held for a retry.
bool directory_release(PluginDirectory* directory, i32* error) noexcept {
    while (directory->ancestor_count) {
        const long result = sys_close(token_descriptor(directory->ancestors[directory->ancestor_count - 1]));
        if (result < 0 && result != -kErrorInterrupted) {
            *error = static_cast<i32>(-result);
            return false;
        }
        directory->ancestors[--directory->ancestor_count] = nullptr;
    }
    reset_value(directory);
    return true;
}
// Every component is opened without following a link, relative to the one before it, and held while descendants are
// opened: a checked ancestor cannot be swapped for another directory between the hash and the load, and the file is
// opened relative to the last of them.
bool directory_pin(PluginDirectory* directory, ConstSlice<wchar_t> path, i32* error) noexcept {
    constexpr int kFlags = kOpenPath | kOpenDirectory | kOpenNoFollow | kOpenCloseOnExec;
    *error = kErrorInvalid;
    if (!path.data || path.count >= kPluginPathCapacity || path_root(path) != PathRoot::rooted || !path_plain(path)) return false;
    const u32 bytes = utf16_to_utf8(path.data, path.count, directory->utf8, sizeof(directory->utf8) - 1, true);
    if (!bytes) return false;
    directory->utf8[bytes] = 0;
    directory->utf8_bytes = bytes;
    copy_bytes(directory->path, path.data, path.count * 2u);
    directory->path[path.count] = 0;
    directory->path_characters = path.count;
    long descriptor = sys_openat(kAtCurrentDirectory, "/", kFlags);
    u32 start = 1;
    for (bool more = true; more;) {
        if (descriptor < 0) {
            *error = static_cast<i32>(-descriptor);
            return false;
        }
        if (directory->ancestor_count == kPluginPathMaximumAncestors) {
            sys_close(static_cast<int>(descriptor));
            return false;
        }
        directory->ancestors[directory->ancestor_count++] = descriptor_token(static_cast<int>(descriptor));
        more = start < bytes;
        if (!directory_trusted(static_cast<int>(descriptor), !more, error)) return false;
        if (!more) break;
        u32 end = start;
        while (end < bytes && directory->utf8[end] != '/') ++end;
        const u32 length = end - start;
        if (!length || length > 255 || (length == 1 && directory->utf8[start] == '.') ||
            (length == 2 && directory->utf8[start] == '.' && directory->utf8[start + 1] == '.') ||
            (end + 1 == bytes && directory->utf8[end] == '/'))
            return false;
        char name[256];
        copy_bytes(name, directory->utf8 + start, length);
        name[length] = 0;
        descriptor = sys_openat(static_cast<int>(descriptor), name, kFlags);
        start = end + 1;
    }
    return true;
}

// A file opened beside the pinned directory, and the sealed copy of its bytes (file_view).
struct PinnedFile {
    int descriptor = -1;
    u64 bytes;
    int copy = -1;
    const u8* view; // the copy, mapped for the checks
};
// A plain name: printable ASCII without '/' or '\\', no leading dot, short.
bool leaf_valid(ConstSlice<wchar_t> leaf) noexcept {
    if (!leaf.data || !leaf.count || leaf.count >= kLeafMaximumCharacters || leaf[0] == L'.') return false;
    for (u32 i = 0; i < leaf.count; ++i)
        if (leaf[i] <= 32 || leaf[i] >= 127 || leaf[i] == L'/' || leaf[i] == L'\\') return false;
    return true;
}
// Opens leaf relative to the pinned plugins directory without following a link: a regular, non-empty file of the
// user that only the user can write, which is still where the pin says it is (the path the system reports for the
// open file is the pinned one and the name, so a directory renamed since the pin, or a bind mount, is refused).
bool file_open(const PluginDirectory& directory, ConstSlice<wchar_t> leaf, PinnedFile* out, i32* error) noexcept {
    *error = kErrorInvalid;
    if (!directory.ancestor_count || !leaf_valid(leaf)) return false;
    char name[kLeafMaximumCharacters];
    for (u32 i = 0; i < leaf.count; ++i) name[i] = static_cast<char>(leaf[i]);
    name[leaf.count] = 0;
    const long opened = sys_openat(token_descriptor(directory.ancestors[directory.ancestor_count - 1]), name,
                                   kOpenReadOnly | kOpenNoFollow | kOpenCloseOnExec | kOpenNonBlock |
                                       kOpenNoControllingTerminal);
    if (opened < 0) {
        *error = static_cast<i32>(-opened);
        return false;
    }
    out->descriptor = static_cast<int>(opened);
    constexpr u32 kNeeded = kStatxType | kStatxMode | kStatxUser | kStatxSize;
    Statx stat{};
    const long result = sys_statx(out->descriptor, "", kAtEmptyPath, kNeeded, &stat);
    if (result < 0) {
        *error = static_cast<i32>(-result);
        return false;
    }
    *error = kErrorAccess;
    if ((stat.mask & kNeeded) != kNeeded || (stat.mode & kModeTypeMask) != kModeRegular ||
        stat.user != static_cast<u32>(sys_geteuid()) || (stat.mode & (kModeGroupWrite | kModeOtherWrite)) || !stat.size)
        return false;
    char link[32];
    descriptor_path(out->descriptor, link);
    char expected[sizeof(directory.utf8) + kLeafMaximumCharacters + 1], actual[sizeof(expected) + 1];
    u32 count = directory.utf8_bytes;
    copy_bytes(expected, directory.utf8, count);
    if (count > 1) expected[count++] = '/';
    for (u32 i = 0; i < leaf.count; ++i) expected[count++] = name[i];
    const long length = sys_readlink(link, actual, sizeof(actual));
    if (length < 0) {
        *error = static_cast<i32>(-length);
        return false;
    }
    if (static_cast<u32>(length) != count || !bytes_equal(actual, expected, count)) return false;
    out->bytes = stat.size;
    return true;
}

// Copies the file into a sealed anonymous memory file and maps the copy: the bytes that are hashed and checked are the
// bytes dlopen maps, and nobody can change them afterwards. One byte more than the file had is asked for, so a file
// that grew or shrank since it was opened is refused.
PluginLoadResult file_view(PinnedFile* file, ConstSlice<wchar_t> leaf) noexcept {
    char name[sizeof("maicord-plugin:") + kLeafMaximumCharacters];
    constexpr char kPrefix[] = "maicord-plugin:";
    u32 at = 0;
    for (; at + 1 < sizeof(kPrefix); ++at) name[at] = kPrefix[at];
    for (u32 i = 0; i < leaf.count; ++i) name[at++] = static_cast<char>(leaf[i]);
    name[at] = 0;
    // No MFD_EXEC or MFD_NOEXEC_SEAL (Linux 6.3): the copy is mapped executable by dlopen, which the default of
    // vm.memfd_noexec allows; a system that sets it to 2 refuses plugins.
    const long memory = sys_memfd_create(name, kMemfdCloseOnExec | kMemfdAllowSealing);
    if (memory < 0) return failure(PluginLoadError::load, static_cast<DWORD>(-memory));
    file->copy = static_cast<int>(memory);
    for (u64 total = 0;;) {
        const long moved = sys_sendfile(file->copy, file->descriptor, file->bytes + 1 - total);
        if (moved == -kErrorInterrupted) continue;
        if (moved < 0) return failure(PluginLoadError::read, static_cast<DWORD>(-moved));
        if (!moved) {
            if (total != file->bytes) return failure(PluginLoadError::read, kErrorInvalid); // it changed under us
            break;
        }
        if ((total += static_cast<u64>(moved)) > file->bytes) return failure(PluginLoadError::read, kErrorInvalid);
    }
    if (sys_fcntl(file->copy, kFcntlAddSeals, kSealSeal | kSealShrink | kSealGrow | kSealWrite) < 0)
        return failure(PluginLoadError::load, kErrorInvalid);
    const long view = sys_mmap_file(file->copy, file->bytes, kProtectRead, kMapPrivate);
    if (view < 0) return failure(PluginLoadError::load, static_cast<DWORD>(-view));
    file->view = reinterpret_cast<const u8*>(view);
    return success();
}
// The sealed copy is what was hashed and checked: no file name is opened again. Nothing is imported, so the system
// loader finds no library to search for and has no initializer to run. The image is mapped at the address that puts
// the export where its symbol says.
PluginLoadResult image_map(const PinnedFile& file, u64 export_offset, HMODULE* module, const u8** base) noexcept {
    char path[32];
    descriptor_path(file.copy, path);
    *module = dlopen(path, kDlNow | kDlLocal);
    if (!*module) return failure(PluginLoadError::load);
    const auto* found = static_cast<const u8*>(dlsym(*module, MC_DESCRIPTOR_EXPORT));
    if (!found) return failure(PluginLoadError::exports);
    *base = found - export_offset;
    return reinterpret_cast<usize>(*base) & (kPageBytes - 1) ? failure(PluginLoadError::exports) : success();
}
bool image_free(HMODULE module, i32*) noexcept {
    return dlclose(module) == 0;
}
void file_close(PinnedFile* file, PluginImage* image) noexcept {
    if (file->view) sys_munmap(const_cast<u8*>(file->view), file->bytes);
    // The system loader remembers a module by the name it was opened under, and a second open of the same name
    // returns the first module: a loaded plugin keeps its copy open (so no other copy gets that descriptor number)
    // until it is unloaded. A failed close cannot be acted on here: the image is already mapped or already refused.
    if (image->module && file->copy >= 0) image->sealed_token = static_cast<u32>(file->copy) + 1;
    else if (file->copy >= 0) sys_close(file->copy);
    if (file->descriptor >= 0) sys_close(file->descriptor);
}

// One PT_LOAD: where it lies in the file and in memory.
struct Segment {
    u64 vaddr, offset, file_bytes, memory_bytes;
    u32 flags;
};
struct Elf {
    const u8* file; // the sealed copy, mapped
    u64 file_bytes;
    u32 load_count;
    Segment loads[kPluginMaximumSections];
    u64 descriptor; // the export's address in the image
};
// The file offset of [vaddr, vaddr + bytes) when it lies in the file-backed part of one segment.
bool vaddr_to_offset(const Elf& elf, u64 vaddr, u64 bytes, u64* offset) noexcept {
    for (u32 i = 0; i < elf.load_count; ++i) {
        const Segment& s = elf.loads[i];
        if (vaddr < s.vaddr || vaddr - s.vaddr > s.file_bytes || bytes > s.file_bytes - (vaddr - s.vaddr)) continue;
        *offset = s.offset + (vaddr - s.vaddr);
        return true;
    }
    return false;
}
// The segment that holds [vaddr, vaddr + bytes) in memory.
const Segment* segment_of(const Elf& elf, u64 vaddr, u64 bytes) noexcept {
    for (u32 i = 0; i < elf.load_count; ++i) {
        const Segment& s = elf.loads[i];
        if (vaddr >= s.vaddr && vaddr - s.vaddr <= s.memory_bytes && bytes <= s.memory_bytes - (vaddr - s.vaddr)) return &s;
    }
    return nullptr;
}

// The file header and the program headers: an x86-64 shared object with no entry point, interpreter or TLS, whose
// loadable segments are readable, never writable and executable, ascend and leave no unmapped page between them, and
// whose stack is not executable (dlopen would make the whole process's stack executable). Records the executable
// ranges and the image size, and the dynamic segment.
PluginLoadResult elf_headers(Elf* elf, Segment* dynamic, PluginImage* output) noexcept {
    if (elf->file_bytes < kElfHeaderBytes) return failure(PluginLoadError::not_pe);
    const u8* header = elf->file;
    if (!bytes_equal(header, "\x7f" "ELF", 4) || header[4] != 2 || header[5] != 1 || header[6] != 1 ||
        byte_load_le16(header + 16) != 3 || byte_load_le32(header + 20) != 1)
        return failure(PluginLoadError::not_pe);
    if (byte_load_le16(header + 18) != 62) return failure(PluginLoadError::machine);
    if (byte_load_le64(header + 24)) return failure(PluginLoadError::entry_point);
    const u64 table = byte_load_le64(header + 32);
    const u32 count = byte_load_le16(header + 56);
    if (byte_load_le16(header + 52) != kElfHeaderBytes || byte_load_le16(header + 54) != kProgramHeaderBytes || !count ||
        count > kProgramHeaderMaximum || table < kElfHeaderBytes || table > elf->file_bytes ||
        count * kProgramHeaderBytes > elf->file_bytes - table)
        return failure(PluginLoadError::sections);
    const u8* headers = elf->file + table;
    bool stack = false, has_dynamic = false;
    u64 previous_end = 0;
    for (u32 i = 0; i < count; ++i) {
        const u8* h = headers + i * kProgramHeaderBytes;
        const u32 type = byte_load_le32(h), flags = byte_load_le32(h + 4);
        const u64 offset = byte_load_le64(h + 8), vaddr = byte_load_le64(h + 16), file_bytes = byte_load_le64(h + 32),
                  memory_bytes = byte_load_le64(h + 40), align = byte_load_le64(h + 48);
        if (type == kPtInterp) return failure(PluginLoadError::not_pe);
        if (type == kPtTls) return failure(PluginLoadError::tls);
        if (type == kPtGnuStack) {
            if (flags & kPfExecute) return failure(PluginLoadError::sections);
            stack = true;
        } else if (type == kPtDynamic) {
            if (has_dynamic) return failure(PluginLoadError::sections);
            *dynamic = {vaddr, offset, file_bytes, memory_bytes, flags};
            has_dynamic = true;
        } else if (type == kPtLoad) {
            if (elf->load_count == kPluginMaximumSections || !memory_bytes || file_bytes > memory_bytes ||
                offset > elf->file_bytes || file_bytes > elf->file_bytes - offset || vaddr > kPluginImageMaximumBytes ||
                memory_bytes > kPluginImageMaximumBytes - vaddr || !is_power_of_two(align) || align < kPageBytes ||
                align > kAlignMaximum || vaddr % align != offset % align || !(flags & kPfRead) ||
                ((flags & kPfExecute) && (flags & kPfWrite)))
                return failure(PluginLoadError::sections);
            // The first segment starts at zero and each next one begins in or right after the last page of the one before.
            if (elf->load_count ? vaddr < previous_end || page_down(vaddr) > page_up(previous_end) : vaddr != 0)
                return failure(PluginLoadError::sections);
            elf->loads[elf->load_count++] = {vaddr, offset, file_bytes, memory_bytes, flags};
            previous_end = vaddr + memory_bytes;
            if (flags & kPfExecute) output->executable[output->executable_count++] = {static_cast<u32>(vaddr), static_cast<u32>(previous_end)};
        }
    }
    if (!elf->load_count || !has_dynamic || !stack) return failure(PluginLoadError::sections);
    output->image_bytes = static_cast<u32>(page_up(previous_end));
    return success();
}

// What the dynamic section names that the checks use.
enum DynamicSlot : u32 { d_symtab, d_strtab, d_strsize, d_hash, d_gnu_hash, d_rela, d_rela_size, d_jmprel, d_jmprel_size, d_slots };
struct Dynamic {
    u64 value[d_slots];
    u32 seen;
};
// The dynamic section: nothing is needed (no library is loaded for it), nothing runs at load or unload (no
// initializer or finalizer), no text relocation and no relocation but the relative ones that the tables below hold.
PluginLoadResult elf_dynamic(const Elf& elf, const Segment& segment, Dynamic* dynamic) noexcept {
    u64 at = 0;
    if (segment.file_bytes < 16 || segment.file_bytes > kDynamicMaximumBytes || segment.file_bytes % 16 ||
        !vaddr_to_offset(elf, segment.vaddr, segment.file_bytes, &at) || at != segment.offset)
        return failure(PluginLoadError::sections);
    const u8* table = elf.file + at;
    bool ended = false;
    for (u32 entry = 0; entry < segment.file_bytes && !ended; entry += 16) {
        const u64 tag = byte_load_le64(table + entry), value = byte_load_le64(table + entry + 8);
        u32 slot = d_slots;
        switch (tag) {
        case kDtNull: ended = true; break;
        case kDtNeeded:
        case kDtFilter:
        case kDtAuxiliary:
        case kDtAudit:
        case kDtDepAudit:
        case kDtVerNeed: return failure(PluginLoadError::imports);
        case kDtInit:
        case kDtFini:
        case kDtInitArray:
        case kDtFiniArray:
        case kDtPreinitArray: return failure(PluginLoadError::entry_point);
        case kDtTextRel: return failure(PluginLoadError::sections);
        case kDtFlags:
            if (value & kDfTextRel) return failure(PluginLoadError::sections);
            break;
        case kDtRel:
        case kDtRelSize:
        case kDtRelEntry:
        case kDtRelr:
        case kDtRelrSize:
        case kDtRelrEntry: return failure(PluginLoadError::relocations);
        case kDtSymEntry:
            if (value != kSymbolBytes) return failure(PluginLoadError::exports);
            break;
        case kDtRelaEntry:
            if (value != kRelocationBytes) return failure(PluginLoadError::relocations);
            break;
        case kDtPltRel:
            if (value != kDtRela) return failure(PluginLoadError::relocations);
            break;
        case kDtSymtab: slot = d_symtab; break;
        case kDtStrtab: slot = d_strtab; break;
        case kDtStrSize: slot = d_strsize; break;
        case kDtHash: slot = d_hash; break;
        case kDtGnuHash: slot = d_gnu_hash; break;
        case kDtRela: slot = d_rela; break;
        case kDtRelaSize: slot = d_rela_size; break;
        case kDtJmpRel: slot = d_jmprel; break;
        case kDtPltRelSize: slot = d_jmprel_size; break;
        default: break;
        }
        if (slot == d_slots) continue;
        if (dynamic->seen & (1u << slot)) return failure(PluginLoadError::sections); // which one would the loader use?
        dynamic->seen |= 1u << slot;
        dynamic->value[slot] = value;
    }
    return ended ? success() : failure(PluginLoadError::sections);
}

// How many entries the symbol table has, as the hash tables that the loader looks symbols up with count them (the
// header of a GNU hash names the first hashed symbol, the chain of the last bucket ends at the last one; DT_HASH
// names its chain count). 0 when it cannot be told or is above the limit.
u32 symbol_count(const Elf& elf, const Dynamic& dynamic) noexcept {
    u32 count = 0;
    u64 at = 0;
    if (dynamic.seen & (1u << d_hash)) {
        if (!vaddr_to_offset(elf, dynamic.value[d_hash], 8, &at)) return 0;
        count = byte_load_le32(elf.file + at + 4);
    }
    if (dynamic.seen & (1u << d_gnu_hash)) {
        const u64 table = dynamic.value[d_gnu_hash];
        if (!vaddr_to_offset(elf, table, 16, &at)) return 0;
        const u8* words = elf.file + at;
        const u32 buckets = byte_load_le32(words), first = byte_load_le32(words + 4), bloom = byte_load_le32(words + 8);
        u64 check = 0;
        if (!buckets || buckets > kSymbolMaximum || bloom > kSymbolMaximum || first > kSymbolMaximum ||
            !vaddr_to_offset(elf, table, 16 + u64{bloom} * 8 + u64{buckets} * 4, &check))
            return 0;
        const u64 bucket_at = at + 16 + u64{bloom} * 8;
        u32 last = 0;
        for (u32 i = 0; i < buckets; ++i) last = maximum(last, byte_load_le32(elf.file + bucket_at + u64{i} * 4));
        u32 gnu_count = first;
        if (last) {
            if (last < first) return 0;
            for (u32 index = last;; ++index) {
                // The chain is read wherever the file holds it; past the file's end the count cannot be told.
                const u64 word = bucket_at + u64{buckets} * 4 + u64{index - first} * 4;
                if (index >= kSymbolMaximum || word + 4 > elf.file_bytes) return 0;
                if (byte_load_le32(elf.file + word) & 1) {
                    gnu_count = index + 1;
                    break;
                }
            }
        }
        count = maximum(count, gnu_count);
    }
    return count <= kSymbolMaximum ? count : 0;
}

// Every symbol: none undefined (nothing is imported), none thread-local or indirect (those run or resolve code at
// load), and exactly one exported, by name MC_DESCRIPTOR_EXPORT, as data in a segment that is not executable.
PluginLoadResult elf_symbols(Elf* elf, const Dynamic& dynamic) noexcept {
    constexpr u32 kRequired = (1u << d_symtab) | (1u << d_strtab) | (1u << d_strsize);
    u64 symbols = 0, strings = 0;
    const u32 count = (dynamic.seen & kRequired) == kRequired && (dynamic.seen & ((1u << d_hash) | (1u << d_gnu_hash)))
                          ? symbol_count(*elf, dynamic) : 0;
    const u64 string_bytes = dynamic.value[d_strsize];
    if (count < 2 || !string_bytes || !vaddr_to_offset(*elf, dynamic.value[d_symtab], u64{count} * kSymbolBytes, &symbols) ||
        !vaddr_to_offset(*elf, dynamic.value[d_strtab], string_bytes, &strings))
        return failure(PluginLoadError::exports);
    u32 exported = 0;
    for (u32 i = 1; i < count; ++i) {
        const u8* symbol = elf->file + symbols + u64{i} * kSymbolBytes;
        const u32 name = byte_load_le32(symbol), type = symbol[4] & 15, bind = symbol[4] >> 4, visibility = symbol[5] & 3;
        const u32 section = byte_load_le16(symbol + 6);
        const u64 value = byte_load_le64(symbol + 8);
        if (!section || type == 6 || type == 10) return failure(PluginLoadError::imports); // undefined, TLS, indirect function
        if (!bind || visibility == 1 || visibility == 2) continue;                          // local, or not exported
        constexpr u32 kText = sizeof(MC_DESCRIPTOR_EXPORT);
        if (++exported > 1 || type != 1 || section >= kSectionSpecial || name >= string_bytes || string_bytes - name < kText)
            return failure(PluginLoadError::exports);
        const Segment* data = segment_of(*elf, value, sizeof(mc_descriptor));
        if (!bytes_equal(elf->file + strings + name, MC_DESCRIPTOR_EXPORT, kText) || !data || (data->flags & kPfExecute))
            return failure(PluginLoadError::exports);
        elf->descriptor = value;
    }
    return exported == 1 ? success() : failure(PluginLoadError::exports);
}

// A relocation table: only R_X86_64_RELATIVE without a symbol, aimed at a word in a writable segment. Any other kind
// resolves a symbol against the process (a libc function would be found there), or runs a resolver, or addresses
// thread-local storage.
PluginLoadResult elf_relocations(const Elf& elf, u64 vaddr, u64 bytes) noexcept {
    u64 at = 0;
    if (!bytes) return success();
    if (bytes % kRelocationBytes || bytes > kRelocationMaximumBytes || !vaddr_to_offset(elf, vaddr, bytes, &at))
        return failure(PluginLoadError::relocations);
    for (u64 i = 0; i < bytes; i += kRelocationBytes) {
        const u64 target = byte_load_le64(elf.file + at + i), info = byte_load_le64(elf.file + at + i + 8);
        if (info != kRelativeRelocation) return failure((info >> 32) ? PluginLoadError::imports : PluginLoadError::relocations);
        const Segment* segment = segment_of(elf, target, 8);
        if (!segment || !(segment->flags & kPfWrite)) return failure(PluginLoadError::relocations);
    }
    return success();
}

// The checks plugin_image_load applies to the sealed copy before dlopen sees it. Fills the image's executable ranges
// and size, and *descriptor with the export's address inside the image.
PluginLoadResult image_check(const u8* file, u64 file_bytes, PluginImage* output, u64* descriptor) noexcept {
    Elf elf{};
    elf.file = file;
    elf.file_bytes = file_bytes;
    Segment segment{};
    Dynamic dynamic{};
    PluginLoadResult result = elf_headers(&elf, &segment, output);
    if (result.ok()) result = elf_dynamic(elf, segment, &dynamic);
    if (result.ok()) result = elf_symbols(&elf, dynamic);
    if (result.ok()) result = elf_relocations(elf, dynamic.value[d_rela], dynamic.value[d_rela_size]);
    if (result.ok()) result = elf_relocations(elf, dynamic.value[d_jmprel], dynamic.value[d_jmprel_size]);
    *descriptor = elf.descriptor;
    return result;
}
#endif
} // namespace

// Unbound, or a key that cannot be typing: with Ctrl or Alt, or a function key.
bool plugin_keybind_valid(u32 key) noexcept {
    const u32 vk = key & 0xFFu;
    return !key || (vk && !(key & ~(0xFFu | MC_KEY_CTRL | MC_KEY_SHIFT | MC_KEY_ALT)) &&
                    ((key & (MC_KEY_CTRL | MC_KEY_ALT)) || (vk >= 0x70 && vk <= 0x87)));
}

// The permissions a validated descriptor's hooks, buttons and network hosts imply (plugin.hpp computes the same at
// compile time); the loader's permissions include them.
static u64 plugin_implied_permissions(const mc_descriptor& d) noexcept {
    u64 implied = d.network_count ? MC_PERM_NETWORK : 0;
    if (d.permissions & MC_PERM_AUTOMATED_ACTIONS) implied |= MC_PERM_ACT_MESSAGES;
    for (u32 i = 0; i < d.hook_count; ++i) {
        const mc_hook& hook = d.hooks[i];
        switch (hook.kind) {
        case MC_HOOK_PRE_SEND:
        case MC_HOOK_PRE_EDIT:
        case MC_HOOK_PRE_TYPING: implied |= MC_PERM_MESSAGE_SEND; break;
        case MC_HOOK_MESSAGE_CLICK:
        case MC_HOOK_DECORATE_MESSAGE: implied |= MC_PERM_READ_MESSAGES; break;
        case MC_HOOK_MESSAGE_DELETED:
        case MC_HOOK_MESSAGE_EDITED: implied |= MC_PERM_READ_MESSAGES | MC_PERM_MESSAGE_HISTORY; break;
        case MC_HOOK_EVENTS:
            if (hook.filter & MC_EVENT_MESSAGE_KINDS) implied |= MC_PERM_READ_MESSAGES;
            if (hook.filter & ~MC_EVENT_MESSAGE_KINDS) implied |= MC_PERM_READ_ACTIVITY;
            break;
        default: break;
        }
    }
    for (u32 i = 0; i < d.button_count; ++i)
        if (d.buttons[i].surface == MC_SURFACE_MESSAGE_POPOVER) implied |= MC_PERM_READ_MESSAGES;
    return implied;
}

// The descriptor rules, on a loaded image: base is where the image's first byte is mapped.
static PluginLoadResult plugin_descriptor_check(const u8* base, const PluginImage& pe, const mc_descriptor* d,
                                                PluginInfo* info, u64* permissions) noexcept {
    const Image image{base, pe.image_bytes, &pe};
    if (!image.contains(d, sizeof(mc_descriptor)) || d->size < sizeof(mc_descriptor) || d->size > 4096 ||
        !image.contains(d, d->size))
        return problem(PluginDescriptorProblem::size);
    if (d->abi != MC_ABI) return problem(PluginDescriptorProblem::abi);
    if (d->api_level > MC_API_LEVEL) return failure(PluginLoadError::api_level);
    if (!d->api_level) return problem(PluginDescriptorProblem::abi);
    if (d->flags) return problem(PluginDescriptorProblem::flags);
    if (!d->owner_id) return problem(PluginDescriptorProblem::owner);
    if (!image.text(d->name, 1, MC_NAME_MAX) || !image.text(d->author, 0, MC_NAME_MAX) ||
        !image.text(d->version, 1, MC_VERSION_MAX) || !image.text(d->summary, 0, MC_SUMMARY_MAX, true) ||
        !image.text(d->homepage, 0, MC_SUMMARY_MAX))
        return problem(PluginDescriptorProblem::text);
    if (d->homepage.bytes && (d->homepage.bytes < 9 || !bytes_equal(d->homepage.data, "https://", 8)))
        return problem(PluginDescriptorProblem::text);
    if (d->permissions & ~MC_PERM_KNOWN) return problem(PluginDescriptorProblem::permission);
    if (d->theme || d->theme_count) return problem(PluginDescriptorProblem::reserved_capability);
    if (!table_ok(image, d->network, d->network_count, MC_NETWORK_HOSTS_MAX, sizeof(mc_str))) return problem(PluginDescriptorProblem::network);
    for (u32 i = 0; i < d->network_count; ++i)
        if (!image.text(d->network[i], 1, MC_HOST_MAX)) return problem(PluginDescriptorProblem::network, i + 1);

    if (d->state_bytes > MC_STATE_MAX || (d->state_bytes ? !is_power_of_two(d->state_align) ||
            d->state_align > MC_STATE_ALIGN_MAX : d->state_align != 0))
        return problem(PluginDescriptorProblem::state);

    if (d->setting_count > MC_SETTING_MAX || d->settings_bytes > MC_SETTINGS_BYTES_MAX ||
        (d->setting_count && !image.contains(d->settings, static_cast<u64>(d->setting_count) * sizeof(mc_setting_def))) ||
        (!d->setting_count && d->settings))
        return problem(PluginDescriptorProblem::settings);
    for (u32 i = 0; i < d->setting_count; ++i)
        if (!setting_ok(d->settings[i], i, d->settings, d->setting_count, d->settings_bytes, image))
            return problem(PluginDescriptorProblem::settings, i + 1);

    if (!table_ok(image, d->hooks, d->hook_count, MC_HOOK_LAST, sizeof(mc_hook)))
        return problem(PluginDescriptorProblem::hooks);
    for (u32 i = 0; i < d->hook_count; ++i) {
        if (!hook_ok(d->hooks[i], image)) return problem(PluginDescriptorProblem::hooks, i + 1);
        for (u32 j = 0; j < i; ++j)
            if (d->hooks[j].kind == d->hooks[i].kind) return problem(PluginDescriptorProblem::hooks, i + 1);
    }
    {
        // Hosts need a completion hook for the answers; NETWORK without a host asks for nothing.
        bool completion = false;
        for (u32 i = 0; i < d->hook_count; ++i) completion = completion || d->hooks[i].kind == MC_HOOK_COMPLETION;
        if (!plugin_network_hosts_valid(d->network, d->network_count, completion) ||
            ((d->permissions & MC_PERM_NETWORK) && !d->network_count))
            return problem(PluginDescriptorProblem::network);
    }
    if (!table_ok(image, d->commands, d->command_count, MC_COMMAND_MAX, sizeof(mc_command_def)))
        return problem(PluginDescriptorProblem::commands);
    for (u32 i = 0; i < d->command_count; ++i)
        if (!command_ok(d->commands[i], d->commands, i, image)) return problem(PluginDescriptorProblem::commands, i + 1);
    if (!table_ok(image, d->menus, d->menu_count, MC_MENU_MAX, sizeof(mc_menu_def)))
        return problem(PluginDescriptorProblem::menus);
    for (u32 i = 0; i < d->menu_count; ++i)
        if (!menu_ok(d->menus[i], d->menus, i, image)) return problem(PluginDescriptorProblem::menus, i + 1);
    if (!table_ok(image, d->buttons, d->button_count, MC_BUTTON_MAX, sizeof(mc_button_def)))
        return problem(PluginDescriptorProblem::buttons);
    for (u32 i = 0; i < d->button_count; ++i)
        if (!button_ok(d->buttons[i], image)) return problem(PluginDescriptorProblem::buttons, i + 1);
    if ((d->start && !image.code(reinterpret_cast<const void*>(d->start))) ||
        (d->stop && !image.code(reinterpret_cast<const void*>(d->stop))))
        return problem(PluginDescriptorProblem::location);

    // Everything validated; keep the text the host shows after the image is gone.
    reset_value(info);
    const auto keep = [](char* destination, u32 capacity, mc_str source) noexcept {
        copy_bytes(destination, capacity, source.data, source.bytes);
        return source.bytes;
    };
    info->name_bytes = static_cast<u8>(keep(info->name, sizeof(info->name), d->name));
    info->author_bytes = static_cast<u8>(keep(info->author, sizeof(info->author), d->author));
    info->version_bytes = static_cast<u8>(keep(info->version, sizeof(info->version), d->version));
    info->summary_bytes = static_cast<u16>(keep(info->summary, sizeof(info->summary), d->summary));
    *permissions = d->permissions | plugin_implied_permissions(*d);
    return success();
}

PluginLoadResult plugin_directory_open(PluginDirectory* directory, ConstSlice<wchar_t> absolute_path) noexcept {
    if (!directory) return failure(PluginLoadError::invalid_argument);
    if (directory->ancestor_count) return failure(PluginLoadError::already_loaded);
    SystemError error = 0;
    if (directory_pin(directory, absolute_path, &error)) return success();
    // A failed pin keeps the handles it acquired; give them back before reporting.
    SystemError release_error = 0;
    if (!directory_release(directory, &release_error)) return failure(PluginLoadError::release, static_cast<DWORD>(release_error));
    return failure(PluginLoadError::directory, static_cast<DWORD>(error));
}

PluginLoadResult plugin_directory_close(PluginDirectory* directory) noexcept {
    if (!directory) return failure(PluginLoadError::invalid_argument);
    SystemError error = 0;
    return directory_release(directory, &error) ? success() : failure(PluginLoadError::release, static_cast<DWORD>(error));
}

PluginLoadResult plugin_image_load(PluginImage* image, const PluginDirectory& directory,
                                   const ConstSlice<wchar_t>& leaf, u64 approved,
                                   const u8* sha256) noexcept {
    if (!image || !leaf.data || !leaf.count) return failure(PluginLoadError::invalid_argument);
    if (image->module) return failure(PluginLoadError::already_loaded);
    reset_value(image);
    PinnedFile file{};
    SystemError open_error = 0;
    PluginLoadResult result = file_open(directory, leaf, &file, &open_error)
                                  ? success() : failure(PluginLoadError::open, static_cast<DWORD>(open_error));
    if (result.ok() && file.bytes > kPluginDllMaximumBytes) result = failure(PluginLoadError::not_pe);
    if (result.ok()) result = file_view(&file, leaf);
    if (result.ok() && sha256 && !digest_equals(file.view, file.bytes, sha256)) result = failure(PluginLoadError::digest);
    u64 export_offset = 0;
    if (result.ok()) result = image_check(file.view, file.bytes, image, &export_offset);
    HMODULE module = nullptr;
    const u8* base = nullptr;
    if (result.ok()) result = image_map(file, export_offset, &module, &base);
    if (result.ok()) {
        // The export is data, so this is the descriptor's address; no plugin code ran.
        const auto* descriptor = reinterpret_cast<const mc_descriptor*>(base + export_offset);
        result = plugin_descriptor_check(base, *image, descriptor, &image->info, &image->permissions);
        if (result.ok() && (image->permissions & ~approved)) result = failure(PluginLoadError::permissions);
        if (result.ok()) {
            image->module = module;
            image->descriptor = descriptor;
            image->owner_id = descriptor->owner_id;
        }
    }
    if (!result.ok()) {
        reset_value(image);
        // A plugin that cannot be released now stays with the image (on Linux with the copy it was loaded from), so
        // the caller can retry the unload.
        SystemError error = 0;
        if (module && !image_free(module, &error)) {
            result = failure(PluginLoadError::release, static_cast<DWORD>(error));
            image->module = module;
        }
    }
    file_close(&file, image);
    return result;
}

PluginLoadResult plugin_image_unload(PluginImage* image) noexcept {
    if (!image || !image->module) return failure(PluginLoadError::invalid_argument);
    SystemError error = 0;
    if (!image_free(image->module, &error)) return failure(PluginLoadError::release, static_cast<DWORD>(error));
#if !defined(_WIN32)
    // After the module: the name "/proc/self/fd/<n>" it was loaded under is free to name another copy only now.
    if (image->sealed_token) sys_close(static_cast<int>(image->sealed_token - 1));
#endif
    reset_value(image);
    return success();
}
} // namespace maicord
