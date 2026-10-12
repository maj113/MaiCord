#include "maicord/core/base.hpp"
#include "test_output.hpp"
namespace maicord {
void test_application_notifications(void (*)(bool, const char*) noexcept) noexcept;
namespace {
u32 failures;
void check(bool passed, const char* message) noexcept {
    test_check(&failures, passed, message);
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_application_notifications_tests_entry() noexcept {
    maicord::test_application_notifications(maicord::check);
    ExitProcess(maicord::failures ? 1 : 0);
}
