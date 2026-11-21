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

// key_from_fixed_keysym of the keysyms 0xFF00..0xFFFF, one byte a keysym, by the low byte. Of the fixed keysyms only
// Space, ISO_Left_Tab and ISO_Level3_Shift lie outside that page.
struct FixedKeys {
    Key key[256];
};

constexpr FixedKeys kFixedKeys = [] {
    FixedKeys table{};
    for (u32 i = 0; i <= keysym::f24 - keysym::f1; ++i)
        table.key[(keysym::f1 + i) & 0xFF] = static_cast<Key>(key::f1 + i);
    for (u32 i = 0; i <= keysym::kp_9 - keysym::kp_0; ++i)
        table.key[(keysym::kp_0 + i) & 0xFF] = static_cast<Key>(key::numpad0 + i);
    const struct {
        u32 symbol;
        Key key;
    } pairs[] = {
        {keysym::kp_space, key::space},       {keysym::backspace, key::backspace},
        {keysym::tab, key::tab},              {keysym::kp_tab, key::tab},
        {keysym::return_key, key::enter},     {keysym::linefeed, key::enter},
        {keysym::kp_enter, key::enter},       {keysym::pause, key::pause},
        {keysym::scroll_lock, key::scroll_lock}, {keysym::sys_req, key::print_screen},
        {keysym::print, key::print_screen},   {keysym::escape, key::escape},
        {keysym::home, key::home},            {keysym::kp_home, key::home},
        {keysym::left, key::left},            {keysym::kp_left, key::left},
        {keysym::up, key::up},                {keysym::kp_up, key::up},
        {keysym::right, key::right},          {keysym::kp_right, key::right},
        {keysym::down, key::down},            {keysym::kp_down, key::down},
        {keysym::prior, key::page_up},        {keysym::kp_prior, key::page_up},
        {keysym::next, key::page_down},       {keysym::kp_next, key::page_down},
        {keysym::end, key::end},              {keysym::kp_end, key::end},
        {keysym::insert, key::insert},        {keysym::kp_insert, key::insert},
        {keysym::delete_key, key::del},       {keysym::kp_delete, key::del},
        {keysym::menu, key::menu},            {keysym::num_lock, key::num_lock},
        {keysym::caps_lock, key::caps_lock},  {keysym::shift_lock, key::caps_lock},
        {keysym::kp_multiply, key::multiply}, {keysym::kp_add, key::add},
        {keysym::kp_subtract, key::subtract}, {keysym::kp_decimal, key::decimal},
        {keysym::kp_separator, key::decimal}, {keysym::kp_divide, key::divide},
        {keysym::shift_l, key::shift},        {keysym::shift_r, key::shift},
        {keysym::control_l, key::ctrl},       {keysym::control_r, key::ctrl},
        {keysym::alt_l, key::alt},            {keysym::alt_r, key::alt},
        {keysym::meta_l, key::alt},           {keysym::meta_r, key::alt},
        {keysym::super_l, key::left_super},   {keysym::super_r, key::right_super},
    };
    for (const auto& pair : pairs)
        table.key[pair.symbol & 0xFF] = pair.key;
    return table;
}();

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
    if ((symbol >> 8) == 0xFF)
        return kFixedKeys.key[symbol & 0xFF];
    switch (symbol) {
    case keysym::space:
        return key::space;
    case keysym::iso_left_tab:
        return key::tab;
    case keysym::iso_level3_shift:
        return key::right_alt;
    default:
        return key::none;
    }
}

Key key_from_levels(u32 evdev, u32 level0, u32 level1) noexcept {
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
