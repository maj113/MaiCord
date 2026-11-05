// The process entry of the Linux client: the arguments as the one command line the launch parser reads (the Windows
// entry hands it GetCommandLineW's), a second launch handing its link to the running client, the termination signals
// turned into a loop event, then the application.
#include "../notifications/desktop.hpp"
#include "../shell/run.hpp"
#include "shutdown_signal.hpp"
#include "maicord/platform/desktop/launch_line.hpp"
#include "maicord/sys/linux/process.hpp"

namespace {

// Room for a link of kDesktopUrlCapacity units, its options and the program's path.
constexpr maicord::u32 kCommandLineCapacity = 4096;

} // namespace

extern "C" [[noreturn]] void maicord_entry() noexcept {
    using namespace maicord;
    wchar_t line[kCommandLineCapacity];
    const ProcessStartup& startup = process_startup();
    const u32 units = desktop_launch_line(startup.argc, startup.argv, line, kCommandLineCapacity);
    const ConstSlice<wchar_t> command{line, units};
    // A second launch hands its discord:// link to the running client.
    if (!app_detail::application_desktop_forward_existing(command)) {
        // Before any thread exists: the signals' default action (end the process, save nothing) must not stay anywhere.
        app_detail::application_shutdown_signals_install();
        app_detail::application_desktop_prepare(command);
        sys_exit_group(application_run());
    }
    sys_exit_group(0);
}
