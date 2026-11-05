#pragma once

#include "maicord/core/base.hpp"
#include "maicord/platform/window/event.hpp"
#include "maicord/platform/window/frame.hpp"
#include "maicord/ui/shell/caption_bar.hpp"

namespace maicord::app_detail {

// The caption controller of the main window: the bar the app draws over the rows above the content (the frame of a
// window whose client area extends over the system caption). It knows the bar's hover, press and tooltip, its
// layout and its commands, and answers the frame questions of the platform: where a pixel lies (a hit_test event, or
// the Windows frame glue through application_caption_hit and the calls below). The renderer's content starts
// application_caption_height() pixels below the client origin. Every offset is zero until
// application_caption_enable, so other windows and test fixtures keep client coordinates.

// Before the window exists: the creation-time frame messages already apply it.
void application_caption_enable() noexcept;
// Every window event, first: pointer input over the bar's own items (update badge, Settings, the window buttons the
// platform does not draw as frame), the scale and size changes the bar follows. True when the bar took the event.
bool application_caption_event(const WindowEvent& event) noexcept;
// Pixels between the client origin and the content.
u32 application_caption_height() noexcept;
// The bar's height at a DPI, zero without a shown bar.
u32 application_caption_pixels(u32 dpi) noexcept;
// Whether fullscreen hides the bar.
bool application_caption_hidden() noexcept;
// Per frame: follows theme and DPI changes of the height and publishes the
// caption state and title to the renderer.
void application_caption_synchronize() noexcept;
// Fullscreen hides the caption; leaving it restores it.
void application_caption_set_hidden(bool hidden) noexcept;
// Minimize, maximize/restore or close, as the caption buttons do.
bool application_caption_command(UiCaptionPart part) noexcept;
// The bar state; the hovered part's tooltip shows once its delay passed.
UiCaptionBarState application_caption_state() noexcept;
// When the hovered part's tooltip is due, zero when none is pending.
u64 application_caption_tooltip_wake() noexcept;
// Caption buttons in content DIPs (negative rows), for accessibility.
UiCaptionBarLayout application_caption_layout() noexcept;

// The frame questions a platform asks (client pixels): where a pixel lies, and the frame state it reports.
FrameHit application_caption_hit(float x, float y) noexcept;
void application_caption_maximized(bool maximized) noexcept;
void application_caption_active(bool active) noexcept;
// A window button the platform tracks as frame (Windows' maximize button): the pointer over a part (FrameHit::client:
// none), gone from the frame, a part pressed, a part released (true when that ended a press of the bar's).
void application_caption_hover(FrameHit part) noexcept;
void application_caption_leave() noexcept;
void application_caption_press(FrameHit part) noexcept;
bool application_caption_release(FrameHit part) noexcept;
// The bar in client pixels, for a platform that tells the system where the buttons are; false without a bar.
bool application_caption_bar_rects(FrameBarRects* rectangles) noexcept;

} // namespace maicord::app_detail
