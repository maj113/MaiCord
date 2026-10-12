#include "maicord/core/base.hpp"
#include "test_output.hpp"
namespace maicord {
void test_native_notifications(void (*)(bool, const char*) noexcept) noexcept;
void test_toast_abi(void (*)(bool, const char*) noexcept) noexcept;
bool probe_desktop_notifications() noexcept;
bool probe_desktop_notifications_live(const wchar_t* report_path, bool toast) noexcept;
namespace {
u32 failures;
void check(bool passed, const char* message) noexcept {
    test_check(&failures, passed, message);
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_native_notifications_tests_entry() noexcept {
    maicord::test_native_notifications(maicord::check);
    maicord::test_toast_abi(maicord::check);
    maicord::check(maicord::probe_desktop_notifications(), "native notification XML, activation queue and lifetime probe");
    // Manual only: registers a test identity with Windows (and, with the toast
    // flag, shows one real toast). The automated suite never sets these.
    wchar_t report[1024]{}, toast[4]{};
    const DWORD n = GetEnvironmentVariableW(L"MAICORD_NOTIFICATION_LIVE_REPORT", report, 1024);
    if (n && n < 1024) {
        const DWORD t = GetEnvironmentVariableW(L"MAICORD_NOTIFICATION_LIVE_TOAST", toast, 4);
        maicord::check(maicord::probe_desktop_notifications_live(report, t == 1 && toast[0] == L'1'),
            "live notification identity enrollment, activator registration and removal");
    }
    ExitProcess(maicord::failures ? 1 : 0);
}
