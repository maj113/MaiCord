#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/dbus/session.hpp"

namespace maicord::dbus {
namespace {

constexpr u32 kMaxHooks = 12;
constexpr u32 kRetryMs = 10000;

Connection* g_session;
bool g_wanted;
bool g_failure_logged;
u64 g_retry_at;  // monotonic ms before which no new connection is tried
SessionBusHook g_hooks[kMaxHooks];
u32 g_hook_count;
SessionBusExtra g_extras[kSessionBusExtraMaximum];
u32 g_extra_count;

void drop_connection(u64 now) noexcept {
    connection_close(g_session);
    g_session = nullptr;
    g_retry_at = now + kRetryMs;
    log_write(LogLevel::warning, "dbus", "the session bus connection ended");
}

bool try_open(u64 now) noexcept {
    if (g_session)
        return true;
    if (now < g_retry_at)
        return false;
    g_session = connection_open_session();
    if (!g_session) {
        g_retry_at = now + kRetryMs;
        if (!g_failure_logged) {
            g_failure_logged = true;
            log_write(LogLevel::info, "dbus", "no session bus: desktop settings, notifications and portals are unavailable");
        }
        return false;
    }
    g_failure_logged = false;
    for (u32 i = 0; i < g_hook_count; ++i)
        g_hooks[i](g_session);
    return true;
}

bool listed(Waitable waitable, const Waitable* list, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (list[i] == waitable)
            return true;
    return false;
}

} // namespace

bool session_bus_open() noexcept {
    g_wanted = true;
    return try_open(clock_monotonic_milliseconds());
}

Connection* session_bus() noexcept {
    return connection_alive(g_session) ? g_session : nullptr;
}

bool session_bus_add_hook(SessionBusHook hook) noexcept {
    if (g_hook_count == kMaxHooks)
        return false;
    g_hooks[g_hook_count++] = hook;
    if (g_session)
        hook(g_session);
    return true;
}

Waitable session_bus_waitable() noexcept {
    return session_bus() ? connection_waitable(g_session) : nullptr;
}

bool session_bus_add_extra(const SessionBusExtra& extra) noexcept {
    if (g_extra_count == kSessionBusExtraMaximum)
        return false;
    g_extras[g_extra_count++] = extra;
    return true;
}

u32 session_bus_extra_waitables(Waitable* out, u32 capacity) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < g_extra_count && count < capacity; ++i)
        if (const Waitable waitable = g_extras[i].waitable())
            out[count++] = waitable;
    return count;
}

u32 session_bus_wait_ms(u64 now_ms) noexcept {
    u32 wait = kNoTimeout;
    if (g_session)
        wait = connection_wait_ms(g_session, now_ms);
    else if (g_wanted)
        wait = now_ms >= g_retry_at ? 0 : static_cast<u32>(g_retry_at - now_ms);
    for (u32 i = 0; i < g_extra_count; ++i) {
        const u32 due = g_extras[i].wait_ms(now_ms);
        if (due < wait)
            wait = due;
    }
    return wait;
}

void session_bus_process(u64 now_ms, const Waitable* idle, u32 idle_count) noexcept {
    if (g_session) {
        connection_process(g_session, now_ms, !listed(connection_waitable(g_session), idle, idle_count));
        if (!connection_alive(g_session))
            drop_connection(now_ms);
    } else if (g_wanted) {
        try_open(now_ms);
    }
    for (u32 i = 0; i < g_extra_count; ++i)
        g_extras[i].process(now_ms, !listed(g_extras[i].waitable(), idle, idle_count));
}

bool session_bus_wait_for(const volatile bool* done, u32 timeout_ms) noexcept {
    const u64 deadline = clock_monotonic_milliseconds() + timeout_ms;
    while (!*done) {
        const u64 now = clock_monotonic_milliseconds();
        if (now >= deadline || !session_bus())
            break;
        Waitable waitable = session_bus_waitable();
        u32 wait = session_bus_wait_ms(now);
        const u32 left = static_cast<u32>(deadline - now);
        if (wait > left)
            wait = left;
        wait_many(&waitable, 1, wait, 0);
        session_bus_process(clock_monotonic_milliseconds());
    }
    return *done;
}

void session_bus_close() noexcept {
    connection_close(g_session);
    g_session = nullptr;
    g_wanted = false;
    g_retry_at = 0;
    g_failure_logged = false;
}

} // namespace maicord::dbus
