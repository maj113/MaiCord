#include "maicord/platform/window/event.hpp"
#include "maicord/platform/window/key.hpp"
#include "../../support/test_output.hpp"

// The window vocabulary compiles on every platform. Windows additionally checks each key constant against
// <Windows.h> (src/platform/window/win32/key_conformance.cpp).
namespace maicord {
namespace {

// The portable key numbering is the Windows virtual-key numbering that saved keybinds persist.
static_assert(key::enter == 0x0D && key::escape == 0x1B && key::space == 0x20 && key::tab == 0x09);
static_assert(key::left == 0x25 && key::up == 0x26 && key::right == 0x27 && key::down == 0x28);
static_assert(key::f1 == 0x70 && key::f24 == 0x87 && key::numpad0 == 0x60 && key::numpad9 == 0x69);
static_assert(key::shift == 0x10 && key::ctrl == 0x11 && key::alt == 0x12);
static_assert(key::lbutton == 0x01 && key::mbutton == 0x04 && key::xbutton2 == 0x06);

// A chord's modifiers are the low bits of an event's, so the two share one numbering.
static_assert(mod_ctrl == 1 && mod_alt == 2 && mod_shift == 4);

constexpr bool single_bits() noexcept {
    constexpr u32 flags[] = {ev_repeat, ev_double_click, ev_system, ev_synthetic, ev_keyboard, ev_provisional};
    u32 seen = 0;
    for (const u32 flag : flags) {
        if (flag == 0 || (flag & (flag - 1)) != 0 || (seen & flag) != 0)
            return false;
        seen |= flag;
    }
    return seen <= 0xFF;
}
static_assert(single_bits());

static_assert(static_cast<u8>(PointerButton::left) == 0 && static_cast<u8>(PointerButton::forward) == 4);
static_assert(__is_trivially_copyable(WindowEvent) && __is_trivially_copyable(WindowEventResult));

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_window_vocabulary_entry) {
    maicord::WindowEvent event{};
    event.type = maicord::EventType::key_down;
    event.key.key = maicord::key::enter;
    event.modifiers = maicord::mod_ctrl | maicord::mod_shift;
    if (event.type != maicord::EventType::key_down || event.key.key != 0x0D || (event.modifiers & 7) != 5)
        maicord::test_exit(1, "window event fields");
    maicord::test_exit(0, "window vocabulary");
}
