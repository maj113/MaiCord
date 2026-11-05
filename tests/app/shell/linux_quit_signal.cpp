#include "linux/shutdown_signal.hpp"
#include "shell/window.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "../../support/test_output.hpp"

// SIGTERM, SIGINT and SIGHUP are blocked for the whole process and read from a descriptor the main loop waits on
// (src/app/linux/shutdown_signal.cpp): a signal sent to the process is a loop event, not the end of the process,
// and every thread started after the install keeps them blocked.

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

constexpr u64 bit(int signal) noexcept {
    return u64{1} << (signal - 1);
}

u32 mask_in_thread(void* result) noexcept {
    u64 blocked = 0;
    sys_sigprocmask(kSignalBlock, nullptr, &blocked);
    *static_cast<u64*>(result) = blocked;
    return 0;
}

void test_signals() noexcept {
    using namespace app_detail;
    check(application_platform_quit_signal() == nullptr && !application_platform_quit_requested(),
          "before the install there is no descriptor and nothing requested");
    application_shutdown_signals_install();
    const Waitable waitable = application_platform_quit_signal();
    check(waitable != nullptr, "the install opens the descriptor");
    check(wait_one(waitable, 0) == WAIT_TIMEOUT && !application_platform_quit_requested(), "no signal, no request");

    const int pid = static_cast<int>(sys_getpid());
    const int signals[] = {kSignalTerminate, kSignalInterrupt, kSignalHangup};
    for (const int signal : signals) {
        check(sys_kill(pid, signal) == 0, "the signal is sent to this process");
        // The process is still here, and the descriptor is readable.
        check(wait_one(waitable, 2000) == WAIT_OBJECT_0, "a signal makes the descriptor readable");
        check(application_platform_quit_requested(), "a signal is a request to quit");
        check(!application_platform_quit_requested() && wait_one(waitable, 0) == WAIT_TIMEOUT,
              "the request is consumed once");
    }

    // Two signals before the loop looks are one request.
    check(sys_kill(pid, kSignalTerminate) == 0 && sys_kill(pid, kSignalInterrupt) == 0, "two signals are sent");
    check(wait_one(waitable, 2000) == WAIT_OBJECT_0 && application_platform_quit_requested() &&
              !application_platform_quit_requested(),
          "signals that arrive together are one request");

    u64 blocked = 0;
    sys_sigprocmask(kSignalBlock, nullptr, &blocked);
    const u64 wanted = bit(kSignalTerminate) | bit(kSignalInterrupt) | bit(kSignalHangup);
    check((blocked & wanted) == wanted, "the three signals are blocked on the main thread");
    u64 thread_blocked = 0;
    Thread* thread = thread_start(mask_in_thread, &thread_blocked, 0, nullptr);
    check(thread != nullptr && thread_wait(thread, 5000), "a thread started after the install runs");
    if (thread)
        thread_close(thread);
    check((thread_blocked & wanted) == wanted, "a thread started after the install has them blocked");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_linux_quit_signal_entry) {
    maicord::test_signals();
    maicord::test_exit(maicord::g_failures, "linux_quit_signal");
}
