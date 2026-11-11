#include "maicord/platform/dbus/portal.hpp"
#include "maicord/platform/dbus/session.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord::dbus {

bool portal_sandboxed() noexcept {
    const long opened = sys_openat(kAtCurrentDirectory, "/.flatpak-info", kOpenReadOnly | kOpenCloseOnExec);
    if (!syscall_failed(opened)) {
        sys_close(static_cast<int>(opened));
        return true;
    }
    const char* snap = environment_get("SNAP");
    return snap && *snap;
}

u32 portal_property(Connection* c, const char* interface, const char* name, u32 timeout_ms) noexcept {
    Builder body;
    builder_init(&body);
    builder_string(&body, interface);
    builder_string(&body, name);
    const CallTarget target{"org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                            "org.freedesktop.DBus.Properties", "Get"};
    Message reply;
    const Status status = connection_call_sync(c, target, &body, timeout_ms, &reply);
    builder_release(&body);
    u32 value = 0;
    if (status == Status::ok) {
        Reader r;
        message_reader(reply, &r);
        if (!reader_enter_variant(&r, nullptr) || !reader_u32(&r, &value))
            value = 0;
    }
    if (status == Status::ok || status == Status::error_reply)
        message_release(&reply);
    return value;
}

u32 portal_interface_version(const char* interface, u32 timeout_ms) noexcept {
    Connection* c = session_bus_open() ? session_bus() : nullptr;
    return c ? portal_property(c, interface, "version", timeout_ms) : 0;
}

namespace {
char g_parent_window[96];
} // namespace

void portal_set_parent_window(const char* identifier) noexcept {
    u32 length = 0;
    for (; identifier && identifier[length] && length + 1 < sizeof(g_parent_window); ++length)
        g_parent_window[length] = identifier[length];
    g_parent_window[length] = 0;
}

const char* portal_parent_window() noexcept {
    return g_parent_window;
}

} // namespace maicord::dbus
