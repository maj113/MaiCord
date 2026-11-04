#include "font_config.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/os/file.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord::font_locator {
namespace {

constexpr u32 kPathCapacity = 1024;
constexpr u32 kFileLimit = 256 * 1024;
constexpr u32 kDepthLimit = 4;
constexpr u32 kFileCountLimit = 128;

struct Collector {
    Arena* arena;
    FontRoots* roots;
    u32 files;
    bool read_any;
};

bool starts_with(const char* at, const char* end, const char* literal) noexcept {
    for (; *literal; ++literal, ++at)
        if (at >= end || *at != *literal)
            return false;
    return true;
}

const char* find(const char* at, const char* end, const char* literal) noexcept {
    for (; at < end; ++at)
        if (starts_with(at, end, literal))
            return at;
    return nullptr;
}

u32 text_length(const char* text) noexcept {
    u32 length = 0;
    while (text[length])
        ++length;
    return length;
}

bool equal_prefix(const char* a, const char* b) noexcept {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

bool wide_equal(const wchar_t* a, const wchar_t* b) noexcept {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

bool absolute(const char* text) noexcept {
    return text && text[0] == '/';
}

// Appends `bytes` of `text` to the UTF-8 path being built.
bool append(char (&path)[kPathCapacity], u32* length, const char* text, u32 bytes) noexcept {
    if (*length + bytes >= kPathCapacity)
        return false;
    for (u32 i = 0; i < bytes; ++i)
        path[(*length)++] = text[i];
    path[*length] = 0;
    return true;
}

bool append(char (&path)[kPathCapacity], u32* length, const char* text) noexcept {
    return append(path, length, text, text_length(text));
}

// $XDG_DATA_HOME or ~/.local/share, and $XDG_CONFIG_HOME or ~/.config: the base directory rule.
bool xdg_home(const char* variable, const char* fallback, char (&path)[kPathCapacity], u32* length) noexcept {
    *length = 0;
    path[0] = 0;
    const char* value = environment_get(variable);
    if (absolute(value))
        return append(path, length, value);
    const char* home = environment_get("HOME");
    return absolute(home) && append(path, length, home) && append(path, length, fallback);
}

// A regular configuration text read into the arena; the text stays there.
bool read_text(Arena* arena, const char* utf8, char** text, u32* bytes) noexcept {
    wchar_t path[kPathCapacity];
    if (path_from_utf8(utf8, path, kPathCapacity) == 0)
        return false;
    const FileHandle file =
        file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead | kFileShareWrite | kFileShareDelete, 0});
    if (file == INVALID_HANDLE_VALUE)
        return false;
    struct Close {
        FileHandle file;
        ~Close() noexcept { file_close(file); }
    } close{file};
    u64 size = 0;
    if (!file_size(file, &size) || size == 0 || size > kFileLimit)
        return false;
    auto* buffer = static_cast<char*>(arena_allocate(arena, static_cast<usize>(size), 1));
    if (!buffer)
        return false;
    u32 read = 0;
    if (!file_read(file, buffer, static_cast<u32>(size), &read) || read != size)
        return false;
    *text = buffer;
    *bytes = read;
    return true;
}

bool add_root(Collector& c, const char* utf8) noexcept {
    if (c.roots->count >= kFontRootCapacity)
        return false;
    wchar_t path[kPathCapacity];
    u32 length = path_from_utf8(utf8, path, kPathCapacity);
    if (length == 0)
        return false;
    while (length > 1 && path[length - 1] == L'/')
        path[--length] = 0;
    for (u32 i = 0; i < c.roots->count; ++i)
        if (wide_equal(c.roots->paths[i], path))
            return true;
    auto* stored = static_cast<wchar_t*>(arena_allocate(c.arena, (length + 1) * sizeof(wchar_t), alignof(wchar_t)));
    if (!stored)
        return false;
    copy_bytes(stored, path, (length + 1) * static_cast<u32>(sizeof(wchar_t)));
    c.roots->paths[c.roots->count++] = stored;
    return true;
}

void parse(Collector& c, const char* text, u32 bytes, const char* config, u32 depth) noexcept;

// An <include> target: a configuration file, or a directory of *.conf files.
void include_path(Collector& c, const char* utf8, u32 depth) noexcept {
    if (depth > kDepthLimit || c.files >= kFileCountLimit)
        return;
    wchar_t wide[kPathCapacity];
    if (path_from_utf8(utf8, wide, kPathCapacity) == 0)
        return;
    DirIter iterator{};
    if (dir_open(&iterator, wide)) {
        DirEntry entry;
        while (dir_next(&iterator, &entry)) {
            if (entry.kind == FileKind::directory)
                continue;
            u32 units = 0;
            while (entry.name[units])
                ++units;
            if (units < 6 || entry.name[units - 5] != L'.' || entry.name[units - 4] != L'c' ||
                entry.name[units - 3] != L'o' || entry.name[units - 2] != L'n' || entry.name[units - 1] != L'f')
                continue;
            char name[kPathCapacity];
            u32 length = 0;
            for (; utf8[length]; ++length)
                name[length] = utf8[length];
            if (length == 0 || length + 1 >= kPathCapacity)
                continue;
            if (name[length - 1] != '/')
                name[length++] = '/';
            name[length] = 0;
            // The entry names a file of a configuration directory: ASCII in practice, converted unit by unit.
            bool ascii = true;
            for (u32 i = 0; i < units; ++i) {
                if (entry.name[i] >= 0x80 || length + 1 >= kPathCapacity) {
                    ascii = false;
                    break;
                }
                name[length++] = static_cast<char>(entry.name[i]);
            }
            if (!ascii)
                continue;
            name[length] = 0;
            ++c.files;
            char* text = nullptr;
            u32 size = 0;
            if (c.files < kFileCountLimit && read_text(c.arena, name, &text, &size)) {
                c.read_any = true;
                parse(c, text, size, name, depth + 1);
            }
        }
        dir_close(&iterator);
        return;
    }
    ++c.files;
    char* text = nullptr;
    u32 size = 0;
    if (read_text(c.arena, utf8, &text, &size)) {
        c.read_any = true;
        parse(c, text, size, utf8, depth + 1);
    }
}

// The value of attribute `name` inside the tag text [begin, end), or an empty string.
u32 attribute(const char* begin, const char* end, const char* name, char* value, u32 capacity) noexcept {
    const u32 length = text_length(name);
    for (const char* at = begin; at + length + 1 < end; ++at) {
        const bool boundary = at == begin || at[-1] == ' ' || at[-1] == '\t' || at[-1] == '\n' || at[-1] == '\r';
        if (!boundary || !starts_with(at, end, name) || at[length] != '=')
            continue;
        const char* quote = at + length + 1;
        if (quote >= end || (*quote != '"' && *quote != '\''))
            return 0;
        const char* stop = quote + 1;
        while (stop < end && *stop != *quote)
            ++stop;
        const u32 bytes = static_cast<u32>(stop - quote - 1);
        if (stop >= end || bytes + 1 > capacity)
            return 0;
        for (u32 i = 0; i < bytes; ++i)
            value[i] = quote[1 + i];
        value[bytes] = 0;
        return bytes;
    }
    value[0] = 0;
    return 0;
}

// The path a <dir> or <include> names, resolved by its prefix attribute. False when it cannot be placed.
bool resolve(const char* prefix, bool include, const char* content, u32 bytes, const char* config,
             char (&path)[kPathCapacity]) noexcept {
    u32 length = 0;
    path[0] = 0;
    if (equal_prefix(prefix, "xdg")) {
        if (!(include ? xdg_home("XDG_CONFIG_HOME", "/.config", path, &length)
                      : xdg_home("XDG_DATA_HOME", "/.local/share", path, &length)))
            return false;
        return append(path, &length, "/") && append(path, &length, content, bytes);
    }
    if (equal_prefix(prefix, "relative")) {
        u32 directory = text_length(config);
        while (directory && config[directory - 1] != '/')
            --directory;
        return directory && append(path, &length, config, directory) && append(path, &length, content, bytes);
    }
    if (equal_prefix(prefix, "cwd"))
        return false;
    if (bytes && content[0] == '~' && (bytes == 1 || content[1] == '/')) {
        const char* home = environment_get("HOME");
        return absolute(home) && append(path, &length, home) && append(path, &length, content + 1, bytes - 1);
    }
    if (bytes && content[0] == '/')
        return append(path, &length, content, bytes);
    if (!include)
        return false;
    // A relative include is relative to the including file.
    u32 directory = text_length(config);
    while (directory && config[directory - 1] != '/')
        --directory;
    return directory && append(path, &length, config, directory) && append(path, &length, content, bytes);
}

void parse(Collector& c, const char* text, u32 bytes, const char* config, u32 depth) noexcept {
    const char* at = text;
    const char* const end = text + bytes;
    while (at < end) {
        if (*at != '<') {
            ++at;
            continue;
        }
        if (starts_with(at, end, "<!--")) {
            at = find(at + 4, end, "-->");
            if (!at)
                return;
            at += 3;
            continue;
        }
        if (at + 1 < end && (at[1] == '?' || at[1] == '!' || at[1] == '/')) {
            while (at < end && *at != '>')
                ++at;
            ++at;
            continue;
        }
        const char* name = at + 1;
        const char* cursor = name;
        while (cursor < end && ((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z')))
            ++cursor;
        const u32 name_length = static_cast<u32>(cursor - name);
        const char* attributes = cursor;
        while (cursor < end && *cursor != '>') {
            if (*cursor == '"' || *cursor == '\'') {
                const char quote = *cursor++;
                while (cursor < end && *cursor != quote)
                    ++cursor;
                if (cursor >= end)
                    return;
            }
            ++cursor;
        }
        if (cursor >= end)
            return;
        const bool self_closing = cursor > attributes && cursor[-1] == '/';
        const char* attributes_end = cursor;
        at = cursor + 1;
        const bool is_dir = name_length == 3 && starts_with(name, end, "dir");
        const bool is_include = name_length == 7 && starts_with(name, end, "include");
        if ((!is_dir && !is_include) || self_closing)
            continue;
        const char* begin = at;
        while (at < end && *at != '<')
            ++at;
        const char* stop = at;
        while (begin < stop && (*begin == ' ' || *begin == '\t' || *begin == '\n' || *begin == '\r'))
            ++begin;
        while (stop > begin && (stop[-1] == ' ' || stop[-1] == '\t' || stop[-1] == '\n' || stop[-1] == '\r'))
            --stop;
        if (begin == stop)
            continue;
        char prefix[16];
        if (attribute(attributes, attributes_end, "prefix", prefix, sizeof(prefix)) == 0)
            prefix[0] = 0;
        char path[kPathCapacity];
        if (!resolve(prefix, is_include, begin, static_cast<u32>(stop - begin), config, path))
            continue;
        if (is_dir)
            add_root(c, path);
        else
            include_path(c, path, depth);
    }
}

// The entry points of the configuration: $FONTCONFIG_FILE, then fonts.conf in each $FONTCONFIG_PATH directory,
// then /etc/fonts/fonts.conf.
void read_configuration(Collector& c) noexcept {
    char path[kPathCapacity];
    u32 length = 0;
    const char* file = environment_get("FONTCONFIG_FILE");
    if (absolute(file)) {
        include_path(c, file, 0);
        if (c.read_any)
            return;
    }
    const char* directories = environment_get("FONTCONFIG_PATH");
    while (directories && *directories) {
        const char* stop = directories;
        while (*stop && *stop != ':')
            ++stop;
        length = 0;
        path[0] = 0;
        if (stop > directories && absolute(directories) && append(path, &length, directories, static_cast<u32>(stop - directories)) &&
            append(path, &length, "/fonts.conf"))
            include_path(c, path, 0);
        if (c.read_any)
            return;
        directories = *stop ? stop + 1 : stop;
    }
    include_path(c, "/etc/fonts/fonts.conf", 0);
}

// The directories used when no configuration could be read.
void default_roots(Collector& c) noexcept {
    char path[kPathCapacity];
    u32 length = 0;
    add_root(c, "/usr/share/fonts");
    add_root(c, "/usr/local/share/fonts");
    add_root(c, "/run/host/fonts");
    if (xdg_home("XDG_DATA_HOME", "/.local/share", path, &length) && append(path, &length, "/fonts"))
        add_root(c, path);
    const char* home = environment_get("HOME");
    if (absolute(home)) {
        length = 0;
        path[0] = 0;
        if (append(path, &length, home) && append(path, &length, "/.fonts"))
            add_root(c, path);
        length = 0;
        path[0] = 0;
        if (append(path, &length, home) && append(path, &length, "/.nix-profile/share/fonts"))
            add_root(c, path);
    }
    const char* user = environment_get("USER");
    if (user && *user) {
        length = 0;
        path[0] = 0;
        if (append(path, &length, "/etc/profiles/per-user/") && append(path, &length, user) &&
            append(path, &length, "/share/fonts"))
            add_root(c, path);
    }
    add_root(c, "/run/current-system/sw/share/X11/fonts");
    add_root(c, "/run/current-system/sw/share/fonts");
    const char* directories = environment_get("XDG_DATA_DIRS");
    if (!directories || !*directories)
        directories = "/usr/local/share:/usr/share";
    while (*directories) {
        const char* stop = directories;
        while (*stop && *stop != ':')
            ++stop;
        length = 0;
        path[0] = 0;
        if (stop > directories && absolute(directories) && append(path, &length, directories, static_cast<u32>(stop - directories)) &&
            append(path, &length, "/fonts"))
            add_root(c, path);
        directories = *stop ? stop + 1 : stop;
    }
}

} // namespace

void font_roots_collect(Arena* arena, FontRoots* roots) noexcept {
    roots->count = 0;
    Collector collector{arena, roots, 0, false};
    read_configuration(collector);
    if (!collector.read_any || roots->count == 0)
        default_roots(collector);
}

} // namespace maicord::font_locator
