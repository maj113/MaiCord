#include "../../support/test_output.hpp"

// The updater's artifact, acceptance, preparation and package fixtures (the same functions the Windows suite runs in
// tests/native_extensions.cpp) as one Linux test executable.

namespace maicord {
void test_update_manifest(void (*check)(bool, const char*) noexcept) noexcept;
void test_update_artifact(void (*check)(bool, const char*) noexcept) noexcept;
void test_update_acceptance(void (*check)(bool, const char*) noexcept) noexcept;
void test_update_prepare(void (*check)(bool, const char*) noexcept) noexcept;
void test_update_package(void (*check)(bool, const char*) noexcept) noexcept;
void test_update_portable(void (*check)(bool, const char*) noexcept) noexcept;
void test_update_linux_replay(void (*check)(bool, const char*) noexcept) noexcept;

namespace {
u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_update_pipeline_entry) {
    using namespace maicord;
    test_update_manifest(check);
    test_update_artifact(check);
    test_update_acceptance(check);
    test_update_prepare(check);
    test_update_package(check);
    test_update_portable(check);
    test_update_linux_replay(check);
    test_exit(g_failures, "update_pipeline");
}
