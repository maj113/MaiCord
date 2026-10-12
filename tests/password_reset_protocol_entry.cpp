#include "maicord/core/base.hpp"
#include "test_output.hpp"

namespace maicord {
void test_password_reset_protocol(void (*check)(bool, const char*) noexcept) noexcept;
namespace {
u32 failures = 0;
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_password_reset_protocol_entry() noexcept {
    maicord::test_password_reset_protocol(maicord::check);
    constexpr char pass[] = "PASS reset protocol\n";
    constexpr char fail[] = "FAIL reset protocol\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), maicord::failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(maicord::failures ? 1 : 0);
}
