#include "maicord/platform/dbus/logout.hpp"
#include "maicord/platform/dbus/session.hpp"

namespace maicord::dbus {

bool logout_in_progress() noexcept {
    Connection* c = session_bus_open() ? session_bus() : nullptr;
    if (!c)
        return false;
    // ksmserver answers at once, also during a logout. The limit only matters when it hangs.
    constexpr u32 kTimeoutMs = 500;
    const CallTarget target{"org.kde.ksmserver", "/KSMServer", "org.kde.KSMServerInterface", "isShuttingDown"};
    Message reply;
    const Status status = connection_call_sync(c, target, nullptr, kTimeoutMs, &reply, flag_no_auto_start);
    bool shutting_down = false;
    if (status == Status::ok) {
        Reader r;
        message_reader(reply, &r);
        if (!reader_bool(&r, &shutting_down))
            shutting_down = false;
    }
    if (status == Status::ok || status == Status::error_reply)
        message_release(&reply);
    return shutting_down;
}

} // namespace maicord::dbus
