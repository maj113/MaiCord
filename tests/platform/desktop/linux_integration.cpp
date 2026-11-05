#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/os/file.hpp"
#include "maicord/platform/desktop/integration.hpp"
#include "../../support/test_output.hpp"

// The Linux startup and protocol registration (src/platform/desktop/linux/integration.cpp): an XDG autostart desktop
// file, a scheme handler desktop file and the user's default in mimeapps.list, all under XDG_CONFIG_HOME and
// XDG_DATA_HOME, which the test registration points at scratch directories. The questions are the Windows ones:
// absent, ours, other.

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

template <u32 N> ConstSlice<wchar_t> wide(const wchar_t (&text)[N]) noexcept {
    return {text, N - 1};
}

constexpr wchar_t kProgram[] = L"/opt/maicord/bin/MaiCord";
constexpr wchar_t kOther[] = L"/usr/local/bin/other-client";

bool path_in(const wchar_t* variable, const wchar_t* relative, wchar_t* out, u32 capacity) noexcept {
    u32 length = environment_variable(variable, out, capacity);
    return length && length < capacity && wide_append(out, capacity, &length, relative);
}

// The whole file as text; false when it cannot be read.
bool read_file(const wchar_t* path, char* text, u32 capacity, u32* count) noexcept {
    const FileHandle file = file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead, 0});
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const bool ok = file_read(file, text, capacity - 1, count);
    file_close(file);
    text[ok ? *count : 0] = 0;
    return ok;
}

bool write_file(const wchar_t* path, const char* text) noexcept {
    const FileHandle file = file_open(path, {FileAccess::write, FileCreate::create_always, kFileShareRead, 0});
    if (file == INVALID_HANDLE_VALUE)
        return false;
    u32 bytes = 0;
    while (text[bytes])
        ++bytes;
    u32 written = 0;
    const bool ok = file_write(file, text, bytes, &written) && written == bytes;
    file_close(file);
    return ok;
}

bool contains(const char* text, const char* needle) noexcept {
    for (; *text; ++text) {
        u32 i = 0;
        while (needle[i] && text[i] == needle[i])
            ++i;
        if (!needle[i])
            return true;
    }
    return false;
}

u32 occurrences(const char* text, const char* needle) noexcept {
    u32 found = 0;
    for (; *text; ++text) {
        u32 i = 0;
        while (needle[i] && text[i] == needle[i])
            ++i;
        found += needle[i] == 0;
    }
    return found;
}

void test_startup() noexcept {
    wchar_t path[1024];
    const DesktopRegistryScope scope = desktop_registry_user_scope();
    check(path_in(L"XDG_CONFIG_HOME", L"/autostart/io.github.maj113.MaiCord.desktop", path, 1024),
          "the test registration sets XDG_CONFIG_HOME");
    file_remove(path);
    check(desktop_startup_query(scope, wide(kProgram)) == DesktopRegistration::absent, "startup: nothing registered");
    check(desktop_startup_set(scope, wide(kProgram), true), "startup: enabling writes the autostart entry");
    check(desktop_startup_query(scope, wide(kProgram)) == DesktopRegistration::ours, "startup: the entry is ours");
    check(desktop_startup_query(scope, wide(kOther)) == DesktopRegistration::other,
          "startup: another executable sees an entry that is not its");
    char text[2048];
    u32 count = 0;
    check(read_file(path, text, sizeof(text), &count) && contains(text, "[Desktop Entry]") &&
              contains(text, "Exec=\"/opt/maicord/bin/MaiCord\" --startup") &&
              contains(text, "X-GNOME-Autostart-enabled=true"),
          "startup: the desktop entry names the program and --startup");
    check(write_file(path, "[Desktop Entry]\nType=Application\nExec=\"/opt/maicord/bin/MaiCord\" --startup\nHidden=true\n") &&
              desktop_startup_query(scope, wide(kProgram)) == DesktopRegistration::absent,
          "startup: a session that disabled the entry counts as absent");
    check(desktop_startup_set(scope, wide(kProgram), true) &&
              desktop_startup_query(scope, wide(kProgram)) == DesktopRegistration::ours,
          "startup: enabling again rewrites a disabled entry");
    check(desktop_startup_set(scope, wide(kProgram), false) &&
              desktop_startup_query(scope, wide(kProgram)) == DesktopRegistration::absent,
          "startup: disabling removes the entry");
    check(desktop_startup_set(scope, wide(kProgram), false), "startup: disabling twice succeeds");
    check(!desktop_startup_set(scope, wide(L"relative/program"), true) &&
              !desktop_startup_set(scope, wide(L"/opt/with\"quote"), true),
          "startup: a relative path and a quote in the path are refused");
}

void test_protocol() noexcept {
    wchar_t mimeapps[1024], handler[1024], maicord_handler[1024];
    const DesktopRegistryScope discord = desktop_registry_user_scope();
    const DesktopRegistryScope maicord = desktop_registry_maicord_scope();
    check(path_in(L"XDG_CONFIG_HOME", L"/mimeapps.list", mimeapps, 1024) &&
              path_in(L"XDG_DATA_HOME", L"/applications/io.github.maj113.MaiCord.discord.desktop", handler, 1024) &&
              path_in(L"XDG_DATA_HOME", L"/applications/io.github.maj113.MaiCord.maicord.desktop", maicord_handler, 1024),
          "the test registration sets the XDG directories");
    file_remove(handler);
    file_remove(maicord_handler);
    // Defaults of other applications stay when ours is set and removed.
    check(write_file(mimeapps, "[Added Associations]\ntext/plain=editor.desktop;\n\n[Default Applications]\n"
                               "x-scheme-handler/https=browser.desktop;\n"),
          "protocol: a mimeapps.list of other applications exists");
    check(desktop_protocol_query(discord, wide(kProgram)) == DesktopRegistration::absent, "protocol: nothing registered");
    check(desktop_protocol_set(discord, wide(kProgram), true), "protocol: enabling writes the handler");
    check(desktop_protocol_query(discord, wide(kProgram)) == DesktopRegistration::ours, "protocol: the handler is ours");
    check(desktop_protocol_query(discord, wide(kOther)) == DesktopRegistration::other,
          "protocol: another executable sees a handler that is not its");
    check(desktop_protocol_query(maicord, wide(kProgram)) == DesktopRegistration::absent,
          "protocol: the maicord scheme is separate");
    char text[4096];
    u32 count = 0;
    check(read_file(handler, text, sizeof(text), &count) && contains(text, "Exec=\"/opt/maicord/bin/MaiCord\" --url -- %u") &&
              contains(text, "MimeType=x-scheme-handler/discord;"),
          "protocol: the handler names the program, the link argument and the scheme");
    check(read_file(mimeapps, text, sizeof(text), &count) &&
              contains(text, "x-scheme-handler/discord=io.github.maj113.MaiCord.discord.desktop;") &&
              contains(text, "x-scheme-handler/https=browser.desktop;") && contains(text, "text/plain=editor.desktop;"),
          "protocol: mimeapps.list gains our default and keeps the others");
    check(desktop_protocol_set(maicord, wide(kProgram), true) &&
              desktop_protocol_query(maicord, wide(kProgram)) == DesktopRegistration::ours &&
              desktop_protocol_query(discord, wide(kProgram)) == DesktopRegistration::ours,
          "protocol: both schemes can be ours");
    check(desktop_protocol_set(discord, wide(kProgram), true) && read_file(mimeapps, text, sizeof(text), &count) &&
              occurrences(text, "x-scheme-handler/discord=") == 1 &&
              occurrences(text, "x-scheme-handler/maicord=") == 1,
          "protocol: enabling twice keeps one default line per scheme");
    check(write_file(mimeapps, "[Default Applications]\nx-scheme-handler/discord=other.desktop;\n") &&
              desktop_protocol_query(discord, wide(kProgram)) == DesktopRegistration::other,
          "protocol: a default that is another application's is reported as other");
    check(desktop_protocol_set(discord, wide(kProgram), false) && read_file(mimeapps, text, sizeof(text), &count) &&
              contains(text, "x-scheme-handler/discord=other.desktop;"),
          "protocol: disabling never removes another application's default");
    check(desktop_protocol_set(discord, wide(kProgram), true) &&
              desktop_protocol_set(discord, wide(kProgram), false) &&
              desktop_protocol_query(discord, wide(kProgram)) == DesktopRegistration::absent,
          "protocol: enabling then disabling leaves nothing registered");
    check(desktop_protocol_set(maicord, wide(kProgram), false) &&
              desktop_protocol_query(maicord, wide(kProgram)) == DesktopRegistration::absent,
          "protocol: disabling the maicord scheme");
    check(!desktop_protocol_set(discord, wide(L"relative/program"), true), "protocol: a relative path is refused");
}

// The AppImage runtime sets APPIMAGE (the file) and APPDIR (its mount): the test registration puts this test's own directory
// in APPDIR and a file under it in APPIMAGE, as the runtime does for the program it mounts.
void test_appimage() noexcept {
    wchar_t path[1024], expected[1024];
    const u32 units = desktop_appimage_path(path, 1024);
    const u32 wanted = environment_variable(L"APPIMAGE", expected, 1024);
    check(units && units == wanted && bytes_equal(path, expected, units * sizeof(wchar_t)),
          "appimage: from inside the mount named by APPDIR the registration names the file in APPIMAGE");
    wchar_t small[8];
    check(desktop_appimage_path(small, 8) == 0, "appimage: a path that does not fit is refused");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_linux_integration_entry) {
    using namespace maicord;
    test_startup();
    test_protocol();
    test_appimage();
    test_exit(g_failures, "linux_integration");
}
