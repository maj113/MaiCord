#pragma once

#include "maicord/platform/window/key.hpp"

// The pure part of the Wayland keyboard (Linux): xkb keysyms and evdev codes to the portable key::* codes (the
// Windows virtual-key numbering, D14), and the filter that decides which characters reach the app. No libxkbcommon
// and no connection are involved, so the rules are testable with plain numbers (tests/platform/window/wayland_input.cpp).

namespace maicord::wayland {

// <xkbcommon/xkbcommon-keysyms.h>: the keysyms the rules name.
namespace keysym {
inline constexpr u32 space = 0x0020;
inline constexpr u32 apostrophe = 0x0027;
inline constexpr u32 comma = 0x002C;
inline constexpr u32 minus = 0x002D;
inline constexpr u32 period = 0x002E;
inline constexpr u32 slash = 0x002F;
inline constexpr u32 semicolon = 0x003B;
inline constexpr u32 equal = 0x003D;
inline constexpr u32 bracketleft = 0x005B;
inline constexpr u32 backslash = 0x005C;
inline constexpr u32 bracketright = 0x005D;
inline constexpr u32 grave = 0x0060;
inline constexpr u32 iso_level3_shift = 0xFE03;
inline constexpr u32 iso_left_tab = 0xFE20;
inline constexpr u32 backspace = 0xFF08;
inline constexpr u32 tab = 0xFF09;
inline constexpr u32 linefeed = 0xFF0A;
inline constexpr u32 return_key = 0xFF0D;
inline constexpr u32 pause = 0xFF13;
inline constexpr u32 scroll_lock = 0xFF14;
inline constexpr u32 sys_req = 0xFF15;
inline constexpr u32 escape = 0xFF1B;
inline constexpr u32 home = 0xFF50;
inline constexpr u32 left = 0xFF51;
inline constexpr u32 up = 0xFF52;
inline constexpr u32 right = 0xFF53;
inline constexpr u32 down = 0xFF54;
inline constexpr u32 prior = 0xFF55;
inline constexpr u32 next = 0xFF56;
inline constexpr u32 end = 0xFF57;
inline constexpr u32 begin = 0xFF58;
inline constexpr u32 print = 0xFF61;
inline constexpr u32 insert = 0xFF63;
inline constexpr u32 menu = 0xFF67;
inline constexpr u32 num_lock = 0xFF7F;
inline constexpr u32 kp_space = 0xFF80;
inline constexpr u32 kp_tab = 0xFF89;
inline constexpr u32 kp_enter = 0xFF8D;
inline constexpr u32 kp_home = 0xFF95;
inline constexpr u32 kp_left = 0xFF96;
inline constexpr u32 kp_up = 0xFF97;
inline constexpr u32 kp_right = 0xFF98;
inline constexpr u32 kp_down = 0xFF99;
inline constexpr u32 kp_prior = 0xFF9A;
inline constexpr u32 kp_next = 0xFF9B;
inline constexpr u32 kp_end = 0xFF9C;
inline constexpr u32 kp_insert = 0xFF9E;
inline constexpr u32 kp_delete = 0xFF9F;
inline constexpr u32 kp_multiply = 0xFFAA;
inline constexpr u32 kp_add = 0xFFAB;
inline constexpr u32 kp_separator = 0xFFAC;
inline constexpr u32 kp_subtract = 0xFFAD;
inline constexpr u32 kp_decimal = 0xFFAE;
inline constexpr u32 kp_divide = 0xFFAF;
inline constexpr u32 kp_0 = 0xFFB0;
inline constexpr u32 kp_9 = 0xFFB9;
inline constexpr u32 f1 = 0xFFBE;
inline constexpr u32 f24 = 0xFFD5;
inline constexpr u32 shift_l = 0xFFE1;
inline constexpr u32 shift_r = 0xFFE2;
inline constexpr u32 control_l = 0xFFE3;
inline constexpr u32 control_r = 0xFFE4;
inline constexpr u32 caps_lock = 0xFFE5;
inline constexpr u32 shift_lock = 0xFFE6;
inline constexpr u32 meta_l = 0xFFE7;
inline constexpr u32 meta_r = 0xFFE8;
inline constexpr u32 alt_l = 0xFFE9;
inline constexpr u32 alt_r = 0xFFEA;
inline constexpr u32 super_l = 0xFFEB;
inline constexpr u32 super_r = 0xFFEC;
inline constexpr u32 delete_key = 0xFFFF;
} // namespace keysym

// The US-QWERTY virtual key of a physical key (a Linux evdev code, <linux/input-event-codes.h>); key::none for
// codes the portable set has no key for. What non-Latin layouts fall back to, as on Windows.
Key key_from_position(u32 evdev) noexcept;

// The key a keysym names regardless of layout: modifiers (the generic shift, ctrl and alt), editing and navigation
// keys, function keys, the keypad (as the active keysym says: KP_1 with Num Lock on, KP_End with it off). key::none
// for a keysym that is not one of them.
Key key_from_fixed_keysym(u32 keysym) noexcept;

// The virtual key of a key press under the active layout. `active` is the keysym the key produces now, `level0` and
// `level1` its unshifted and shifted keysyms in the active layout group (0 when it has none). Rules, in order:
//   1. a fixed keysym (key_from_fixed_keysym);
//   2. level 0 a Latin letter: 'A'..'Z' (the VK follows the label, so German Z and Dvorak O behave as on Windows);
//   3. level 0 an ASCII digit, else level 1 one (AZERTY): '0'..'9';
//   4. level 0 a US punctuation label: that key (semicolon, equals, comma, ...);
//   5. otherwise the physical position (Cyrillic, Greek, Arabic, JP, accented keys): key_from_position.
Key key_from_keysyms(u32 evdev, u32 active, u32 level0, u32 level1) noexcept;

// The side-specific code of a modifier press, from the evdev code: Left Shift, Right Ctrl, ... (the generic code
// goes in the event; the specific one feeds window_key_down(key::left_shift)). key::none for other keys.
Key specific_modifier_key(u32 evdev) noexcept;

// Whether Windows would deliver this code point as WM_CHAR: printable text, the editing controls (\b \t \n \r and
// Escape) and the Ctrl+letter codes U+0001..U+001A; never U+007F (Delete), the C1 controls, surrogates or values
// beyond Unicode.
bool is_message_character(u32 code_point) noexcept;

// UTF-16 code units of a code point: 1 or 2 (a surrogate pair); 0 for a value that is not a Unicode scalar.
u32 utf16_units(u32 code_point, u16 (&units)[2]) noexcept;

} // namespace maicord::wayland
