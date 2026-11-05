#include "maicord/os/process.hpp"
#include "../support/test_output.hpp"

// Starting another program (maicord/os/process.hpp): a program that exists starts, and a missing one and an
// over-long path are refused with the system's error.

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

#if defined(_WIN32)
constexpr wchar_t kProgram[] = L"C:\\Windows\\System32\\hostname.exe";
constexpr wchar_t kMissing[] = L"C:\\maicord-no-such-directory\\program.exe";
#else
constexpr wchar_t kProgram[] = L"/bin/true";
constexpr wchar_t kMissing[] = L"/maicord-no-such-directory/program";
#endif

void test_launch() noexcept {
    u32 error = 0;
    check(process_launch_detached(kProgram, &error), "an existing program starts");
    error = 0;
    check(!process_launch_detached(kMissing, &error) && error != 0, "a missing program is refused with an error");
    check(!process_launch_detached(kMissing, nullptr), "the error pointer is optional");
    check(!process_launch_detached(nullptr, &error), "no path is refused");
    wchar_t long_path[kProcessLaunchMaximumPathCharacters + 8];
    for (wchar_t& unit : long_path)
        unit = L'a';
    long_path[0] = kProgram[0];
    long_path[kProcessLaunchMaximumPathCharacters + 7] = 0;
    check(!process_launch_detached(long_path, &error), "a path over the limit is refused");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_os_process_entry) {
    using namespace maicord;
    test_launch();
    test_exit(g_failures, "os_process");
}
