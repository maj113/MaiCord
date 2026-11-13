// Key state on Windows: what the calling thread's input state says is down (GetKeyState, as of the message being
// handled), and the physical keys of the whole desktop (GetAsyncKeyState) for GlobalKeys.
#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window/global_keys.hpp"
#include "maicord/platform/window/window.hpp"

namespace maicord {

bool window_key_down(Key key_code) noexcept {
    return (GetKeyState(key_code) & 0x8000) != 0;
}

bool window_toggle_state(Key key_code) noexcept {
    return (GetKeyState(key_code) & 1) != 0;
}

u8 window_modifiers() noexcept {
    u32 modifiers = 0;
    if (GetKeyState(key::ctrl) & 0x8000)
        modifiers |= mod_ctrl;
    if (GetKeyState(key::alt) & 0x8000)
        modifiers |= mod_alt;
    if (GetKeyState(key::shift) & 0x8000)
        modifiers |= mod_shift;
    if ((GetKeyState(key::left_super) & 0x8000) || (GetKeyState(key::right_super) & 0x8000))
        modifiers |= mod_super;
    // AltGr is the right Alt together with the Ctrl the system synthesises for it. ctrl and alt stay set: the
    // physical keys, as Windows reports them.
    if ((GetKeyState(key::right_alt) & 0x8000) && (GetKeyState(key::left_ctrl) & 0x8000))
        modifiers |= mod_altgr;
    if (GetKeyState(key::caps_lock) & 1)
        modifiers |= mod_caps;
    if (GetKeyState(key::num_lock) & 1)
        modifiers |= mod_num;
    return static_cast<u8>(modifiers);
}

bool window_layout_types_text(Key key_code, u8 modifiers) noexcept {
    BYTE state[256]{};
    if (modifiers & mod_ctrl)
        state[key::ctrl] = state[key::left_ctrl] = 0x80;
    if (modifiers & (mod_alt | mod_altgr))
        state[key::alt] = state[key::right_alt] = 0x80;
    if (modifiers & mod_shift)
        state[key::shift] = 0x80;
    wchar_t output[4];
    // Flag 4: leave the keyboard's dead-key state untouched.
    const int produced = ToUnicodeEx(key_code, MapVirtualKeyW(key_code, MAPVK_VK_TO_VSC), state, output, 4, 4,
                                     GetKeyboardLayout(0));
    return produced < 0 || (produced > 0 && output[0] >= 0x20);
}

bool global_key_down(Key key_code) noexcept {
    return (GetAsyncKeyState(key_code) & 0x8000) != 0;
}

} // namespace maicord
