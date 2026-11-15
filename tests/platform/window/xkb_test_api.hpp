#pragma once

#include "maicord/core/shared_library.hpp"
#include "maicord/platform/window/wayland/xkb.hpp"

// The libxkbcommon functions that only the tests call: a keymap from layout names, its text, and the state of a
// virtual keyboard. The client's table (XkbApi) holds only what the client calls.

namespace maicord::wayland {

struct XkbTestApi {
    XkbKeymap* (*keymap_new_from_names)(XkbContext* context, const XkbRuleNames* names, int flags) noexcept;
    char* (*keymap_get_as_string)(XkbKeymap* keymap, int format) noexcept;  // malloc'd: free() it
    int (*state_update_key)(XkbState* state, u32 key, int direction) noexcept;
    u32 (*state_serialize_layout)(XkbState* state, int components) noexcept;
};

// Null when libxkbcommon.so.0 or one of the functions is missing.
inline const XkbTestApi* xkb_test_api() noexcept {
    static XkbTestApi api;
    static bool tried;
    static bool loaded;
    if (!tried) {
        tried = true;
        static constexpr char kSymbols[] =
            "xkb_keymap_new_from_names\0"
            "xkb_keymap_get_as_string\0"
            "xkb_state_update_key\0"
            "xkb_state_serialize_layout\0";
        SharedLibrary library{};
        loaded = shared_library_open(&library, "libxkbcommon.so.0") &&
                 !shared_library_resolve(library, &api, sizeof(api), kSymbols);
    }
    return loaded ? &api : nullptr;
}

} // namespace maicord::wayland
