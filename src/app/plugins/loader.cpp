#include "maicord/app/plugins/loader.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

#include "maicord/core/win32_kernel.hpp"

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

bool power_of_two(u32 value) noexcept {
    return value && !(value & (value - 1));
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
    bool text(mc_str string, u32 minimum, u32 maximum, bool newlines = false) const noexcept {
        if (string.reserved || string.bytes < minimum || string.bytes > maximum) return false;
        if (!string.bytes) return true;
        if (!contains(string.data, string.bytes) || !utf8_validate({string.data, string.bytes}).valid) return false;
        for (u32 i = 0; i < string.bytes; ++i) {
            const u8 c = static_cast<u8>(string.data[i]);
            if ((c < 0x20 && !(newlines && c == '\n')) || c == 0x7f) return false;
        }
        return true;
    }
};

bool key_ok(mc_str key, const Image& image) noexcept {
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
    case MC_SET_TEXT:
    case MC_SET_TEXT_LIST: return sizeof(mc_str);
    default: return 0;
    }
}
u32 setting_alignment(u32 type) noexcept {
    return type == MC_SET_TEXT || type == MC_SET_TEXT_LIST ? 8 : setting_bytes(type);
}

bool setting_ok(const mc_setting_def& s, u32 index, const mc_setting_def* all, u32 count, u32 block_bytes,
                const Image& image) noexcept {
    const u32 type = s.type;
    if (s.reserved || (s.flags & ~(MC_SET_HIDDEN | MC_SET_DANGER | MC_SET_MULTILINE)) ||
        !key_ok(s.key, image) || !image.text(s.label, 1, MC_SETTING_LABEL_MAX) ||
        !image.text(s.description, 0, MC_SETTING_TEXT_MAX, true) || !image.text(s.hint, 0, MC_SETTING_LABEL_MAX))
        return false;
    if (s.visible_if) {
        if (s.visible_if > count || s.visible_if == index + 1 || all[s.visible_if - 1].type != MC_SET_TOGGLE)
            return false;
    }
    const bool lists = type == MC_SET_CHOICE || type == MC_SET_TEXT_LIST;
    if (s.item_count > MC_SETTING_ITEM_MAX || (s.item_count && (!lists || !image.contains(s.items,
            static_cast<u64>(s.item_count) * sizeof(mc_str)))) || (!s.item_count && s.items)) return false;
    const u32 maximum_text = s.max_bytes ? s.max_bytes : MC_SETTING_TEXT_MAX;
    if (maximum_text > MC_SETTING_TEXT_MAX || (s.max_bytes && type != MC_SET_TEXT && type != MC_SET_TEXT_LIST))
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

} // namespace

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
    if (!power_of_two(section_alignment) || section_alignment < 0x1000 || !power_of_two(file_alignment) ||
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

PluginLoadResult plugin_descriptor_check(const u8* base, const PluginImage& pe, const mc_descriptor* d,
                                         PluginInfo* info, u64* permissions) noexcept {
    const Image image{base, pe.image_bytes, &pe};
    if (!image.contains(d, sizeof(mc_descriptor)) || d->size < sizeof(mc_descriptor) || d->size > 4096 ||
        !image.contains(d, d->size))
        return problem(PluginDescriptorProblem::size);
    if (d->abi != MC_ABI) return problem(PluginDescriptorProblem::abi);
    if (d->api_level > MC_API_LEVEL) return failure(PluginLoadError::api_level);
    if (!d->api_level) return problem(PluginDescriptorProblem::abi);
    if (d->flags || d->reserved) return problem(PluginDescriptorProblem::flags);
    if (!d->owner_id) return problem(PluginDescriptorProblem::owner);
    if (!image.text(d->name, 1, MC_NAME_MAX) || !image.text(d->author, 0, MC_NAME_MAX) ||
        !image.text(d->version, 1, MC_VERSION_MAX) || !image.text(d->summary, 0, MC_SUMMARY_MAX, true) ||
        !image.text(d->homepage, 0, MC_SUMMARY_MAX))
        return problem(PluginDescriptorProblem::text);
    if (d->homepage.bytes && (d->homepage.bytes < 9 || !bytes_equal(d->homepage.data, "https://", 8)))
        return problem(PluginDescriptorProblem::text);
    if (d->permissions & ~MC_PERM_KNOWN) return problem(PluginDescriptorProblem::permission);
    if (d->network || d->network_count || d->commands || d->command_count || d->menus || d->menu_count ||
        d->buttons || d->button_count || d->theme || d->theme_count || d->effects || d->effect_count)
        return problem(PluginDescriptorProblem::reserved_capability);

    if (d->state_bytes > MC_STATE_MAX || (d->state_bytes ? !power_of_two(d->state_align) ||
            d->state_align > MC_STATE_ALIGN_MAX : d->state_align != 0))
        return problem(PluginDescriptorProblem::state);

    if (d->setting_count > MC_SETTING_MAX || d->settings_bytes > MC_SETTINGS_BYTES_MAX ||
        (d->setting_count && !image.contains(d->settings, static_cast<u64>(d->setting_count) * sizeof(mc_setting_def))) ||
        (!d->setting_count && d->settings))
        return problem(PluginDescriptorProblem::settings);
    for (u32 i = 0; i < d->setting_count; ++i)
        if (!setting_ok(d->settings[i], i, d->settings, d->setting_count, d->settings_bytes, image))
            return problem(PluginDescriptorProblem::settings, i + 1);

    if (d->hook_count > MC_HOOK_LAST || (d->hook_count && !image.contains(d->hooks,
            static_cast<u64>(d->hook_count) * sizeof(mc_hook))) || (!d->hook_count && d->hooks))
        return problem(PluginDescriptorProblem::hooks);
    for (u32 i = 0; i < d->hook_count; ++i) {
        const mc_hook& hook = d->hooks[i];
        if (hook.kind < 1 || hook.kind > MC_HOOK_LAST || hook.flags || hook.filter || !image.code(
                reinterpret_cast<const void*>(hook.fn.draft)))
            return problem(PluginDescriptorProblem::hooks, i + 1);
        for (u32 j = 0; j < i; ++j)
            if (d->hooks[j].kind == hook.kind) return problem(PluginDescriptorProblem::hooks, i + 1);
    }
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
    info->homepage_bytes = static_cast<u16>(keep(info->homepage, sizeof(info->homepage), d->homepage));
    *permissions = d->permissions | (d->hook_count ? MC_PERM_MESSAGE_SEND : 0);
    return success();
}

PluginLoadResult plugin_directory_open(PluginDirectory* directory, ConstSlice<wchar_t> absolute_path) noexcept {
    if (!directory) return failure(PluginLoadError::invalid_argument);
    if (directory->pin.ancestor_count || directory->pin.file) return failure(PluginLoadError::already_loaded);
    const auto pinned = native_plugin_directory_pin(absolute_path, &directory->pin);
    if (pinned.ok()) return success();
    // A failed pin keeps the handles it acquired; give them back before reporting.
    const auto released = native_plugin_verified_file_release(&directory->pin);
    return failure(released.ok() ? PluginLoadError::directory : PluginLoadError::release, pinned.win32_error);
}

PluginLoadResult plugin_directory_close(PluginDirectory* directory) noexcept {
    if (!directory) return failure(PluginLoadError::invalid_argument);
    const auto released = native_plugin_verified_file_release(&directory->pin);
    return released.ok() ? success() : failure(PluginLoadError::release, released.win32_error);
}

PluginLoadResult plugin_image_load(PluginImage* image, const PluginDirectory& directory, ConstSlice<wchar_t> leaf,
                                   u64 approved) noexcept {
    if (!image || !leaf.data || !leaf.count) return failure(PluginLoadError::invalid_argument);
    if (image->module) return failure(PluginLoadError::already_loaded);
    reset_value(image);
    NativePluginVerifiedFile file{};
    PluginLoadResult result = success();
    HMODULE module = nullptr;
    const auto opened = native_plugin_pinned_file_open(directory.pin, leaf, &file);
    if (!opened.ok()) result = failure(PluginLoadError::open, opened.win32_error);
    else if (file.bytes > kPluginDllMaximumBytes) result = failure(PluginLoadError::not_pe);
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
    native_plugin_verified_file_release(&file);
    return result;
}

PluginLoadResult plugin_image_unload(PluginImage* image) noexcept {
    if (!image || !image->module) return failure(PluginLoadError::invalid_argument);
    if (!FreeLibrary(image->module)) return failure(PluginLoadError::release, GetLastError());
    reset_value(image);
    return success();
}

} // namespace maicord
