// What platform/desktop/integration.hpp registers with the system, on Linux: the startup entry is an XDG autostart
// desktop file, and the URL protocol is a desktop file that names the program for the scheme plus the user's default
// for the scheme in mimeapps.list. Everything is files in the user's own directories; the same questions as on
// Windows are answered (absent, ours, other) and the registry scope has no root. The link parser, the launch parser
// and the close and show policies are shared (integration.cpp).
#include "maicord/platform/desktop/integration.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/os/file.hpp"
#include "maicord/os/text.hpp"

namespace maicord {
namespace {

constexpr wchar_t kApplicationId[] = L"io.github.maj113.MaiCord";
constexpr u32 kPathCapacity = 1024;
constexpr u32 kFileCapacity = 32 * 1024;

struct Files {
    char first[kFileCapacity], second[kFileCapacity];
};

// UTF-8 text built piece by piece; ok turns false at the first piece that does not fit.
struct Text {
    char* data;
    u32 capacity, count;
    bool ok;
    Text& add(const char* text, u32 bytes) noexcept {
        if (ok && bytes < capacity - count) {
            copy_bytes(data + count, text, bytes);
            count += bytes;
        } else {
            ok = false;
        }
        return *this;
    }
    Text& add(const char* text) noexcept {
        u32 bytes = 0;
        while (text[bytes])
            ++bytes;
        return add(text, bytes);
    }
    Text& add_wide(const wchar_t* text, u32 units) noexcept {
        if (!ok)
            return *this;
        const u32 bytes = units ? utf16_to_utf8(text, units, data + count, capacity - count, true) : 0;
        if (units && !bytes)
            ok = false;
        count += bytes;
        return *this;
    }
};

u32 wide_units(const wchar_t* text) noexcept {
    u32 units = 0;
    while (text && text[units])
        ++units;
    return units;
}

// The directory of a base-directory variable ($XDG_CONFIG_HOME, else $HOME/.config) joined with `relative`.
bool base_path(const wchar_t* variable, const wchar_t* fallback, const wchar_t* relative, wchar_t* out,
               u32 capacity) noexcept {
    u32 length = 0;
    wchar_t root[kPathCapacity];
    u32 units = environment_variable(variable, root, kPathCapacity);
    if (!units || units >= kPathCapacity || root[0] != L'/') {
        units = environment_variable(L"HOME", root, kPathCapacity);
        if (!units || units >= kPathCapacity || root[0] != L'/')
            return false;
        out[0] = 0;
        return wide_append(out, capacity, &length, root) && wide_append(out, capacity, &length, fallback) &&
               wide_append(out, capacity, &length, relative);
    }
    out[0] = 0;
    return wide_append(out, capacity, &length, root) && wide_append(out, capacity, &length, relative);
}

bool startup_path(wchar_t* out, u32 capacity) noexcept {
    return base_path(L"XDG_CONFIG_HOME", L"/.config", L"/autostart/io.github.maj113.MaiCord.desktop", out, capacity);
}
bool mimeapps_path(wchar_t* out, u32 capacity) noexcept {
    return base_path(L"XDG_CONFIG_HOME", L"/.config", L"/mimeapps.list", out, capacity);
}
// The desktop file of one scheme's handler and its name as mimeapps.list lists it.
bool handler_id(const DesktopRegistryScope& s, wchar_t* out, u32 capacity) noexcept {
    u32 length = 0;
    out[0] = 0;
    return wide_append(out, capacity, &length, kApplicationId) && wide_append(out, capacity, &length, L".") &&
           wide_append(out, capacity, &length, s.protocol) && wide_append(out, capacity, &length, L".desktop");
}
bool handler_path(const DesktopRegistryScope& s, wchar_t* out, u32 capacity) noexcept {
    wchar_t id[kPathCapacity];
    if (!handler_id(s, id, kPathCapacity) || !base_path(L"XDG_DATA_HOME", L"/.local/share", L"/applications/", out, capacity))
        return false;
    u32 length = wide_units(out);
    return wide_append(out, capacity, &length, id);
}

bool scope_valid(const DesktopRegistryScope& s) noexcept {
    if (!s.protocol || !*s.protocol)
        return false;
    for (u32 i = 0; s.protocol[i]; ++i)
        if (!((s.protocol[i] >= L'a' && s.protocol[i] <= L'z') || (s.protocol[i] >= L'0' && s.protocol[i] <= L'9')))
            return false;
    return true;
}

// A path a desktop file's Exec can quote without escapes.
bool executable_valid(ConstSlice<wchar_t> executable) noexcept {
    if (!executable.data || executable.count < 2 || executable.count > 1024 || executable[0] != L'/')
        return false;
    for (u32 i = 0; i < executable.count; ++i) {
        const wchar_t c = executable[i];
        if (c < L' ' || c == L'"' || c == L'\\' || c == L'`' || c == L'$' || c == 0x7F)
            return false;
    }
    return true;
}

// `"<executable>"` as an Exec value starts it: a percent sign is doubled.
void exec_program(Text* text, ConstSlice<wchar_t> executable) noexcept {
    text->add("\"");
    u32 first = 0;
    for (u32 i = 0; i <= executable.count; ++i) {
        if (i < executable.count && executable[i] != L'%')
            continue;
        text->add_wide(executable.data + first, i - first);
        if (i < executable.count)
            text->add("%%");
        first = i + 1;
    }
    text->add("\"");
}

bool read_text(const wchar_t* path, char* buffer, u32* count) noexcept {
    const FileHandle file = file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead | kFileShareWrite,
                                            kFileSequential});
    if (file == INVALID_HANDLE_VALUE)
        return false;
    u32 read = 0;
    const bool ok = file_read(file, buffer, kFileCapacity, &read) && read < kFileCapacity;
    file_close(file);
    *count = ok ? read : 0;
    return ok;
}

// Every missing directory above the file.
void make_parents(wchar_t* path) noexcept {
    for (u32 i = 1; path[i]; ++i) {
        if (path[i] != L'/')
            continue;
        path[i] = 0;
        dir_create(path);
        path[i] = L'/';
    }
}

bool write_text(wchar_t* path, const char* text, u32 count) noexcept {
    make_parents(path);
    const FileHandle file = file_open(path, {FileAccess::write, FileCreate::create_always, kFileShareRead,
                                            kFileSequential});
    if (file == INVALID_HANDLE_VALUE)
        return false;
    u32 written = 0;
    const bool ok = file_write(file, text, count, &written) && written == count;
    return file_close(file) && ok;
}

bool remove_file(const wchar_t* path) noexcept {
    FileStat stat{};
    return file_remove(path) || !file_path_stat(path, &stat);
}

// The next line of `text` from *at, without its newline.
bool next_line(const char* text, u32 count, u32* at, const char** line, u32* length) noexcept {
    if (*at >= count)
        return false;
    u32 end = *at;
    while (end < count && text[end] != '\n')
        ++end;
    *line = text + *at;
    *length = end - *at;
    *at = end < count ? end + 1 : end;
    return true;
}
bool line_is(const char* line, u32 length, const char* literal) noexcept {
    u32 i = 0;
    for (; literal[i]; ++i)
        if (i >= length || line[i] != literal[i])
            return false;
    return i == length;
}
bool line_starts(const char* line, u32 length, const char* prefix, u32 prefix_length) noexcept {
    return length >= prefix_length && bytes_equal(line, prefix, prefix_length);
}

// Whether the desktop file's Exec line starts `"<executable>"`.
bool exec_names(const char* text, u32 count, ConstSlice<wchar_t> executable, char* scratch) noexcept {
    Text expected{scratch, 4096, 0, true};
    expected.add("Exec=");
    exec_program(&expected, executable);
    if (!expected.ok)
        return false;
    const char* line;
    u32 length = 0, at = 0;
    while (next_line(text, count, &at, &line, &length))
        if (line_starts(line, length, scratch, expected.count) &&
            (length == expected.count || line[expected.count] == ' '))
            return true;
    return false;
}

void entry_header(Text* text) noexcept {
    text->add("[Desktop Entry]\nType=Application\nName=MaiCord\nIcon=io.github.maj113.MaiCord\nTerminal=false\n");
}

} // namespace

DesktopRegistryScope desktop_registry_user_scope() noexcept {
    return {L"MaiCord", L"discord", L"Discord"};
}

DesktopRegistryScope desktop_registry_maicord_scope() noexcept {
    DesktopRegistryScope scope = desktop_registry_user_scope();
    scope.protocol = L"maicord";
    scope.title = L"MaiCord";
    return scope;
}

DesktopRegistration desktop_startup_query(const DesktopRegistryScope&, ConstSlice<wchar_t> executable) noexcept {
    ScratchPages<Files> files;
    wchar_t path[kPathCapacity];
    u32 count = 0;
    if (!files.data || !startup_path(path, kPathCapacity) || !read_text(path, files.data->first, &count))
        return DesktopRegistration::absent;
    if (!executable_valid(executable) || !exec_names(files.data->first, count, executable, files.data->second))
        return DesktopRegistration::other;
    const char* line;
    u32 length = 0, at = 0;
    while (next_line(files.data->first, count, &at, &line, &length))
        if (line_is(line, length, "Hidden=true") || line_is(line, length, "X-GNOME-Autostart-enabled=false"))
            return DesktopRegistration::absent;
    return DesktopRegistration::ours;
}

bool desktop_startup_set(const DesktopRegistryScope&, ConstSlice<wchar_t> executable, bool enable) noexcept {
    wchar_t path[kPathCapacity];
    if (!startup_path(path, kPathCapacity))
        return false;
    if (!enable)
        return remove_file(path);
    ScratchPages<Files> files;
    if (!files.data || !executable_valid(executable))
        return false;
    Text text{files.data->first, kFileCapacity, 0, true};
    entry_header(&text);
    text.add("Exec=");
    exec_program(&text, executable);
    text.add(" --startup\nX-GNOME-Autostart-enabled=true\n");
    return text.ok && write_text(path, text.data, text.count);
}

DesktopRegistration desktop_protocol_query(const DesktopRegistryScope& s, ConstSlice<wchar_t> executable) noexcept {
    ScratchPages<Files> files;
    wchar_t path[kPathCapacity], id[kPathCapacity];
    u32 count = 0;
    if (!scope_valid(s) || !files.data || !handler_id(s, id, kPathCapacity) || !mimeapps_path(path, kPathCapacity) ||
        !read_text(path, files.data->first, &count))
        return DesktopRegistration::absent;
    // The default entry for the scheme in [Default Applications]: "x-scheme-handler/<scheme>=<id>.desktop;...".
    Text key{files.data->second, 256, 0, true};
    key.add("x-scheme-handler/").add_wide(s.protocol, wide_units(s.protocol)).add("=");
    const u32 key_bytes = key.count;
    Text wanted{files.data->second + 256, 1024, 0, true};
    wanted.add_wide(id, wide_units(id)).add(";");
    if (!key.ok || !wanted.ok)
        return DesktopRegistration::absent;
    bool in_defaults = false, found = false, first_is_ours = false;
    const char* line;
    u32 length = 0, at = 0;
    while (next_line(files.data->first, count, &at, &line, &length)) {
        if (length && line[0] == '[') {
            in_defaults = line_is(line, length, "[Default Applications]");
        } else if (in_defaults && line_starts(line, length, key.data, key_bytes)) {
            found = true;
            first_is_ours = line_starts(line + key_bytes, length - key_bytes, wanted.data, wanted.count);
        }
    }
    if (!found)
        return DesktopRegistration::absent;
    if (!first_is_ours)
        return DesktopRegistration::other;
    // Our file must still start this program.
    wchar_t desktop[kPathCapacity];
    u32 file_count = 0;
    if (!handler_path(s, desktop, kPathCapacity) || !read_text(desktop, files.data->first, &file_count))
        return DesktopRegistration::other;
    return executable_valid(executable) && exec_names(files.data->first, file_count, executable, files.data->second)
               ? DesktopRegistration::ours
               : DesktopRegistration::other;
}

// Sets (id) or removes (null) the user's default for the scheme in mimeapps.list, keeping everything else.
static bool set_default(const DesktopRegistryScope& s, const wchar_t* id) noexcept {
    ScratchPages<Files> files;
    wchar_t path[kPathCapacity];
    if (!files.data || !mimeapps_path(path, kPathCapacity))
        return false;
    u32 count = 0;
    read_text(path, files.data->first, &count);
    Text key{files.data->second + kFileCapacity - 512, 512, 0, true};
    key.add("x-scheme-handler/").add_wide(s.protocol, wide_units(s.protocol)).add("=");
    const u32 key_bytes = key.count;
    if (id)
        key.add_wide(id, wide_units(id)).add(";");
    if (!key.ok)
        return false;
    Text out{files.data->second, kFileCapacity - 512, 0, true};
    const auto emit_key = [&]() noexcept { out.add(key.data, key.count).add("\n"); };
    bool in_defaults = false, seen_defaults = false, wrote = !id;
    const char* line;
    u32 length = 0, at = 0;
    while (next_line(files.data->first, count, &at, &line, &length)) {
        if (length && line[0] == '[') {
            if (in_defaults && !wrote) {
                emit_key();
                wrote = true;
            }
            in_defaults = line_is(line, length, "[Default Applications]");
            seen_defaults = seen_defaults || in_defaults;
        } else if (in_defaults && line_starts(line, length, key.data, key_bytes)) {
            if (!wrote)
                emit_key();
            wrote = true;
            continue;
        }
        out.add(line, length).add("\n");
    }
    if (!wrote) {
        if (!seen_defaults)
            out.add("[Default Applications]\n");
        emit_key();
    }
    return out.ok && write_text(path, out.data, out.count);
}

bool desktop_protocol_set(const DesktopRegistryScope& s, ConstSlice<wchar_t> executable, bool enable) noexcept {
    wchar_t desktop[kPathCapacity], id[kPathCapacity];
    if (!scope_valid(s) || !handler_id(s, id, kPathCapacity) || !handler_path(s, desktop, kPathCapacity))
        return false;
    if (!enable) {
        if (desktop_protocol_query(s, executable) != DesktopRegistration::ours)
            return true;
        return set_default(s, nullptr) && remove_file(desktop);
    }
    ScratchPages<Files> files;
    if (!files.data || !executable_valid(executable))
        return false;
    Text text{files.data->first, kFileCapacity, 0, true};
    entry_header(&text);
    text.add("NoDisplay=true\nExec=");
    exec_program(&text, executable);
    text.add(" --url -- %u\nMimeType=x-scheme-handler/").add_wide(s.protocol, wide_units(s.protocol)).add(";\n");
    return text.ok && write_text(desktop, text.data, text.count) && set_default(s, id);
}

u32 desktop_appimage_path(wchar_t* output, u32 capacity) noexcept {
    wchar_t mount[kPathCapacity], executable[kPathCapacity];
    const u32 image = environment_variable(L"APPIMAGE", output, capacity);
    const u32 root = environment_variable(L"APPDIR", mount, kPathCapacity);
    const u32 running = executable_path(executable, kPathCapacity);
    // The variables are inherited by anything started from an AppImage, so they count only for the AppImage whose mount
    // holds this very program.
    if (!image || image >= capacity || !root || root >= kPathCapacity || running <= root || executable[root] != L'/' ||
        !bytes_equal(executable, mount, root * sizeof(wchar_t)))
        return 0;
    return image;
}
} // namespace maicord
