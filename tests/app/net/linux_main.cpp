#include "../../support/test_output.hpp"

// The HTTPS file download owner's offline fixtures (tests/app/net/https_file_download.cpp, which the Windows suite runs
// inside its core test bundle): the staged-file rules and the destination checks that fail before any name lookup.

namespace maicord {
void test_https_file_download(void (*check)(bool, const char*) noexcept) noexcept;

namespace {
u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_https_file_download_entry) {
    using namespace maicord;
    test_https_file_download(check);
    test_exit(g_failures, "https_file_download");
}
