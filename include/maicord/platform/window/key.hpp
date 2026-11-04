#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// A portable key code. The numbering is the Windows virtual-key numbering (0 = none): custom keybinds, the
// push-to-talk key and the plugin SDK persist these values, so every platform translates to them. Letters and
// digits are their upper-case ASCII codes ('A'..'Z', '0'..'9'). The Windows build checks every constant
// against <Windows.h> (src/platform/window/win32/key_conformance.cpp).
using Key = u8;

namespace key {
inline constexpr Key none = 0x00;

// Mouse buttons, where an API names a button as a key.
inline constexpr Key lbutton = 0x01;
inline constexpr Key rbutton = 0x02;
inline constexpr Key mbutton = 0x04;
inline constexpr Key xbutton1 = 0x05;
inline constexpr Key xbutton2 = 0x06;

inline constexpr Key backspace = 0x08;
inline constexpr Key tab = 0x09;
inline constexpr Key enter = 0x0D;
inline constexpr Key shift = 0x10;
inline constexpr Key ctrl = 0x11;
inline constexpr Key alt = 0x12;
inline constexpr Key pause = 0x13;
inline constexpr Key caps_lock = 0x14;
inline constexpr Key escape = 0x1B;
inline constexpr Key space = 0x20;
inline constexpr Key page_up = 0x21;
inline constexpr Key page_down = 0x22;
inline constexpr Key end = 0x23;
inline constexpr Key home = 0x24;
inline constexpr Key left = 0x25;
inline constexpr Key up = 0x26;
inline constexpr Key right = 0x27;
inline constexpr Key down = 0x28;
inline constexpr Key print_screen = 0x2C;
inline constexpr Key insert = 0x2D;
inline constexpr Key del = 0x2E;

inline constexpr Key left_super = 0x5B;
inline constexpr Key right_super = 0x5C;
inline constexpr Key menu = 0x5D;

inline constexpr Key numpad0 = 0x60;
inline constexpr Key numpad1 = 0x61;
inline constexpr Key numpad2 = 0x62;
inline constexpr Key numpad3 = 0x63;
inline constexpr Key numpad4 = 0x64;
inline constexpr Key numpad5 = 0x65;
inline constexpr Key numpad6 = 0x66;
inline constexpr Key numpad7 = 0x67;
inline constexpr Key numpad8 = 0x68;
inline constexpr Key numpad9 = 0x69;
inline constexpr Key multiply = 0x6A;
inline constexpr Key add = 0x6B;
inline constexpr Key subtract = 0x6D;
inline constexpr Key decimal = 0x6E;
inline constexpr Key divide = 0x6F;

inline constexpr Key f1 = 0x70;
inline constexpr Key f2 = 0x71;
inline constexpr Key f3 = 0x72;
inline constexpr Key f4 = 0x73;
inline constexpr Key f5 = 0x74;
inline constexpr Key f6 = 0x75;
inline constexpr Key f7 = 0x76;
inline constexpr Key f8 = 0x77;
inline constexpr Key f9 = 0x78;
inline constexpr Key f10 = 0x79;
inline constexpr Key f11 = 0x7A;
inline constexpr Key f12 = 0x7B;
inline constexpr Key f13 = 0x7C;
inline constexpr Key f14 = 0x7D;
inline constexpr Key f15 = 0x7E;
inline constexpr Key f16 = 0x7F;
inline constexpr Key f17 = 0x80;
inline constexpr Key f18 = 0x81;
inline constexpr Key f19 = 0x82;
inline constexpr Key f20 = 0x83;
inline constexpr Key f21 = 0x84;
inline constexpr Key f22 = 0x85;
inline constexpr Key f23 = 0x86;
inline constexpr Key f24 = 0x87;

inline constexpr Key num_lock = 0x90;
inline constexpr Key scroll_lock = 0x91;

inline constexpr Key left_shift = 0xA0;
inline constexpr Key right_shift = 0xA1;
inline constexpr Key left_ctrl = 0xA2;
inline constexpr Key right_ctrl = 0xA3;
inline constexpr Key left_alt = 0xA4;
inline constexpr Key right_alt = 0xA5;

// Punctuation keys, named by their US-layout label.
inline constexpr Key semicolon = 0xBA;
inline constexpr Key equals = 0xBB;
inline constexpr Key comma = 0xBC;
inline constexpr Key minus = 0xBD;
inline constexpr Key period = 0xBE;
inline constexpr Key slash = 0xBF;
inline constexpr Key grave = 0xC0;
inline constexpr Key left_bracket = 0xDB;
inline constexpr Key backslash = 0xDC;
inline constexpr Key right_bracket = 0xDD;
inline constexpr Key quote = 0xDE;
} // namespace key

// Modifier bits. The low three are the ShortcutModifier values, so a chord's modifiers are
// event.modifiers & (mod_ctrl | mod_alt | mod_shift).
enum Modifier : u8 {
    mod_ctrl = 1,
    mod_alt = 2,
    mod_shift = 4,
    mod_super = 8,
    mod_altgr = 16,
    mod_caps = 32,
    mod_num = 64,
};

enum class PointerButton : u8 { left, right, middle, back, forward };

} // namespace maicord
