// maicord/platform/desktop/shell_open.hpp on Linux: the OpenURI portal (xdg-open when it refuses) through
// src/platform/dbus/open_uri.cpp. A path becomes a file: URI; a file is "revealed" by opening its folder, since
// the portals cannot select a file in the manager.
#include "maicord/platform/desktop/shell_open.hpp"
#include "maicord/os/text.hpp"
#include "maicord/platform/dbus/open_uri.hpp"

namespace maicord {
namespace {

constexpr u32 kMaximumUriBytes = 4096;

bool unreserved(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~' || c == '/';
}

// "file://" and the UTF-8 of an absolute path, percent-encoded; backslashes read as separators.
bool file_uri(const wchar_t* path, u32 units, char* output, u32 capacity) noexcept {
    char text[kMaximumUriBytes];
    const u32 bytes = utf16_to_utf8(path, units, text, sizeof(text), true);
    if (bytes == 0 || text[0] != '/')
        return false;
    static constexpr char kPrefix[] = "file://";
    u32 used = sizeof(kPrefix) - 1;
    if (capacity <= used)
        return false;
    for (u32 index = 0; index < used; ++index)
        output[index] = kPrefix[index];
    for (u32 index = 0; index < bytes; ++index) {
        char c = text[index];
        if (c == '\\')
            c = '/';
        if (unreserved(c)) {
            if (used + 1 >= capacity)
                return false;
            output[used++] = c;
        } else {
            if (used + 3 >= capacity)
                return false;
            static constexpr char kHex[] = "0123456789ABCDEF";
            output[used++] = '%';
            output[used++] = kHex[static_cast<u8>(c) >> 4];
            output[used++] = kHex[static_cast<u8>(c) & 15];
        }
    }
    output[used] = 0;
    return true;
}

u32 wide_length(const wchar_t* text) noexcept {
    u32 length = 0;
    while (text[length])
        ++length;
    return length;
}

} // namespace

bool desktop_open_target(Window, const wchar_t* target) noexcept {
    if (!target || !*target)
        return false;
    const u32 units = wide_length(target);
    char text[kMaximumUriBytes];
    // A link is opened as written; anything that is not one is an absolute path.
    if (const u32 bytes = utf16_to_utf8(target, units, text, sizeof(text) - 1, true)) {
        text[bytes] = 0;
        if (dbus::open_uri_valid(text))
            return dbus::open_uri(text, nullptr);
    }
    char uri[kMaximumUriBytes];
    return file_uri(target, units, uri, sizeof(uri)) && dbus::open_uri(uri, nullptr);
}

bool desktop_reveal_file(const wchar_t* path) noexcept {
    if (!path || !*path)
        return false;
    u32 units = wide_length(path);
    while (units && path[units - 1] != L'/' && path[units - 1] != L'\\')
        --units;
    // The folder, without its trailing separator (the root keeps its own).
    const u32 folder = units > 1 ? units - 1 : units;
    char uri[kMaximumUriBytes];
    return folder && file_uri(path, folder, uri, sizeof(uri)) && dbus::open_uri(uri, nullptr);
}

bool desktop_open_notification_settings(Window) noexcept {
    return false;
}

void desktop_alert_beep() noexcept {}

} // namespace maicord
