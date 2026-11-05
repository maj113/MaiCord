// Loads libwayland-client (required) and libwayland-cursor (optional) by soname; see abi.hpp.
#include "maicord/core/log.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/platform/window/wayland/abi.hpp"

namespace maicord::wayland {

ClientApi g_client;

namespace {

SharedLibrary g_client_library;
bool g_client_loaded;
SharedLibrary g_cursor_library;
CursorApi g_cursor_api;
bool g_cursor_tried;
bool g_cursor_loaded;

// Resolves a function of a table, remembering the first one that is missing.
struct Resolver {
    const SharedLibrary& library;
    const char* missing;

    template <typename Function> void operator()(Function* slot, const char* name) noexcept {
        if (missing)
            return;
        void* address = shared_library_symbol(library, name);
        if (!address) {
            missing = name;
            return;
        }
        *slot = reinterpret_cast<Function>(address);
    }
};

void log_missing(const char* library, const char* name) noexcept {
    char message[128];
    u32 length = 0;
    const char* const parts[] = {library, ": no symbol ", name};
    for (const char* part : parts)
        for (; *part && length + 1 < sizeof(message); ++part)
            message[length++] = *part;
    message[length] = 0;
    log_write(LogLevel::warning, "wayland", message);
}

} // namespace

bool client_load() noexcept {
    if (g_client_loaded)
        return true;
    if (!shared_library_open(&g_client_library, "libwayland-client.so.0")) {
        log_write(LogLevel::warning, "wayland", "libwayland-client.so.0 is not installed");
        return false;
    }
    Resolver resolve{g_client_library, nullptr};
    ClientApi api{};
    resolve(&api.display_connect, "wl_display_connect");
    resolve(&api.display_disconnect, "wl_display_disconnect");
    resolve(&api.display_get_fd, "wl_display_get_fd");
    resolve(&api.display_dispatch_pending, "wl_display_dispatch_pending");
    resolve(&api.display_roundtrip, "wl_display_roundtrip");
    resolve(&api.display_flush, "wl_display_flush");
    resolve(&api.display_prepare_read, "wl_display_prepare_read");
    resolve(&api.display_read_events, "wl_display_read_events");
    resolve(&api.display_cancel_read, "wl_display_cancel_read");
    resolve(&api.display_get_error, "wl_display_get_error");
    resolve(&api.display_get_protocol_error, "wl_display_get_protocol_error");
    resolve(&api.proxy_add_listener, "wl_proxy_add_listener");
    resolve(&api.proxy_destroy, "wl_proxy_destroy");
    resolve(&api.proxy_get_version, "wl_proxy_get_version");
    resolve(&api.proxy_marshal_array, "wl_proxy_marshal_array");
    resolve(&api.proxy_marshal_array_constructor_versioned, "wl_proxy_marshal_array_constructor_versioned");
    if (resolve.missing) {
        log_missing("libwayland-client.so.0", resolve.missing);
        shared_library_close(&g_client_library);
        return false;
    }
    g_client = api;
    g_client_loaded = true;
    return true;
}

const CursorApi* cursor_api() noexcept {
    if (g_cursor_tried)
        return g_cursor_loaded ? &g_cursor_api : nullptr;
    g_cursor_tried = true;
    if (!shared_library_open(&g_cursor_library, "libwayland-cursor.so.0"))
        return nullptr;
    Resolver resolve{g_cursor_library, nullptr};
    CursorApi api{};
    resolve(&api.theme_load, "wl_cursor_theme_load");
    resolve(&api.theme_destroy, "wl_cursor_theme_destroy");
    resolve(&api.theme_get_cursor, "wl_cursor_theme_get_cursor");
    resolve(&api.image_get_buffer, "wl_cursor_image_get_buffer");
    if (resolve.missing) {
        log_missing("libwayland-cursor.so.0", resolve.missing);
        shared_library_close(&g_cursor_library);
        return nullptr;
    }
    g_cursor_api = api;
    g_cursor_loaded = true;
    return &g_cursor_api;
}

} // namespace maicord::wayland
