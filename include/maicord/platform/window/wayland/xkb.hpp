#pragma once

#include "maicord/core/base.hpp"
#include "maicord/platform/window/key.hpp"

// The libxkbcommon interface the Wayland backend uses, declared without its headers (Linux). The library is
// dlopen()ed ("libxkbcommon.so.0"); the declarations follow <xkbcommon/xkbcommon.h> and xkbcommon-compose.h.

namespace maicord::wayland {

struct XkbContext;
struct XkbKeymap;
struct XkbState;
struct XkbComposeTable;
struct XkbComposeState;

// xkb_keymap_format, xkb_state_component, xkb_log_level and the compose enums.
inline constexpr int kXkbKeymapFormatTextV1 = 1;
inline constexpr int kXkbKeyUp = 0;
inline constexpr int kXkbKeyDown = 1;
inline constexpr int kXkbStateDepressed = 1 << 0;  // xkb_state_component, for modifiers and layouts alike
inline constexpr int kXkbStateLatched = 1 << 1;
inline constexpr int kXkbStateModsLocked = 1 << 2;
inline constexpr int kXkbStateModsEffective = 1 << 3;
inline constexpr int kXkbLogLevelCritical = 10;
inline constexpr u32 kXkbModifierInvalid = 0xFFFFFFFFu;
inline constexpr int kXkbComposeStatusNothing = 0;
inline constexpr int kXkbComposeStatusComposing = 1;
inline constexpr int kXkbComposeStatusComposed = 2;
inline constexpr int kXkbComposeStatusCancelled = 3;
inline constexpr int kXkbComposeFeedAccepted = 1;

// An evdev keycode plus this is the xkb keycode of the same key.
inline constexpr u32 kEvdevToXkb = 8;

struct XkbRuleNames {
    const char* rules;
    const char* model;
    const char* layout;
    const char* variant;
    const char* options;
};

struct XkbApi {
    XkbContext* (*context_new)(int flags) noexcept;
    void (*context_unref)(XkbContext* context) noexcept;
    void (*context_set_log_level)(XkbContext* context, int level) noexcept;
    XkbKeymap* (*keymap_new_from_buffer)(XkbContext* context, const char* buffer, usize length, int format,
                                         int flags) noexcept;
    XkbKeymap* (*keymap_new_from_names)(XkbContext* context, const XkbRuleNames* names, int flags) noexcept;
    void (*keymap_unref)(XkbKeymap* keymap) noexcept;
    char* (*keymap_get_as_string)(XkbKeymap* keymap, int format) noexcept;  // malloc'd: free() it
    int (*keymap_key_repeats)(XkbKeymap* keymap, u32 key) noexcept;
    int (*keymap_key_get_syms_by_level)(XkbKeymap* keymap, u32 key, u32 layout, u32 level,
                                        const u32** symbols) noexcept;
    u32 (*keymap_num_layouts_for_key)(XkbKeymap* keymap, u32 key) noexcept;
    u32 (*keymap_mod_get_index)(XkbKeymap* keymap, const char* name) noexcept;
    XkbState* (*state_new)(XkbKeymap* keymap) noexcept;
    void (*state_unref)(XkbState* state) noexcept;
    int (*state_update_mask)(XkbState* state, u32 depressed, u32 latched, u32 locked, u32 depressed_layout,
                             u32 latched_layout, u32 locked_layout) noexcept;
    int (*state_update_key)(XkbState* state, u32 key, int direction) noexcept;
    u32 (*state_serialize_mods)(XkbState* state, int components) noexcept;
    u32 (*state_serialize_layout)(XkbState* state, int components) noexcept;
    u32 (*state_key_get_one_sym)(XkbState* state, u32 key) noexcept;
    u32 (*state_key_get_utf32)(XkbState* state, u32 key) noexcept;
    u32 (*state_key_get_layout)(XkbState* state, u32 key) noexcept;
    int (*state_mod_index_is_active)(XkbState* state, u32 index, int type) noexcept;
    int (*state_mod_name_is_active)(XkbState* state, const char* name, int type) noexcept;
    u32 (*keysym_to_utf32)(u32 keysym) noexcept;
    XkbComposeTable* (*compose_table_new_from_locale)(XkbContext* context, const char* locale, int flags) noexcept;
    void (*compose_table_unref)(XkbComposeTable* table) noexcept;
    XkbComposeState* (*compose_state_new)(XkbComposeTable* table, int flags) noexcept;
    void (*compose_state_unref)(XkbComposeState* state) noexcept;
    int (*compose_state_feed)(XkbComposeState* state, u32 keysym) noexcept;
    int (*compose_state_get_status)(XkbComposeState* state) noexcept;
    int (*compose_state_get_utf8)(XkbComposeState* state, char* buffer, usize size) noexcept;
    void (*compose_state_reset)(XkbComposeState* state) noexcept;
};

// Opens libxkbcommon.so.0 (once) and resolves every function; null, with a log line, when it cannot.
const XkbApi* xkb_api() noexcept;

// The portable key (key.hpp) of the evdev key `evdev` under the layout group and level `state` has now, and the
// keysym it produces now in *active_symbol: key_from_keysyms with the keysyms read from the keymap.
Key xkb_key_for(const XkbApi& api, XkbKeymap* keymap, XkbState* state, u32 evdev, u32* active_symbol) noexcept;

} // namespace maicord::wayland
