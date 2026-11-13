#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/dbus/launcher_entry.hpp"
#include "maicord/platform/dbus/session.hpp"

namespace maicord::dbus {
namespace {

constexpr char kScheme[] = "application://";
// The application's desktop file (packaging/linux), which the dock matches the entry to.
constexpr char kDesktopUri[] = "application://io.github.maj113.MaiCord.desktop";
constexpr char kPathBase[] = "/com/canonical/unity/launcherentry/";

// The FNV-1a hash of the desktop id, the last element of the entry's object path.
constexpr u32 entry_hash() noexcept {
    u32 hash = 2166136261u;
    for (u32 i = sizeof(kScheme) - 1; kDesktopUri[i]; ++i)
        hash = (hash ^ static_cast<u8>(kDesktopUri[i])) * 16777619u;
    return hash;
}

struct Entry {
    bool known;
    bool hook_added;
    LauncherEntryState state;
};

Entry g;

bool send(Connection* c) noexcept {
    char path[64];
    copy_bytes(path, kPathBase, sizeof(kPathBase) - 1);
    path[sizeof(kPathBase) - 1 + decimal_write(path + sizeof(kPathBase) - 1, entry_hash())] = 0;
    Builder body;
    builder_init(&body);
    builder_string(&body, kDesktopUri);
    builder_open_array(&body, "{sv}");
    builder_open_dict_entry(&body, "count", "x");
    builder_i64(&body, g.state.count);
    builder_close_dict_entry(&body);
    builder_dict_bool(&body, "count-visible", g.state.count_visible);
    builder_dict_bool(&body, "urgent", g.state.urgent);
    builder_close_array(&body);
    const bool sent = builder_ok(body) && connection_emit(c, path, "com.canonical.Unity.LauncherEntry", "Update", &body);
    builder_release(&body);
    return sent;
}

void on_session(Connection* c) noexcept {
    if (g.known)
        send(c);
}

} // namespace

bool launcher_entry_update(const LauncherEntryState& state) noexcept {
    g.known = true;
    g.state = state;
    if (!g.hook_added) {
        g.hook_added = true;
        session_bus_add_hook(on_session);
    }
    if (!session_bus_open())
        return false;
    Connection* c = session_bus();
    return c && send(c);
}

} // namespace maicord::dbus
