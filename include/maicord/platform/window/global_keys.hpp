#pragma once

#include "maicord/platform/window/key.hpp"

// Key state read without a message queue: push-to-talk on the capture thread and the chords that fire while the
// window is in the background. Windows polls GetAsyncKeyState. Wayland has nothing like it: the Linux backend keeps
// the keys the window itself sees go down and up while it has keyboard focus (cleared when focus leaves), so
// push-to-talk works while the window is focused; the GlobalShortcuts portal widens that later.
//
// Any thread. `key` is a Key as in key.hpp, a mouse button code excluded.
namespace maicord {

bool global_key_down(Key key) noexcept;

} // namespace maicord
