#include "maicord/core/base.hpp"
#include "test_output.hpp"

namespace maicord {
void test_notification_policy(void (*)(bool, const char*) noexcept) noexcept;
void test_notification_settings_wire(void (*)(bool, const char*) noexcept) noexcept;
void test_desktop_notification_queue(void (*)(bool, const char*) noexcept) noexcept;
void test_notification_sound_policy(void (*)(bool, const char*) noexcept) noexcept;
void test_notification_sound_admission(void (*)(bool, const char*) noexcept) noexcept;
void test_notification_controls(void (*)(bool, const char*) noexcept) noexcept;
namespace {
u32 failures;
void check(bool passed, const char* message) noexcept {
    test_check(&failures, passed, message);
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_notifications_tests_entry() noexcept {
    maicord::test_notification_policy(maicord::check);
    maicord::test_notification_settings_wire(maicord::check);
    maicord::test_desktop_notification_queue(maicord::check);
    maicord::test_notification_sound_policy(maicord::check);
    maicord::test_notification_sound_admission(maicord::check);
    maicord::test_notification_controls(maicord::check);
    ExitProcess(maicord::failures ? 1 : 0);
}
