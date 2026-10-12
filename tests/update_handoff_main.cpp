#include "maicord/core/base.hpp"
#include "test_output.hpp"

namespace maicord {
void test_update_handoff(void (*check)(bool, const char*) noexcept) noexcept;
namespace {
u32 failures;
void check(bool success, const char* name) noexcept {
    test_check(&failures, success, name);
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_update_handoff_tests_entry() noexcept {
    maicord::test_update_handoff(maicord::check);
    ExitProcess(maicord::failures ? 1 : 0);
}
