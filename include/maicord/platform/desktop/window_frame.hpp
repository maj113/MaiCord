#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"

// Geometry of a top-level window whose client area extends over the system
// caption (WM_NCCALCSIZE keeps the DWM side and bottom frame for resizing,
// the shadow, rounded corners and window animations). Pixels throughout.

namespace maicord {

enum WindowFrameEdge : u8 {
    window_frame_edge_left = 1u << 0,
    window_frame_edge_top = 1u << 1,
    window_frame_edge_right = 1u << 2,
    window_frame_edge_bottom = 1u << 3,
};

// Client pixels left uncovered at a monitor edge holding an auto-hide
// appbar, so the shell does not treat the maximized window as fullscreen
// and the taskbar can still be revealed.
inline constexpr i32 kWindowFrameAutohideReserve = 2;

// Resize border thickness (SM_CXFRAME + SM_CXPADDEDBORDER) at a DPI.
i32 window_frame_border(u32 dpi) noexcept;

// WM_NCCALCSIZE: the client rectangle for the proposed window rectangle.
// A maximized window overhangs its monitor by the border on every side; its
// client is clamped to the work area, less the auto-hide reserve.
RECT window_frame_client(const RECT& window, i32 border, bool maximized, const RECT& work_area,
                         u8 autohide_edges) noexcept;


} // namespace maicord
