#pragma once

#include "maicord/core/base.hpp"

// The colour picker's eyedropper: a colour picked from anywhere on the screen. UI thread, one pick at a time.
//  - Windows: the next primary click anywhere on the desktop, through a low-level mouse hook on the UI thread; the
//    click is swallowed (it never reaches the window under it), its pixel is read from the screen, and a secondary
//    click ends the pick without a colour. The client keeps the keyboard, so its Escape calls screen_color_pick_end.
//  - Linux: the Screenshot portal's PickColor (the desktop draws its own picker); a desktop without it fails.
// `settled` runs once, on the UI thread, when the pick ends by itself (picked, cancelled or failed); never from
// screen_color_pick_end.

namespace maicord {

enum class ScreenColorState : u8 {
    idle,      // no pick
    picking,   // waiting for the click (or the portal)
    picked,    // the colour is ready
    cancelled, // the user ended it
    failed,    // the desktop cannot pick colours
};

bool screen_color_pick_start(void (*settled)(void* context) noexcept, void* context) noexcept;
// The state; `rgb` (0xRRGGBB) once picked.
ScreenColorState screen_color_pick_state(u32* rgb) noexcept;
// Ends the pick (a pending one without a colour) and returns to idle.
void screen_color_pick_end() noexcept;

} // namespace maicord
