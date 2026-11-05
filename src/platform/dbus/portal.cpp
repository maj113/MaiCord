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

u32 portal_interface_version(const char* interface, u32 timeout_ms) noexcept {
    if (!session_bus_open())
        return 0;
    Connection* c = session_bus();
    if (!c)
        return 0;
    Builder body;
    builder_init(&body);
    builder_string(&body, interface);
    builder_string(&body, "version");
    const CallTarget target{"org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                            "org.freedesktop.DBus.Properties", "Get"};
    Message reply;
    const Status status = connection_call_sync(c, target, &body, timeout_ms, &reply);
    builder_release(&body);
    u32 version = 0;
    if (status == Status::ok) {
        Reader r;
        message_reader(reply, &r);
        if (!reader_enter_variant(&r, nullptr) || !reader_u32(&r, &version))
            version = 0;
    }
    if (status == Status::ok || status == Status::error_reply)
        message_release(&reply);
    return version;
}

u32 inhibit_portal_version(u32 timeout_ms) noexcept {
    return portal_interface_version("org.freedesktop.portal.Inhibit", timeout_ms);
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
