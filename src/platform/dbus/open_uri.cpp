#include "maicord/core/heap.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/platform/dbus/open_uri.hpp"
#include "maicord/platform/dbus/portal.hpp"
#include "maicord/platform/dbus/session.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord::dbus {
namespace {

constexpr u32 kMaxUriBytes = 4096;
constexpr u32 kPortalTimeoutMs = 30000;
constexpr long kCallRtSigaction = 13;
constexpr int kSignalChild = 17;

using SpawnFunction = int (*)(int* pid, const char* file, const void* actions, const void* attributes,
                              char* const argv[], char* const environment[]);

struct KernelSigaction {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
};

SharedLibrary g_libc;
SpawnFunction g_spawn;
bool g_spawn_resolved;
bool g_children_ignored;

u32 length_of(const char* text) noexcept {
    u32 length = 0;
    while (text[length])
        ++length;
    return length;
}

bool resolve_spawn() noexcept {
    if (g_spawn_resolved)
        return g_spawn != nullptr;
    g_spawn_resolved = true;
    // libc is loaded already: opening it again only gives its handle.
    if (shared_library_open(&g_libc, "libc.so.6"))
        g_spawn = reinterpret_cast<SpawnFunction>(shared_library_symbol(g_libc, "posix_spawnp"));
    return g_spawn != nullptr;
}

// xdg-open, with the URI as its only argument.
bool spawn_xdg_open(const char* uri) noexcept {
    if (!resolve_spawn()) {
        log_write(LogLevel::warning, "open_uri", "posix_spawnp is not available");
        return false;
    }
    if (!g_children_ignored) {
        g_children_ignored = true;
        KernelSigaction action{};
        action.handler = 1;  // SIG_IGN: children are reaped by the kernel
        syscall4(kCallRtSigaction, kSignalChild, reinterpret_cast<long>(&action), 0, 8);
    }
    char program[] = "xdg-open";
    char* argv[3] = {program, const_cast<char*>(uri), nullptr};
    int pid = 0;
    const int result = g_spawn(&pid, "xdg-open", nullptr, nullptr, argv, process_startup().environment);
    if (result != 0) {
        log_u64(LogLevel::warning, "open_uri", "xdg-open could not start, error", static_cast<u64>(result));
        return false;
    }
    return true;
}

struct Pending {
    char uri[kMaxUriBytes];
};

void on_portal_reply(void* context, Status status, const Message* reply) noexcept {
    Pending* pending = static_cast<Pending*>(context);
    if (status != Status::ok) {
        log_write(LogLevel::info, "open_uri",
                  status == Status::error_reply && reply && reply->error_name ? reply->error_name
                                                                              : "the OpenURI portal did not answer");
        spawn_xdg_open(pending->uri);
    }
    heap_free(pending);
}

} // namespace

bool open_uri_valid(const char* uri) noexcept {
    if (!uri)
        return false;
    const u32 length = length_of(uri);
    if (length == 0 || length >= kMaxUriBytes)
        return false;
    const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (!alpha(uri[0]))
        return false;
    u32 i = 1;
    while (uri[i] && (alpha(uri[i]) || (uri[i] >= '0' && uri[i] <= '9') || uri[i] == '+' || uri[i] == '-' || uri[i] == '.'))
        ++i;
    if (uri[i] != ':')
        return false;
    for (; i < length; ++i)
        if (static_cast<u8>(uri[i]) < 0x20 || uri[i] == 0x7F)
            return false;
    return true;
}

bool open_uri(const char* uri, const char* activation_token) noexcept {
    if (!open_uri_valid(uri))
        return false;
    Connection* c = session_bus_open() ? session_bus() : nullptr;
    if (!c)
        return spawn_xdg_open(uri);
    Pending* pending = static_cast<Pending*>(heap_allocate(sizeof(Pending)));
    if (!pending)
        return false;
    copy_bytes(pending->uri, uri, length_of(uri) + 1);
    Builder body;
    builder_init(&body);
    builder_string(&body, portal_parent_window());
    builder_string(&body, uri);
    builder_open_array(&body, "{sv}");
    if (activation_token && *activation_token) {
        builder_open_entry(&body);
        builder_string(&body, "activation_token");
        builder_open_variant(&body, "s");
        builder_string(&body, activation_token);
        builder_close_variant(&body);
        builder_close_entry(&body);
    }
    builder_close_array(&body);
    const CallTarget target{"org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                            "org.freedesktop.portal.OpenURI", "OpenURI"};
    const bool sent = connection_call(c, target, &body, kPortalTimeoutMs, on_portal_reply, pending);
    builder_release(&body);
    if (!sent) {
        heap_free(pending);
        return spawn_xdg_open(uri);
    }
    return true;
}

} // namespace maicord::dbus
