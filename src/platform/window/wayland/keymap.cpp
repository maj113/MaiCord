#include "maicord/platform/window/wayland/keymap.hpp"

namespace maicord::wayland {
namespace {

// <linux/input-event-codes.h> keys by name where the table below reads better with them.
constexpr u32 kKey102nd = 86;
constexpr u32 kKeyRo = 89;
constexpr u32 kKeyYen = 124;

constexpr Key kPositionTable[] = {
    /*   0 */ key::none,
    /*   1 */ key::escape,
    /*   2 */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    /*  12 */ key::minus,
    /*  13 */ key::equals,
    /*  14 */ key::backspace,
    /*  15 */ key::tab,
    /*  16 */ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
    /*  26 */ key::left_bracket,
    /*  27 */ key::right_bracket,
    /*  28 */ key::enter,
    /*  29 */ key::ctrl,
    /*  30 */ 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L',
    /*  39 */ key::semicolon,
    /*  40 */ key::quote,
    /*  41 */ key::grave,
    /*  42 */ key::shift,
    /*  43 */ key::backslash,
    /*  44 */ 'Z', 'X', 'C', 'V', 'B', 'N', 'M',
    /*  51 */ key::comma,
    /*  52 */ key::period,
    /*  53 */ key::slash,
    /*  54 */ key::shift,
    /*  55 */ key::multiply,
    /*  56 */ key::alt,
    /*  57 */ key::space,
    /*  58 */ key::caps_lock,
    /*  59 */ key::f1, key::f2, key::f3, key::f4, key::f5, key::f6, key::f7, key::f8, key::f9, key::f10,
    /*  69 */ key::num_lock,
    /*  70 */ key::scroll_lock,
    /*  71 */ key::numpad7, key::numpad8, key::numpad9,
    /*  74 */ key::subtract,
    /*  75 */ key::numpad4, key::numpad5, key::numpad6,
    /*  78 */ key::add,
    /*  79 */ key::numpad1, key::numpad2, key::numpad3,
    /*  82 */ key::numpad0,
    /*  83 */ key::decimal,
};

constexpr u32 kPositionTableCount = sizeof(kPositionTable) / sizeof(kPositionTable[0]);
static_assert(kPositionTableCount == 84);

bool is_latin_letter(u32 symbol) noexcept {
    return (symbol >= 'a' && symbol <= 'z') || (symbol >= 'A' && symbol <= 'Z');
}

bool is_digit(u32 symbol) noexcept {
    return symbol >= '0' && symbol <= '9';
}

Key punctuation_key(u32 symbol) noexcept {
    switch (symbol) {
    case keysym::semicolon:
        return key::semicolon;
    case keysym::equal:
        return key::equals;
    case keysym::comma:
        return key::comma;
    case keysym::minus:
        return key::minus;
    case keysym::period:
        return key::period;
    case keysym::slash:
        return key::slash;
    case keysym::grave:
        return key::grave;
    case keysym::bracketleft:
        return key::left_bracket;
    case keysym::backslash:
        return key::backslash;
    case keysym::bracketright:
        return key::right_bracket;
    case keysym::apostrophe:
        return key::quote;
    default:
        return key::none;
    }
}

} // namespace

Key key_from_position(u32 evdev) noexcept {
    if (evdev < kPositionTableCount)
        return kPositionTable[evdev];
    switch (evdev) {
    case kKey102nd:
    case kKeyRo:
        return 0xE2; // VK_OEM_102: the extra key of ISO and JIS boards
    case kKeyYen:
        return key::backslash;
    case 87:
        return key::f11;
    case 88:
        return key::f12;
    case 96:
        return key::enter; // keypad Enter
    case 97:
        return key::ctrl;
    case 98:
        return key::divide;
    case 99:
        return key::print_screen;
    case 100:
        return key::alt;
    case 102:
        return key::home;
    case 103:
        return key::up;
    case 104:
        return key::page_up;
    case 105:
        return key::left;
    case 106:
        return key::right;
    case 107:
        return key::end;
    case 108:
        return key::down;
    case 109:
        return key::page_down;
    case 110:
        return key::insert;
    case 111:
        return key::del;
    case 119:
        return key::pause;
    case 125:
        return key::left_super;
    case 126:
        return key::right_super;
    case 127:
        return key::menu;
    default:
        break;
    }
    if (evdev >= 183 && evdev <= 194)
        return static_cast<Key>(key::f13 + (evdev - 183)); // KEY_F13..KEY_F24
    return key::none;
}

Key key_from_fixed_keysym(u32 symbol) noexcept {
    if (symbol >= keysym::f1 && symbol <= keysym::f24)
        return static_cast<Key>(key::f1 + (symbol - keysym::f1));
    if (symbol >= keysym::kp_0 && symbol <= keysym::kp_9)
        return static_cast<Key>(key::numpad0 + (symbol - keysym::kp_0));
    switch (symbol) {
    case keysym::space:
    case keysym::kp_space:
        return key::space;
    case keysym::backspace:
        return key::backspace;
    case keysym::tab:
    case keysym::kp_tab:
    case keysym::iso_left_tab:
        return key::tab;
    case keysym::return_key:
    case keysym::linefeed:
    case keysym::kp_enter:
        return key::enter;
    case keysym::pause:
        return key::pause;
    case keysym::scroll_lock:
        return key::scroll_lock;
    case keysym::sys_req:
    case keysym::print:
        return key::print_screen;
    case keysym::escape:
        return key::escape;
    case keysym::home:
    case keysym::kp_home:
        return key::home;
    case keysym::left:
    case keysym::kp_left:
        return key::left;
    case keysym::up:
    case keysym::kp_up:
        return key::up;
    case keysym::right:
    case keysym::kp_right:
        return key::right;
    case keysym::down:
    case keysym::kp_down:
        return key::down;
    case keysym::prior:
    case keysym::kp_prior:
        return key::page_up;
    case keysym::next:
    case keysym::kp_next:
        return key::page_down;
    case keysym::end:
    case keysym::kp_end:
        return key::end;
    case keysym::insert:
    case keysym::kp_insert:
        return key::insert;
    case keysym::delete_key:
    case keysym::kp_delete:
        return key::del;
    case keysym::menu:
        return key::menu;
    case keysym::num_lock:
        return key::num_lock;
    case keysym::caps_lock:
    case keysym::shift_lock:
        return key::caps_lock;
    case keysym::kp_multiply:
        return key::multiply;
    case keysym::kp_add:
        return key::add;
    case keysym::kp_subtract:
        return key::subtract;
    case keysym::kp_decimal:
    case keysym::kp_separator:
        return key::decimal;
    case keysym::kp_divide:
        return key::divide;
    case keysym::shift_l:
    case keysym::shift_r:
        return key::shift;
    case keysym::control_l:
    case keysym::control_r:
        return key::ctrl;
    case keysym::alt_l:
    case keysym::alt_r:
    case keysym::meta_l:
    case keysym::meta_r:
        return key::alt;
    case keysym::iso_level3_shift:
        return key::right_alt;
    case keysym::super_l:
        return key::left_super;
    case keysym::super_r:
        return key::right_super;
    default:
        return key::none;
    }
}

Key key_from_keysyms(u32 evdev, u32 active, u32 level0, u32 level1) noexcept {
    if (const Key fixed = key_from_fixed_keysym(active))
        return fixed;
    if (is_latin_letter(level0))
        return static_cast<Key>(level0 & ~0x20u); // upper case
    if (is_digit(level0))
        return static_cast<Key>(level0);
    if (is_digit(level1))
        return static_cast<Key>(level1);
    if (const Key punctuation = punctuation_key(level0))
        return punctuation;
    return key_from_position(evdev);
}

Key specific_modifier_key(u32 evdev) noexcept {
    switch (evdev) {
    case 42:
        return key::left_shift;
    case 54:
        return key::right_shift;
    case 29:
        return key::left_ctrl;
    case 97:
        return key::right_ctrl;
    case 56:
        return key::left_alt;
    case 100:
        return key::right_alt;
    default:
        return key::none;
    }
}

bool is_message_character(u32 code_point) noexcept {
    if (code_point == 0 || code_point == 0x7F)
        return false;
    if (code_point < 0x7F)
        return true; // controls below U+0020 included: \b \t \n \r, Escape and the Ctrl+letter codes
    if (code_point < 0xA0)
        return false; // C1 controls
    if (code_point >= 0xD800 && code_point <= 0xDFFF)
        return false;
    return code_point <= 0x10FFFF;
}

u32 utf16_units(u32 code_point, u16 (&units)[2]) noexcept {
    if (code_point >= 0xD800 && code_point <= 0xDFFF)
        return 0;
    if (code_point < 0x10000) {
        units[0] = static_cast<u16>(code_point);
        return 1;
    }
    if (code_point > 0x10FFFF)
        return 0;
    const u32 offset = code_point - 0x10000;
    units[0] = static_cast<u16>(0xD800 + (offset >> 10));
    units[1] = static_cast<u16>(0xDC00 + (offset & 0x3FF));
    return 2;
}

} // namespace maicord::wayland
