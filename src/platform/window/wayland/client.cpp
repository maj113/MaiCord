// Loads libwayland-client (required) and libwayland-cursor (optional) by soname; see abi.hpp.
#include "maicord/core/log.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/platform/window/wayland/abi.hpp"

namespace maicord::wayland {

ClientApi g_client;

namespace {

constexpr char kClientSoname[] = "libwayland-client.so.0";
constexpr char kCursorSoname[] = "libwayland-cursor.so.0";

SharedLibrary g_client_library;
bool g_client_loaded;
SharedLibrary g_cursor_library;
CursorApi g_cursor_api;
bool g_cursor_tried;
bool g_cursor_loaded;

// The symbol of each ClientApi and CursorApi member, in member order.
constexpr char kClientSymbols[] =
    "wl_display_connect\0"
    "wl_display_disconnect\0"
    "wl_display_get_fd\0"
    "wl_display_dispatch_pending\0"
    "wl_display_roundtrip\0"
    "wl_display_flush\0"
    "wl_display_prepare_read\0"
    "wl_display_read_events\0"
    "wl_display_cancel_read\0"
    "wl_display_get_error\0"
    "wl_display_get_protocol_error\0"
    "wl_proxy_add_listener\0"
    "wl_proxy_destroy\0"
    "wl_proxy_get_version\0"
    "wl_proxy_marshal_array_flags\0";
constexpr char kCursorSymbols[] =
    "wl_cursor_theme_load\0"
    "wl_cursor_theme_destroy\0"
    "wl_cursor_theme_get_cursor\0"
    "wl_cursor_image_get_buffer\0";
static_assert(shared_library_name_count(kClientSymbols) * sizeof(void*) == sizeof(ClientApi),
              "one symbol per ClientApi member");
static_assert(shared_library_name_count(kCursorSymbols) * sizeof(void*) == sizeof(CursorApi),
              "one symbol per CursorApi member");

} // namespace

bool client_load() noexcept {
    if (g_client_loaded)
        return true;
    if (!shared_library_open(&g_client_library, kClientSoname)) {
        log_write(LogLevel::warning, "wayland", "libwayland-client.so.0 is not installed");
        return false;
    }
    // Resolved in place: g_client is read only once client_load says it is complete.
    if (const char* missing = shared_library_resolve(g_client_library, &g_client, sizeof(g_client), kClientSymbols)) {
        shared_library_log_missing("wayland", kClientSoname, missing);
        shared_library_close(&g_client_library);
        return false;
    }
    g_client_loaded = true;
    return true;
}

const CursorApi* cursor_api() noexcept {
    if (g_cursor_tried)
        return g_cursor_loaded ? &g_cursor_api : nullptr;
    g_cursor_tried = true;
    if (!shared_library_open(&g_cursor_library, kCursorSoname))
        return nullptr;
    if (const char* missing = shared_library_resolve(g_cursor_library, &g_cursor_api, sizeof(g_cursor_api), kCursorSymbols)) {
        shared_library_log_missing("wayland", kCursorSoname, missing);
        shared_library_close(&g_cursor_library);
        return nullptr;
    }
    g_cursor_loaded = true;
    return &g_cursor_api;
}

} // namespace maicord::wayland
