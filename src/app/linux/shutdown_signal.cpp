// SIGTERM, SIGINT and SIGHUP as an event of the main loop. Windows tells a closing session through window messages;
// on Linux the session manager, `kill` and a terminal send signals, whose default action ends the process with no
// state saved. The three are blocked for the whole process (threads inherit the mask, so none takes the default
// action) and read from a signalfd, a descriptor the loop waits on with everything else: the loop then asks the
// application to quit the way its Quit command does, and the normal exit saves what a close saves.
#include "shutdown_signal.hpp"

#include "../shell/window.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/sys/linux/calls.hpp"

namespace maicord::app_detail {
namespace {

int g_descriptor = -1;
Waitable g_waitable{};

constexpr u64 signal_bit(int number) noexcept {
    return u64{1} << (number - 1);
}

} // namespace

void application_shutdown_signals_install() noexcept {
    if (g_descriptor >= 0)
        return;
    const u64 mask = signal_bit(kSignalHangup) | signal_bit(kSignalInterrupt) | signal_bit(kSignalTerminate);
    // Without the descriptor the signals keep their default action: never block what nothing reads.
    const long descriptor = sys_signalfd(-1, &mask, kSignalfdNonBlock | kSignalfdCloseOnExec);
    if (descriptor < 0)
        return;
    if (sys_sigprocmask(kSignalBlock, &mask, nullptr) < 0) {
        sys_close(static_cast<int>(descriptor));
        return;
    }
    g_descriptor = static_cast<int>(descriptor);
    g_waitable = waitable_borrow_fd(g_descriptor);
}

Waitable application_platform_quit_signal() noexcept {
    return g_waitable;
}

bool application_platform_quit_requested() noexcept {
    if (g_descriptor < 0)
        return false;
    bool arrived = false;
    u8 record[kSignalfdRecordBytes];
    // Everything pending is one request; a second signal while the application is already quitting changes nothing.
    while (sys_read(g_descriptor, record, sizeof(record)) == static_cast<long>(sizeof(record)))
        arrived = true;
    return arrived;
}

} // namespace maicord::app_detail
