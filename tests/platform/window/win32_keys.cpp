#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window/global_keys.hpp"
#include "maicord/platform/window/window.hpp"
#include "../../support/test_output.hpp"

// The Windows key-state service (src/platform/window/win32/keys.cpp) against the thread's synthetic keyboard state:
// window_key_down, window_toggle_state and window_modifiers read what GetKeyState reads, as of the message being
// handled; global_key_down reads the physical keys.
namespace maicord {
namespace {

u32 failures;

void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}

bool apply(const BYTE (&keys)[256]) noexcept {
    return SetKeyboardState(const_cast<BYTE*>(keys)) != FALSE;
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_window_win32_keys_entry) {
    using namespace maicord;
    BYTE saved[256]{}, keys[256]{};
    const bool have_saved = GetKeyboardState(saved) != FALSE;
    check(have_saved, "win32 keys: thread keyboard state is saved before synthetic input");
    if (have_saved) {
        check(apply(keys) && window_modifiers(nullptr) == 0 && !window_key_down(nullptr, key::ctrl) &&
                  !window_key_down(nullptr, key::lbutton) && !window_toggle_state(nullptr, key::caps_lock),
              "win32 keys: a clean state reads as nothing held");

        keys[key::ctrl] = keys[key::shift] = 0x80;
        check(apply(keys) && window_key_down(nullptr, key::ctrl) && window_key_down(nullptr, key::shift) &&
                  !window_key_down(nullptr, key::alt) && window_modifiers(nullptr) == (mod_ctrl | mod_shift),
              "win32 keys: control and shift are the first and third modifier bits");

        reset_value(&keys);
        keys[key::alt] = 0x80;
        check(apply(keys) && window_modifiers(nullptr) == mod_alt, "win32 keys: alt alone is mod_alt without altgr");

        // AltGr is the right Alt plus the Ctrl Windows adds: ctrl and alt stay set, altgr marks the pair.
        reset_value(&keys);
        keys[key::ctrl] = keys[key::left_ctrl] = keys[key::alt] = keys[key::right_alt] = 0x80;
        check(apply(keys) && window_modifiers(nullptr) == (mod_ctrl | mod_alt | mod_altgr),
              "win32 keys: right alt with the synthetic left control reads as altgr and keeps ctrl and alt");

        reset_value(&keys);
        keys[key::left_super] = 0x80;
        check(apply(keys) && window_modifiers(nullptr) == mod_super, "win32 keys: either Windows key is mod_super");

        // The toggle bit is the low bit of the key's state; the down bit is the high one.
        reset_value(&keys);
        keys[key::caps_lock] = keys[key::num_lock] = 0x01;
        check(apply(keys) && window_toggle_state(nullptr, key::caps_lock) && !window_key_down(nullptr, key::caps_lock) &&
                  window_modifiers(nullptr) == (mod_caps | mod_num),
              "win32 keys: lock keys read through their toggle bit");

        reset_value(&keys);
        keys[key::lbutton] = 0x80;
        check(apply(keys) && window_key_down(nullptr, key::lbutton) && !window_key_down(nullptr, key::rbutton),
              "win32 keys: mouse buttons are keys");

        // Keys that type nothing never read as text, with or without modifiers.
        check(!window_layout_types_text(nullptr, key::f1, mod_ctrl | mod_alt) &&
                  !window_layout_types_text(nullptr, key::escape, 0),
              "win32 keys: function and escape keys type no text");

        check(!global_key_down(0x07), "win32 keys: an unassigned virtual key is never down");

        check(apply(saved) && GetKeyboardState(keys) != FALSE && bytes_equal(saved, keys, sizeof(saved)),
              "win32 keys: the original thread keyboard state is restored");
    }
    test_exit(failures, "window win32 keys");
}
