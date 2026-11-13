#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/os/file.hpp"
#include "maicord/platform/font/default_fonts.hpp"
#include "maicord/platform/font/system_font_locator.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"

#include "font_names.hpp"

namespace maicord {
namespace {

constexpr u32 kRoleCount = static_cast<u32>(DefaultFont::count);
constexpr u32 kDescriptionCapacity = 160;
constexpr u32 kLineCapacity = 512;

struct State {
    // 0 unresolved, 1 resolving (one thread), 2 resolved.
    volatile u32 phase;
    bool portal_present;
    char portal_interface[kDescriptionCapacity];
    char portal_monospace[kDescriptionCapacity];
    wchar_t names[kRoleCount][kSystemFontNameCapacity + 1];
    u32 units[kRoleCount];
};

State g_state;

bool equal_ignore_case(const char* a, u32 a_length, const char* b) noexcept {
    u32 i = 0;
    for (; i < a_length && b[i]; ++i)
        if ((a[i] | 0x20) != (b[i] | 0x20))
            return false;
    return i == a_length && !b[i];
}

// The style words a GTK font description may carry after the family ("Noto Sans Bold Italic 11").
constexpr char kStyleWords[] =
    "Regular\0Roman\0Normal\0Book\0Medium\0Bold\0Semi-Bold\0Demi-Bold\0Ultra-Bold\0Extra-Bold\0Heavy\0Light\0"
    "Semi-Light\0Ultra-Light\0Extra-Light\0Thin\0Italic\0Oblique\0Condensed\0Semi-Condensed\0Expanded\0"
    "Semi-Expanded\0Semibold\0Semilight\0";

bool style_word(const char* word, u32 length) noexcept {
    for (NameList list{kStyleWords}; list; list.next())
        if (equal_ignore_case(word, length, list.at))
            return true;
    return false;
}

// "11", "11.5", "11px", "11pt".
bool size_word(const char* word, u32 length) noexcept {
    u32 i = 0;
    bool digits = false;
    for (; i < length && word[i] >= '0' && word[i] <= '9'; ++i)
        digits = true;
    if (i < length && word[i] == '.') {
        for (++i; i < length && word[i] >= '0' && word[i] <= '9'; ++i)
            digits = true;
    }
    if (!digits)
        return false;
    if (i == length)
        return true;
    return equal_ignore_case(word + i, length - i, "px") || equal_ignore_case(word + i, length - i, "pt");
}

// Splits a Pango font description ("Cantarell Bold 11") into the family without its trailing size and the one
// without style words either. Both are tried, the first installed one wins.
bool split_description(const char* description, char (&with_style)[kDescriptionCapacity],
                       char (&family)[kDescriptionCapacity]) noexcept {
    u32 length = 0;
    // Only the first family of a comma separated list.
    while (description[length] && description[length] != ',' && length + 1 < kDescriptionCapacity)
        ++length;
    while (length && ascii_is_space(description[length - 1]))
        --length;
    u32 begin = 0;
    while (begin < length && ascii_is_space(description[begin]))
        ++begin;
    u32 end = length;
    auto trim_word = [&](bool (*accept)(const char*, u32)) noexcept {
        u32 start = end;
        while (start > begin && !ascii_is_space(description[start - 1]))
            --start;
        if (start == begin || !accept(description + start, end - start))
            return false;
        end = start;
        while (end > begin && ascii_is_space(description[end - 1]))
            --end;
        return true;
    };
    trim_word(size_word);
    if (end == begin)
        return false;
    for (u32 i = begin; i < end; ++i)
        with_style[i - begin] = description[i];
    with_style[end - begin] = 0;
    while (trim_word(style_word) && end > begin) {
    }
    if (end == begin)
        return false;
    for (u32 i = begin; i < end; ++i)
        family[i - begin] = description[i];
    family[end - begin] = 0;
    return true;
}

// Reads `key` of `section` from an INI file ([Section] headers, key=value lines). The value runs to the end of the
// line; false when the file or the key is missing.
bool ini_value(const char* utf8_path, const char* section, const char* key, char* value, u32 capacity,
               bool stop_at_comma) noexcept {
    wchar_t path[1024];
    if (path_from_utf8(utf8_path, path, 1024) == 0)
        return false;
    const FileHandle file =
        file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead | kFileShareWrite | kFileShareDelete,
                         kFileSequential});
    if (file == INVALID_HANDLE_VALUE)
        return false;
    struct Close {
        FileHandle file;
        ~Close() noexcept { file_close(file); }
    } close{file};
    char chunk[2048];
    char line[kLineCapacity];
    u32 line_length = 0;
    bool in_section = false;
    bool overflow = false;
    const u32 section_length = c_string_length(section);
    const u32 key_length = c_string_length(key);
    // The line in `line` is complete.
    auto handle = [&]() noexcept {
        if (overflow) {
            overflow = false;
            return false;
        }
        u32 start = 0;
        while (start < line_length && ascii_is_space(line[start]))
            ++start;
        u32 stop = line_length;
        while (stop > start && ascii_is_space(line[stop - 1]))
            --stop;
        if (start >= stop || line[start] == '#' || line[start] == ';')
            return false;
        if (line[start] == '[') {
            in_section = stop - start == section_length + 2 && line[stop - 1] == ']' &&
                         bytes_equal(line + start + 1, section, section_length);
            return false;
        }
        if (!in_section || stop - start <= key_length || !bytes_equal(line + start, key, key_length))
            return false;
        u32 at = start + key_length;
        while (at < stop && ascii_is_space(line[at]))
            ++at;
        if (at >= stop || line[at] != '=')
            return false;
        ++at;
        while (at < stop && ascii_is_space(line[at]))
            ++at;
        u32 count = 0;
        for (; at < stop && count + 1 < capacity; ++at) {
            if (stop_at_comma && line[at] == ',')
                break;
            value[count++] = line[at];
        }
        value[count] = 0;
        return count != 0;
    };
    for (;;) {
        u32 read = 0;
        if (!file_read(file, chunk, sizeof(chunk), &read) || read == 0)
            break;
        for (u32 i = 0; i < read; ++i) {
            if (chunk[i] == '\n') {
                if (handle())
                    return true;
                line_length = 0;
            } else if (line_length < kLineCapacity) {
                line[line_length++] = chunk[i];
            } else {
                overflow = true;
            }
        }
    }
    return line_length != 0 && !overflow && handle();
}

bool config_path(const char* relative, char (&path)[1024]) noexcept {
    const char* base = environment_get("XDG_CONFIG_HOME");
    const char* tail = "";
    if (!base || base[0] != '/') {
        base = environment_get("HOME");
        tail = "/.config";
    }
    if (!base || base[0] != '/')
        return false;
    const char* const parts[] = {base, tail, "/", relative};
    u32 length = 0;
    for (const char* part : parts)
        for (; *part; ++part) {
            if (length + 1 >= sizeof(path))
                return false;
            path[length++] = *part;
        }
    path[length] = 0;
    return true;
}

bool desktop_is_kde() noexcept {
    const char* desktop = environment_get("XDG_CURRENT_DESKTOP");
    if (!desktop)
        return false;
    for (u32 i = 0; desktop[i]; ++i)
        if ((desktop[i] | 0x20) == 'k' && (desktop[i + 1] | 0x20) == 'd' && (desktop[i + 2] | 0x20) == 'e')
            return true;
    return false;
}

// The desktop's own settings for the interface and monospace fonts, as GTK font descriptions.
struct DesktopFonts {
    char interface_font[kDescriptionCapacity];
    char monospace_font[kDescriptionCapacity];
};

void read_desktop_fonts(DesktopFonts* fonts) noexcept {
    char path[1024];
    if (g_state.portal_present) {
        copy_bytes(fonts->interface_font, g_state.portal_interface, kDescriptionCapacity);
        copy_bytes(fonts->monospace_font, g_state.portal_monospace, kDescriptionCapacity);
    }
    const bool kde = desktop_is_kde();
    if (kde && config_path("kdeglobals", path)) {
        // KDE writes "Noto Sans,10,-1,5,400,0,0,0,0,0,0,0,0,0,0,1": the family is the first field.
        if (!fonts->interface_font[0])
            ini_value(path, "General", "font", fonts->interface_font, kDescriptionCapacity, true);
        if (!fonts->monospace_font[0])
            ini_value(path, "General", "fixed", fonts->monospace_font, kDescriptionCapacity, true);
    }
    if (!fonts->interface_font[0]) {
        const char* const files[] = {"gtk-4.0/settings.ini", "gtk-3.0/settings.ini"};
        for (const char* file : files)
            if (config_path(file, path) &&
                ini_value(path, "Settings", "gtk-font-name", fonts->interface_font, kDescriptionCapacity, false))
                break;
    }
    if (!kde && !fonts->interface_font[0] && config_path("kdeglobals", path))
        ini_value(path, "General", "font", fonts->interface_font, kDescriptionCapacity, true);
}

// The first installed family a font description names.
bool installed_family(SystemFontLocator* locator, const char* description, SystemFontName* name) noexcept {
    char with_style[kDescriptionCapacity];
    char family[kDescriptionCapacity];
    if (!description[0] || !split_description(description, with_style, family))
        return false;
    const char* const candidates[] = {family, with_style};
    for (const char* candidate : candidates) {
        SystemFontName converted{};
        if (!font_locator::utf8_to_name(candidate, &converted))
            continue;
        SystemFontFile file;
        if (system_font_locator_find(locator, {{converted.units, converted.count}, 400, false}, &file)) {
            *name = converted;
            return true;
        }
    }
    return false;
}

void store(DefaultFont role, const SystemFontName& name) noexcept {
    const u32 index = static_cast<u32>(role);
    for (u32 i = 0; i < name.count; ++i)
        g_state.names[index][i] = static_cast<wchar_t>(name.units[i]);
    g_state.names[index][name.count] = 0;
    g_state.units[index] = name.count;
}

void store_literal(DefaultFont role, const char* text) noexcept {
    SystemFontName name{};
    if (font_locator::utf8_to_name(text, &name))
        store(role, name);
}

void resolve() noexcept {
    SystemFontLocator locator{};
    DesktopFonts desktop{};
    read_desktop_fonts(&desktop);
    SystemFontName name{};

    SystemFontName ui{};
    if (!installed_family(&locator, desktop.interface_font, &ui) &&
        system_font_locator_role(&locator, SystemFontRole::ui, &ui) == 0)
        font_locator::utf8_to_name("Noto Sans", &ui);
    store(DefaultFont::ui, ui);
    store(DefaultFont::display, ui);
    store(DefaultFont::rounded, ui);

    if (!installed_family(&locator, desktop.monospace_font, &name) &&
        system_font_locator_role(&locator, SystemFontRole::monospace, &name) == 0)
        font_locator::utf8_to_name("DejaVu Sans Mono", &name);
    store(DefaultFont::monospace, name);

    if (system_font_locator_role(&locator, SystemFontRole::serif, &name) == 0)
        font_locator::utf8_to_name("DejaVu Serif", &name);
    store(DefaultFont::serif, name);

    if (system_font_locator_role(&locator, SystemFontRole::cursive, &name) == 0)
        name = ui;
    store(DefaultFont::handwriting, name);

    if (system_font_locator_role(&locator, SystemFontRole::emoji, &name) != 0)
        store(DefaultFont::emoji, name);
    else
        store_literal(DefaultFont::emoji, "Noto Color Emoji");

    system_font_locator_release(&locator);
}

void ensure() noexcept {
    if (__atomic_load_n(&g_state.phase, __ATOMIC_ACQUIRE) == 2)
        return;
    u32 expected = 0;
    if (__atomic_compare_exchange_n(&g_state.phase, &expected, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
        resolve();
        __atomic_store_n(&g_state.phase, 2u, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(&g_state.phase, __ATOMIC_ACQUIRE) != 2)
        sys_sched_yield();
}

} // namespace

bool default_fonts_set_portal(const char* interface_font, const char* monospace_font) noexcept {
    if (__atomic_load_n(&g_state.phase, __ATOMIC_ACQUIRE) != 0)
        return false;
    g_state.portal_interface[0] = 0;
    g_state.portal_monospace[0] = 0;
    const char* const sources[] = {interface_font, monospace_font};
    char* const targets[] = {g_state.portal_interface, g_state.portal_monospace};
    for (u32 i = 0; i < 2; ++i) {
        if (!sources[i])
            continue;
        u32 length = 0;
        while (sources[i][length] && length + 1 < kDescriptionCapacity) {
            targets[i][length] = sources[i][length];
            ++length;
        }
        targets[i][length] = 0;
    }
    g_state.portal_present = true;
    return true;
}

const wchar_t* default_font_family(DefaultFont role) noexcept {
    if (static_cast<u32>(role) >= kRoleCount)
        return nullptr;
    ensure();
    return g_state.names[static_cast<u32>(role)];
}

ConstSlice<u16> default_font_name(DefaultFont role) noexcept {
    if (static_cast<u32>(role) >= kRoleCount)
        return {};
    ensure();
    return {reinterpret_cast<const u16*>(g_state.names[static_cast<u32>(role)]), g_state.units[static_cast<u32>(role)]};
}

} // namespace maicord
