#include "maicord/core/log.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/platform/window/wayland/abi.hpp"
#include "maicord/platform/window/wayland/keymap.hpp"
#include "maicord/platform/window/wayland/xkb.hpp"

namespace maicord::wayland {
namespace {

SharedLibrary g_library;
XkbApi g_api;
bool g_tried;
bool g_loaded;

constexpr char kSoname[] = "libxkbcommon.so.0";

// The symbol of each XkbApi member, in member order.
constexpr char kSymbols[] =
    "xkb_context_new\0"
    "xkb_context_unref\0"
    "xkb_context_set_log_level\0"
    "xkb_keymap_new_from_buffer\0"
    "xkb_keymap_new_from_names\0"
    "xkb_keymap_unref\0"
    "xkb_keymap_get_as_string\0"
    "xkb_keymap_key_repeats\0"
    "xkb_keymap_key_get_syms_by_level\0"
    "xkb_keymap_mod_get_index\0"
    "xkb_state_new\0"
    "xkb_state_unref\0"
    "xkb_state_update_mask\0"
    "xkb_state_update_key\0"
    "xkb_state_serialize_mods\0"
    "xkb_state_serialize_layout\0"
    "xkb_state_key_get_one_sym\0"
    "xkb_state_key_get_utf32\0"
    "xkb_state_key_get_layout\0"
    "xkb_state_mod_name_is_active\0"
    "xkb_compose_table_new_from_locale\0"
    "xkb_compose_table_unref\0"
    "xkb_compose_state_new\0"
    "xkb_compose_state_unref\0"
    "xkb_compose_state_feed\0"
    "xkb_compose_state_get_status\0"
    "xkb_compose_state_get_utf8\0"
    "xkb_compose_state_reset\0";
static_assert(shared_library_name_count(kSymbols) * sizeof(void*) == sizeof(XkbApi), "one symbol per XkbApi member");

} // namespace

const XkbApi* xkb_api() noexcept {
    if (g_tried)
        return g_loaded ? &g_api : nullptr;
    g_tried = true;
    if (!shared_library_open(&g_library, kSoname)) {
        log_write(LogLevel::warning, "wayland", "libxkbcommon.so.0 is not installed");
        return nullptr;
    }
    // Resolved in place: g_api is read only once g_loaded says it is complete.
    if (const char* missing = shared_library_resolve(g_library, &g_api, sizeof(g_api), kSymbols)) {
        shared_library_log_missing("wayland", kSoname, missing);
        shared_library_close(&g_library);
        return nullptr;
    }
    g_loaded = true;
    return &g_api;
}

Key xkb_key_for(const XkbApi& api, XkbKeymap* keymap, XkbState* state, u32 evdev, u32* active_symbol) noexcept {
    const u32 code = evdev + kEvdevToXkb;
    const u32 active = api.state_key_get_one_sym(state, code);
    const u32 layout = api.state_key_get_layout(state, code);
    u32 level0 = 0;
    u32 level1 = 0;
    const u32* symbols = nullptr;
    if (api.keymap_key_get_syms_by_level(keymap, code, layout, 0, &symbols) > 0 && symbols)
        level0 = symbols[0];
    symbols = nullptr;
    if (api.keymap_key_get_syms_by_level(keymap, code, layout, 1, &symbols) > 0 && symbols)
        level1 = symbols[0];
    if (active_symbol)
        *active_symbol = active;
    return key_from_keysyms(evdev, active, level0, level1);
}

} // namespace maicord::wayland
