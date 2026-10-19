#pragma once

#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {

struct UiTextMeasureCache;

// Transient confirmation pill at the top centre of the window ("Link
// Copied!", "Image Saved"). One shows at a time; later ones wait in order.
// It never takes input, focus or a timer of its own: the host ticks it from
// frames and wakes at ui_toast_next_wake, which is zero once the queue drains.
inline constexpr u32 kUiToastTextBytes = 96;
// The current toast plus pending ones; a full queue drops its oldest pending.
inline constexpr u32 kUiToastQueueCapacity = 4;
inline constexpr u32 kUiToastHoldMs = 3'000;
// Linear enter and exit; the next toast enters this long after the previous
// one started leaving.
inline constexpr u32 kUiToastMotionMs = 300;
inline constexpr u32 kUiToastHandoverMs = 400;
inline constexpr float kUiToastMaximumWidth = 280;
inline constexpr float kUiToastIconSize = 18;
inline constexpr u32 kUiToastMaximumLines = 2;

enum class UiToastKind : u8 {
    message, // no icon
    success, // check icon, positive tint
    failure, // error icon, critical tint
};
enum class UiToastPhase : u8 {
    idle,
    entering,
    shown,
    leaving,
    // Between a toast that left and the next one entering.
    waiting,
};

struct UiToastEntry {
    char text[kUiToastTextBytes];
    u8 bytes;
    UiToastKind kind;
};

// Zero-initialized storage is a valid, empty queue.
struct UiToastScene {
    UiToastEntry entries[kUiToastQueueCapacity]; // entries[0] is current
    u64 phase_started_ms;                        // zero: the phase starts at the next update
    UiWidgetReference text_owner;
    UiRect viewport, rectangle, icon, text;
    UiSize text_size;
    float progress; // 0 hidden .. 1 settled; the host maps it to layer opacity
    u32 serial;     // bumps whenever a toast becomes current
    u32 revision;   // geometry or content changed
    u32 measured_serial;
    u32 dropped;
    u8 count;
    UiToastPhase phase;
};

struct UiToastEnvironment {
    const Theme* theme;
    UiTextMeasureCache* measure; // null measures by estimate
    UiRect viewport;
    // false settles each phase at once (hidden or minimized window);
    // reduced motion fades without sliding.
    bool animate, reduced_motion;
};

// Copies the text (truncated at a code point). A repeat of the newest toast
// while it shows restarts its time instead of queueing again. Returns whether
// the queue changed; the host then requests a frame.
bool ui_toast_push(UiToastScene*, UiToastKind, Utf8View text) noexcept;
bool ui_toast_bind_text_owner(UiToastScene*, UiWidgetReference) noexcept;
// Advances the phase, measures a new current toast and places it. Returns
// whether paint output changed (geometry, content or progress).
bool ui_toast_update(UiToastScene*, u64 now_ms, const UiToastEnvironment&) noexcept;
// When the next update is due; zero while idle with nothing queued.
u64 ui_toast_next_wake(const UiToastScene&, u64 now_ms, bool animate) noexcept;
bool ui_toast_visible(const UiToastScene&) noexcept;
// The current toast's text and kind (empty while idle).
Utf8View ui_toast_current_text(const UiToastScene&) noexcept;
UiToastKind ui_toast_current_kind(const UiToastScene&) noexcept;
bool ui_toast_paint(const UiToastScene&, const Theme&, PaintStream*) noexcept;
bool ui_toast_text_output(const UiToastScene&, const Theme&, UiTextOutput*) noexcept;
void ui_toast_clear(UiToastScene*) noexcept;

} // namespace maicord
