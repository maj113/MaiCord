#pragma once
// Windows opened by benchmarks and probes (MaiCordPerfProbe, MaiCordUiProbe and
// the other window-backed UI probes) share the desktop with whoever is using
// it. They never activate or take keyboard focus, have no taskbar button and
// open at the bottom of the z-order, so every other window stays above them.
// Such a window is still shown (neither minimized nor cloaked): DWM composes
// its swap chain, Present and the frame-latency waitable pace as for a
// foreground window, and the GPU does the same work, except while the display
// sleeps: DWM then composes at ~4 Hz. See tools/perf/README.md.
#include <windows.h>

namespace maicord::probe_window {

// Extended styles for a probe window; size its frame with them
// (AdjustWindowRectEx) so the client area stays the requested size.
inline constexpr DWORD kExtendedStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

// Shows the window without activating it, behind every other window. Use it
// instead of ShowWindow, which raises the window (and activates it unless
// told not to).
inline bool show(HWND window) noexcept {
    return window && SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0,
                                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                                      SWP_SHOWWINDOW) != FALSE;
}

} // namespace maicord::probe_window
