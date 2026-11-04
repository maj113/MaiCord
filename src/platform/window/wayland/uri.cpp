#include "maicord/core/heap.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/window/wayland/uri.hpp"
#include "maicord/sys/linux/path.hpp"

namespace maicord::wayland {
namespace {

constexpr u32 kPathCapacity = 4096;

i32 hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool starts_with(const char* text, u32 length, const char* prefix) noexcept {
    u32 i = 0;
    for (; prefix[i]; ++i)
        if (i >= length || text[i] != prefix[i])
            return false;
    return true;
}

} // namespace

bool uri_percent_decode(const char* text, u32 length, char* output, u32 capacity, u32* written) noexcept {
    u32 used = 0;
    for (u32 i = 0; i < length; ++i) {
        char c = text[i];
        if (c == '%') {
            if (i + 2 >= length)
                return false;
            const i32 first = hex_digit(text[i + 1]);
            const i32 second = hex_digit(text[i + 2]);
            if (first < 0 || second < 0)
                return false;
            c = static_cast<char>(first * 16 + second);
            if (c == 0)
                return false;
            i += 2;
        }
        if (used + 1 > capacity)
            return false;
        output[used++] = c;
    }
    *written = used;
    return true;
}

bool uri_file_path(const char* uri, u32 length, char* output, u32 capacity, u32* written) noexcept {
    if (!starts_with(uri, length, "file://"))
        return false;
    const char* rest = uri + 7;
    u32 left = length - 7;
    if (left == 0)
        return false;
    if (rest[0] != '/') {
        u32 host = 0;
        while (host < left && rest[host] != '/')
            ++host;
        if (host == left || !(host == 9 && starts_with(rest, host, "localhost")))
            return false;
        rest += host;
        left -= host;
    }
    return uri_percent_decode(rest, left, output, capacity, written) && *written > 0;
}

DroppedFiles* uri_list_files(const u8* text, u32 bytes) noexcept {
    DroppedFiles* files = static_cast<DroppedFiles*>(heap_allocate(sizeof(DroppedFiles), true));
    if (!files)
        return nullptr;
    // UTF-16 never has more units than the UTF-8 it came from has bytes.
    files->characters = static_cast<wchar_t*>(heap_allocate((bytes + 1) * sizeof(wchar_t)));
    if (!files->characters) {
        heap_free(files);
        return nullptr;
    }
    u32 used = 0;
    u32 line = 0;
    bool first_line = true;
    while (line < bytes && files->count < kMaxDroppedFiles) {
        u32 end = line;
        while (end < bytes && text[end] != '\n' && text[end] != '\r')
            ++end;
        const char* entry = reinterpret_cast<const char*>(text + line);
        const u32 length = end - line;
        const bool gnome_verb = first_line && (starts_with(entry, length, "copy") || starts_with(entry, length, "cut")) &&
                                !starts_with(entry, length, "file:");
        first_line = false;
        if (length != 0 && entry[0] != '#' && !gnome_verb) {
            char path[kPathCapacity];
            u32 path_bytes = 0;
            if (uri_file_path(entry, length, path, kPathCapacity - 1, &path_bytes)) {
                path[path_bytes] = 0;
                const u32 units = path_from_utf8(path, files->characters + used, bytes + 1 - used);
                if (units != 0) {
                    files->start[files->count] = used;
                    files->length[files->count] = units;
                    ++files->count;
                    used += units + 1;  // path_from_utf8 wrote a terminator: leave it
                }
            }
        }
        line = end;
        while (line < bytes && (text[line] == '\n' || text[line] == '\r'))
            ++line;
    }
    if (files->count == 0) {
        heap_free(files->characters);
        heap_free(files);
        return nullptr;
    }
    return files;
}

} // namespace maicord::wayland
