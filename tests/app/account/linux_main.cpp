#include "../../support/test_output.hpp"

// The client identity fixtures (tests/app/account/client_request_metadata.cpp and client_identity.cpp, which the Windows
// suite runs inside its core test bundle): the startup configuration of the running host, the request-time super
// properties and languages, and the Linux client's properties for fixture hosts.

namespace maicord {
void test_client_request_metadata(void (*check)(bool, const char*) noexcept) noexcept;
void test_client_identity(void (*check)(bool, const char*) noexcept) noexcept;

namespace {
u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_client_identity_entry) {
    using namespace maicord;
    test_client_request_metadata(check);
    test_client_identity(check);
    test_exit(g_failures, "client_identity");
}
