#include "maicord/app/plugins/http.hpp"
#include "maicord/app/plugins/loader.hpp"
#include "maicord/app/plugins/text.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/crypto.hpp"

namespace maicord {
namespace {

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

PluginLoadResult failure(PluginLoadError error, DWORD win32_error = 0) noexcept {
    return {error, PluginDescriptorProblem::none, win32_error, 0};
}
PluginLoadResult problem(PluginDescriptorProblem which, u32 index = 0) noexcept {
    return {PluginLoadError::descriptor, which, 0, index};
}
PluginLoadResult success() noexcept {
    return {};
}

bool read_at(HANDLE file, u64 offset, void* output, u32 bytes) noexcept {
    Win32LargeInteger position{static_cast<LONGLONG>(offset)};
    DWORD read = 0;
    return SetFilePointerEx(file, *position.native(), nullptr, FILE_BEGIN) &&
           ReadFile(file, output, bytes, &read, nullptr) && read == bytes;
}

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
    if (!path.data || path.count < 3 || path.count >= kPluginPathCapacity || ascii_lower(path[0]) < L'a' ||
        ascii_lower(path[0]) > L'z' || path[1] != L':' || path[2] != L'\\')
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
};
constexpr u32 kLeafMaximumCharacters = 128;
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
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(out->file, &size)) {
        *error = GetLastError();
        return false;
    }
    if (size.QuadPart <= 0) return false;
    out->bytes = static_cast<u64>(size.QuadPart);
    return true;
}
// Whether the whole file, from offset zero, hashes to expected (32 bytes of SHA-256).
bool file_digest_equals(HANDLE file, u64 bytes, const u8* expected) noexcept {
    Win32LargeInteger start{0};
    if (!bytes || bytes > kPluginDllMaximumBytes || !SetFilePointerEx(file, *start.native(), nullptr, FILE_BEGIN)) return false;
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    BCRYPT_HASH_HANDLE hash{};
    bool match = false;
    // CNG owns the hash object, so a failed destroy never leaves a live handle into expired stack storage.
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        u8 buffer[8192], digest[32];
        bool good = true;
        for (u64 remaining = bytes; remaining && good;) {
            const DWORD wanted = static_cast<DWORD>(minimum<u64>(remaining, sizeof(buffer)));
            DWORD read = 0;
            good = ReadFile(file, buffer, wanted, &read, nullptr) && read == wanted &&
                   BCryptHashData(hash, buffer, read, 0) >= 0;
            remaining -= wanted;
        }
        DWORD trailing = 0;
        good = good && ReadFile(file, buffer, 1, &trailing, nullptr) && !trailing;
        match = good && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0 && bytes_equal(digest, expected, sizeof(digest));
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return match;
}

} // namespace

// Unbound, or a key that cannot be typing: with Ctrl or Alt, or a function key.
bool plugin_keybind_valid(u32 key) noexcept {
    const u32 vk = key & 0xFFu;
    return !key || (vk && !(key & ~(0xFFu | MC_KEY_CTRL | MC_KEY_SHIFT | MC_KEY_ALT)) &&
                    ((key & (MC_KEY_CTRL | MC_KEY_ALT)) || (vk >= 0x70 && vk <= 0x87)));
}

PluginLoadResult plugin_pe_check(HANDLE file, u64 file_bytes, PluginImage* output) noexcept {
    if (!file || !output) return failure(PluginLoadError::invalid_argument);
    if (file_bytes < 64) return failure(PluginLoadError::not_pe);
    u8 headers[kHeaderBytes];
    const u32 header_bytes = file_bytes < kHeaderBytes ? static_cast<u32>(file_bytes) : kHeaderBytes;
    if (!read_at(file, 0, headers, header_bytes)) return failure(PluginLoadError::read, GetLastError());
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
    u8 table_bytes[kExportDirectoryBytes];
    if (!export_rva || export_size < kExportDirectoryBytes ||
        !rva_to_offset(sections, section_count, export_rva, kExportDirectoryBytes, &at) ||
        !read_at(file, at, table_bytes, kExportDirectoryBytes))
        return failure(PluginLoadError::exports);
    if (byte_load_le32(table_bytes + 20) != 1 || byte_load_le32(table_bytes + 24) != 1)
        return failure(PluginLoadError::exports);
    u32 function_rva = 0, name_rva = 0;
    u16 ordinal = 0xffff;
    char name[kExportNameBytes + 1]{};
    if (!rva_to_offset(sections, section_count, byte_load_le32(table_bytes + 28), 4, &at) ||
        !read_at(file, at, &function_rva, 4) ||
        !rva_to_offset(sections, section_count, byte_load_le32(table_bytes + 32), 4, &at) ||
        !read_at(file, at, &name_rva, 4) ||
        !rva_to_offset(sections, section_count, byte_load_le32(table_bytes + 36), 2, &at) ||
        !read_at(file, at, &ordinal, 2) || ordinal != 0 ||
        !rva_to_offset(sections, section_count, name_rva, kExportNameBytes, &at) ||
        !read_at(file, at, name, kExportNameBytes) || !bytes_equal(name, MC_DESCRIPTOR_EXPORT, kExportNameBytes))
        return failure(PluginLoadError::exports);
    // A function RVA inside the export directory is a forwarder string.
    if (function_rva >= export_rva && function_rva - export_rva < export_size) return failure(PluginLoadError::exports);
    const Section* data = section_of(sections, section_count, function_rva);
    if (!data || (data->flags & kSectionExecute) || sizeof(mc_descriptor) > data->virtual_bytes - (function_rva - data->rva))
        return failure(PluginLoadError::exports);

    output->image_bytes = image_bytes;
    output->executable_count = executable;
    return success();
}

u64 plugin_implied_permissions(const mc_descriptor& d) noexcept {
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

PluginLoadResult plugin_descriptor_check(const u8* base, const PluginImage& pe, const mc_descriptor* d,
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
    DWORD error = 0;
    if (directory_pin(directory, absolute_path, &error)) return success();
    // A failed pin keeps the handles it acquired; give them back before reporting.
    DWORD release_error = 0;
    if (!directory_release(directory, &release_error)) return failure(PluginLoadError::release, release_error);
    return failure(PluginLoadError::directory, error);
}

PluginLoadResult plugin_directory_close(PluginDirectory* directory) noexcept {
    if (!directory) return failure(PluginLoadError::invalid_argument);
    DWORD error = 0;
    return directory_release(directory, &error) ? success() : failure(PluginLoadError::release, error);
}

PluginLoadResult plugin_image_load(PluginImage* image, const PluginDirectory& directory,
                                   const ConstSlice<wchar_t>& leaf, u64 approved,
                                   const u8* sha256) noexcept {
    if (!image || !leaf.data || !leaf.count) return failure(PluginLoadError::invalid_argument);
    if (image->module) return failure(PluginLoadError::already_loaded);
    reset_value(image);
    PinnedFile file{};
    PluginLoadResult result = success();
    HMODULE module = nullptr;
    DWORD open_error = 0;
    if (!file_open(directory, leaf, &file, &open_error)) result = failure(PluginLoadError::open, open_error);
    else if (file.bytes > kPluginDllMaximumBytes) result = failure(PluginLoadError::not_pe);
    else if (sha256 && !file_digest_equals(file.file, file.bytes, sha256)) result = failure(PluginLoadError::digest);
    else result = plugin_pe_check(file.file, file.bytes, image);
    if (result.ok()) {
        module = LoadLibraryExW(file.path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) result = failure(PluginLoadError::load, GetLastError());
    }
    if (result.ok()) {
        // The export is data, so this is the descriptor's address; no plugin code ran.
        const auto* descriptor = reinterpret_cast<const mc_descriptor*>(GetProcAddress(module, MC_DESCRIPTOR_EXPORT));
        if (!descriptor) result = failure(PluginLoadError::exports, GetLastError());
        else result = plugin_descriptor_check(reinterpret_cast<const u8*>(module), *image, descriptor, &image->info,
                                              &image->permissions);
        if (result.ok() && (image->permissions & ~approved)) result = failure(PluginLoadError::permissions);
        if (result.ok()) {
            image->module = module;
            image->descriptor = descriptor;
            image->owner_id = descriptor->owner_id;
        }
    }
    if (!result.ok() && module && !FreeLibrary(module)) {
        // The plugin cannot be released now; the caller keeps the module to retry the unload.
        result = failure(PluginLoadError::release, GetLastError());
        reset_value(image);
        image->module = module;
        module = nullptr;
    } else if (!result.ok()) {
        reset_value(image);
    }
    // A failed handle close cannot be acted on here: the image is already mapped or already refused.
    if (file.file) CloseHandle(file.file);
    return result;
}

PluginLoadResult plugin_image_unload(PluginImage* image) noexcept {
    if (!image || !image->module) return failure(PluginLoadError::invalid_argument);
    if (!FreeLibrary(image->module)) return failure(PluginLoadError::release, GetLastError());
    reset_value(image);
    return success();
}

} // namespace maicord
