#include "maicord/core/base.hpp"
#include "test_output.hpp"
namespace maicord {
void test_opus_encoder(void (*)(bool, const char*) noexcept) noexcept;
namespace {
u32 failures;
void check(bool passed, const char* message) noexcept {
    test_check(&failures, passed, message);
}
} // namespace
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_opus_encoder_tests_entry() noexcept {
    maicord::test_opus_encoder(maicord::check);
    ExitProcess(maicord::failures ? 1 : 0);
}
