#pragma once

#include "maicord/core/base.hpp"

// The libwayland-client interface MaiCord uses, declared without libwayland's headers (Linux). The library is
// dlopen()ed ("libwayland-client.so.0"), so a system without Wayland fails with a message, not at load time. Its
// types and calls keep libwayland's binary layout; the protocol tables and the typed request wrappers that marshal
// through wl_proxy_marshal_array{,_constructor_versioned} are generated from third_party/wayland/*.xml by
// tools/generate_wayland_protocols.py (wayland_protocols.hpp). Only functions of libwayland-client 1.18 (Ubuntu
// 20.04) are used.

namespace maicord::wayland {

// An object of the connection: every generated interface type (wl_surface, xdg_toplevel, ...) is one.
struct Proxy;
struct wl_display;
struct wl_shm;
struct wl_buffer;
struct wl_surface;

struct Interface;

struct Message {
    const char* name;
    const char* signature;
    const Interface* const* types;
};

struct Interface {
    const char* name;
    int version;
    int method_count;
    const Message* methods;
    int event_count;
    const Message* events;
};

struct Array {
    usize size;
    usize allocation;
    void* data;
};

union Argument {
    i32 i;
    u32 u;
    i32 f;  // wl_fixed_t
    const char* s;
    void* o;
    u32 n;
    Array* a;
    i32 h;
};
static_assert(sizeof(Argument) == 8 && sizeof(Interface) == 40 && sizeof(Message) == 24 && sizeof(Array) == 24);

// wl_fixed_t: 24.8 signed fixed point.
constexpr float fixed_to_float(i32 value) noexcept {
    return static_cast<float>(value) * (1.0f / 256.0f);
}

// The entry points of libwayland-client used by the backend and the generated wrappers. Filled by client_load().
struct ClientApi {
    wl_display* (*display_connect)(const char* name) noexcept;
    void (*display_disconnect)(wl_display* display) noexcept;
    int (*display_get_fd)(wl_display* display) noexcept;
    int (*display_dispatch_pending)(wl_display* display) noexcept;
    int (*display_roundtrip)(wl_display* display) noexcept;
    int (*display_flush)(wl_display* display) noexcept;
    int (*display_prepare_read)(wl_display* display) noexcept;
    int (*display_read_events)(wl_display* display) noexcept;
    void (*display_cancel_read)(wl_display* display) noexcept;
    int (*display_get_error)(wl_display* display) noexcept;
    u32 (*display_get_protocol_error)(wl_display* display, const Interface** interface, u32* id) noexcept;
    int (*proxy_add_listener)(Proxy* proxy, void (**implementation)(), void* data) noexcept;
    void (*proxy_destroy)(Proxy* proxy) noexcept;
    u32 (*proxy_get_version)(Proxy* proxy) noexcept;
    void (*proxy_marshal_array)(Proxy* proxy, u32 opcode, Argument* arguments) noexcept;
    Proxy* (*proxy_marshal_array_constructor_versioned)(Proxy* proxy, u32 opcode, Argument* arguments,
                                                        const Interface* interface, u32 version) noexcept;
};
extern ClientApi g_client;

// Opens libwayland-client.so.0 and resolves every ClientApi entry; false (with a log line) when the library or one
// of its functions is missing. Idempotent. UI thread only.
bool client_load() noexcept;
// The warning a library of the backend (libwayland-*, libxkbcommon) logs when it lacks a function: "<library>: no
// symbol <name>".
void log_missing_symbol(const char* library, const char* name) noexcept;

// libwayland-cursor.so.0: XCursor themes loaded into wl_shm buffers, for compositors without cursor-shape.
struct CursorImage {
    u32 width;
    u32 height;
    u32 hotspot_x;
    u32 hotspot_y;
    u32 delay;
};
struct Cursor {
    u32 image_count;
    CursorImage** images;
    char* name;
};
struct CursorTheme;
struct CursorApi {
    CursorTheme* (*theme_load)(const char* name, int size, wl_shm* shm) noexcept;
    void (*theme_destroy)(CursorTheme* theme) noexcept;
    Cursor* (*theme_get_cursor)(CursorTheme* theme, const char* name) noexcept;
    wl_buffer* (*image_get_buffer)(CursorImage* image) noexcept;
};
// Null when the library is not installed.
const CursorApi* cursor_api() noexcept;

} // namespace maicord::wayland
