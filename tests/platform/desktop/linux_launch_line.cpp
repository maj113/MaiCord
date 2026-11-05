#include "maicord/core/memory.hpp"
#include "maicord/platform/desktop/integration.hpp"
#include "maicord/platform/desktop/launch_line.hpp"
#include "../../support/test_output.hpp"

// The launch's arguments as the command line desktop_launch_parse reads (src/platform/desktop/linux/launch_line.cpp):
// what the Linux entry feeds the same startup the Windows entry feeds.

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

// Builds the line for `arguments` and parses it back.
struct Launch {
    bool parsed;
    DesktopLaunch launch;
};

template <int N> Launch launch_of(const char* const (&arguments)[N]) noexcept {
    wchar_t line[4096];
    const u32 units = desktop_launch_line(N, arguments, line, 4096);
    Launch result{};
    result.parsed = desktop_launch_parse({line, units}, &result.launch);
    return result;
}

bool url_is(const DesktopLaunch& launch, const wchar_t* expected) noexcept {
    u32 length = 0;
    while (expected[length])
        ++length;
    return launch.url_present && launch.url_units == length && bytes_equal(launch.url, expected, length * sizeof(wchar_t));
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_linux_launch_line_entry) {
    using namespace maicord;
    {
        const char* const arguments[] = {"/opt/maicord/MaiCord"};
        const Launch launch = launch_of(arguments);
        check(launch.parsed && !launch.launch.url_present && !launch.launch.startup, "a plain launch has no link and no startup flag");
    }
    {
        const char* const arguments[] = {"/opt/maicord/MaiCord", "--startup"};
        const Launch launch = launch_of(arguments);
        check(launch.parsed && launch.launch.startup && !launch.launch.url_present, "--startup is the startup entry's launch");
    }
    {
        const char* const arguments[] = {"/opt/maicord/MaiCord", "--url", "--", "discord://-/channels/1/2/3"};
        const Launch launch = launch_of(arguments);
        check(launch.parsed && url_is(launch.launch, L"discord://-/channels/1/2/3"), "--url -- <link>, the shape of the desktop entry's Exec");
    }
    {
        const char* const arguments[] = {"/opt/maicord/MaiCord", "discord://-/invite/abc"};
        const Launch launch = launch_of(arguments);
        check(launch.parsed && url_is(launch.launch, L"discord://-/invite/abc"), "a bare discord:// argument is the link");
    }
    {
        const char* const arguments[] = {"/opt/my programs/MaiCord", "--url", "--", "discord://-/channels/@me/9"};
        const Launch launch = launch_of(arguments);
        check(launch.parsed && url_is(launch.launch, L"discord://-/channels/@me/9"), "a program path with a space does not shift the arguments");
    }
    {
        const char* const arguments[] = {"/opt/maicord/MaiCord", "--url", "--", "discord://-/a b\"c\\\\d\\"};
        const Launch launch = launch_of(arguments);
        check(launch.parsed && url_is(launch.launch, L"discord://-/a b\"c\\\\d\\"),
              "spaces, quotes and backslashes in a link come back as they went in");
    }
    {
        const char* const arguments[] = {"/opt/maicord/MaiCord", "", "--startup"};
        const Launch launch = launch_of(arguments);
        check(launch.parsed && launch.launch.startup, "an empty argument is kept as one and does not hide the next");
    }
    {
        const char* const arguments[] = {"/opt/maicord/MaiCord", "--url", "--", "discord://-/caf\xC3\xA9"};
        const Launch launch = launch_of(arguments);
        check(launch.parsed && url_is(launch.launch, L"discord://-/caf\u00E9"), "UTF-8 arguments become UTF-16");
    }
    {
        // An argument longer than the line holds ends the line before it: the earlier ones stay whole.
        const char* const arguments[] = {"/opt/maicord/MaiCord", "--startup",
                                         "0123456789012345678901234567890123456789012345678901234567890123456789"};
        wchar_t line[40];
        const u32 units = desktop_launch_line(3, arguments, line, 40);
        DesktopLaunch launch{};
        check(units < 40 && desktop_launch_parse({line, units}, &launch) && launch.startup,
              "a line that cannot take an argument ends before it");
    }
    test_exit(g_failures, "linux_launch_line");
}
