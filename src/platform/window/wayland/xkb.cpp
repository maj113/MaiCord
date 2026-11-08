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

} // namespace

const XkbApi* xkb_api() noexcept {
    if (g_tried)
        return g_loaded ? &g_api : nullptr;
    g_tried = true;
    if (!shared_library_open(&g_library, "libxkbcommon.so.0")) {
        log_write(LogLevel::warning, "wayland", "libxkbcommon.so.0 is not installed");
        return nullptr;
    }
    SharedLibraryResolver resolve{g_library, nullptr};
    XkbApi api{};
    resolve(&api.context_new, "xkb_context_new");
    resolve(&api.context_unref, "xkb_context_unref");
    resolve(&api.context_set_log_level, "xkb_context_set_log_level");
    resolve(&api.keymap_new_from_buffer, "xkb_keymap_new_from_buffer");
    resolve(&api.keymap_new_from_names, "xkb_keymap_new_from_names");
    resolve(&api.keymap_unref, "xkb_keymap_unref");
    resolve(&api.keymap_get_as_string, "xkb_keymap_get_as_string");
    resolve(&api.keymap_key_repeats, "xkb_keymap_key_repeats");
    resolve(&api.keymap_key_get_syms_by_level, "xkb_keymap_key_get_syms_by_level");
    resolve(&api.keymap_mod_get_index, "xkb_keymap_mod_get_index");
    resolve(&api.state_new, "xkb_state_new");
    resolve(&api.state_unref, "xkb_state_unref");
    resolve(&api.state_update_mask, "xkb_state_update_mask");
    resolve(&api.state_update_key, "xkb_state_update_key");
    resolve(&api.state_serialize_mods, "xkb_state_serialize_mods");
    resolve(&api.state_serialize_layout, "xkb_state_serialize_layout");
    resolve(&api.state_key_get_one_sym, "xkb_state_key_get_one_sym");
    resolve(&api.state_key_get_utf32, "xkb_state_key_get_utf32");
    resolve(&api.state_key_get_layout, "xkb_state_key_get_layout");
    resolve(&api.state_mod_name_is_active, "xkb_state_mod_name_is_active");
    resolve(&api.compose_table_new_from_locale, "xkb_compose_table_new_from_locale");
    resolve(&api.compose_table_unref, "xkb_compose_table_unref");
    resolve(&api.compose_state_new, "xkb_compose_state_new");
    resolve(&api.compose_state_unref, "xkb_compose_state_unref");
    resolve(&api.compose_state_feed, "xkb_compose_state_feed");
    resolve(&api.compose_state_get_status, "xkb_compose_state_get_status");
    resolve(&api.compose_state_get_utf8, "xkb_compose_state_get_utf8");
    resolve(&api.compose_state_reset, "xkb_compose_state_reset");
    if (resolve.missing) {
        log_missing_symbol("libxkbcommon.so.0", resolve.missing);
        shared_library_close(&g_library);
        return nullptr;
    }
    g_api = api;
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
