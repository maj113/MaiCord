// Plugin catalog index tests: the verifier (plugin_index.cpp), the policy
// (plugin_index_policy.cpp) and the plugin manager records
// (plugin_manager_records.cpp).
#include "test_output.hpp"

namespace maicord {
void test_plugin_index(void (*check)(bool, const char*) noexcept) noexcept;
void test_plugin_index_policy(void (*check)(bool, const char*) noexcept) noexcept;
void test_plugin_manager_records(void (*check)(bool, const char*) noexcept) noexcept;
namespace {
u32 failures;
void check(bool passed, const char* name) noexcept { test_check(&failures, passed, name); }
} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_plugin_index_tests_entry() noexcept {
    maicord::test_plugin_index(maicord::check);
    maicord::test_plugin_index_policy(maicord::check);
    maicord::test_plugin_manager_records(maicord::check);
    ExitProcess(maicord::failures ? 1 : 0);
}
