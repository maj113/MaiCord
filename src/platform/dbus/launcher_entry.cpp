#include "maicord/core/memory.hpp"
#include "maicord/platform/dbus/launcher_entry.hpp"
#include "maicord/platform/dbus/session.hpp"

namespace maicord::dbus {
namespace {

struct Entry {
    bool known;
    bool hook_added;
    char desktop_id[160];
    LauncherEntryState state;
};

Entry g;

u32 text_length(const char* text) noexcept {
    u32 length = 0;
    while (text[length])
        ++length;
    return length;
}

// "/com/canonical/unity/launcherentry/<number>": the number a hash of the desktop id.
void entry_path(const char* desktop_id, char (&path)[64]) noexcept {
    u32 hash = 2166136261u;
    for (u32 i = 0; desktop_id[i]; ++i)
        hash = (hash ^ static_cast<u8>(desktop_id[i])) * 16777619u;
    const char base[] = "/com/canonical/unity/launcherentry/";
    copy_bytes(path, base, sizeof(base) - 1);
    u32 length = sizeof(base) - 1;
    char digits[12];
    u32 count = 0;
    for (u32 n = hash; n || count == 0; n /= 10)
        digits[count++] = static_cast<char>('0' + n % 10);
    while (count)
        path[length++] = digits[--count];
    path[length] = 0;
}

bool send(Connection* c) noexcept {
    char uri[192];
    const char scheme[] = "application://";
    copy_bytes(uri, scheme, sizeof(scheme) - 1);
    copy_bytes(uri + sizeof(scheme) - 1, g.desktop_id, text_length(g.desktop_id) + 1);
    char path[64];
    entry_path(g.desktop_id, path);
    Builder body;
    builder_init(&body);
    builder_string(&body, uri);
    builder_open_array(&body, "{sv}");
    builder_open_entry(&body);
    builder_string(&body, "count");
    builder_open_variant(&body, "x");
    builder_i64(&body, g.state.count);
    builder_close_variant(&body);
    builder_close_entry(&body);
    builder_open_entry(&body);
    builder_string(&body, "count-visible");
    builder_open_variant(&body, "b");
    builder_bool(&body, g.state.count_visible);
    builder_close_variant(&body);
    builder_close_entry(&body);
    builder_open_entry(&body);
    builder_string(&body, "urgent");
    builder_open_variant(&body, "b");
    builder_bool(&body, g.state.urgent);
    builder_close_variant(&body);
    builder_close_entry(&body);
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

bool launcher_entry_update(const char* desktop_id, const LauncherEntryState& state) noexcept {
    if (!desktop_id || text_length(desktop_id) >= sizeof(g.desktop_id) - 20)
        return false;
    g.known = true;
    copy_bytes(g.desktop_id, desktop_id, text_length(desktop_id) + 1);
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

bool launcher_entry_clear(const char* desktop_id) noexcept {
    return launcher_entry_update(desktop_id, LauncherEntryState{0, false, false});
}

} // namespace maicord::dbus
