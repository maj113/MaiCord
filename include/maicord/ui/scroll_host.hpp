#pragma once

#include "maicord/ui/smooth_scroll.hpp"
#include "maicord/ui/widget_manager.hpp"

namespace maicord {

inline constexpr float kUiScrollLineDips = 32;

struct UiScrollRange {
    UiRect viewport;
    double offset;
    double maximum;
    double line;
    double units_per_dip;
    double origin = 0;
    u64 generation = 0;
};

// Virtualized content provides geometry and an absolute offset setter only.
// False from read means the owner is no longer alive/visible.
struct UiScrollBinding {
    void* context;
    u64 key;
    bool (*read)(void*, u64, UiScrollRange*) noexcept;
    bool (*write)(void*, u64, double, bool*) noexcept;
};

struct UiScrollEntry {
    UiScrollBinding binding;
    UiSmoothScroll animation;
    double position;
    double reported_position;
    double origin;
    u64 generation;
};

// Active animations and fractional immediate positions share a fixed number of slots.
// Inactive slots are evictable; no per-widget allocation or immediate timers.
inline constexpr u32 kUiScrollHostCapacity = 16;
struct UiScrollHost {
    UiScrollEntry entries[kUiScrollHostCapacity];
    bool immediate = false;
};
enum class UiScrollDispatch : u8 {
    unhandled,
    handled,
    failed
};

UiScrollDispatch ui_scroll_host_wheel(UiScrollHost* host, UiScrollBinding binding, UiPoint point,
                                      i32 wheel_delta, u32 lines, bool page, u64 now_us,
                                      double dips_per_pixel = 1.0) noexcept;
// Reveal a content interval using the same animation as wheel input. Bounds
// are in the binding's scroll units; oversized intervals align to their start.
UiScrollDispatch ui_scroll_host_reveal(UiScrollHost* host, UiScrollBinding binding, double first,
                                       double last, u64 now_us, double dips_per_pixel = 1.0) noexcept;
// Moves to an absolute offset (clamped) with the wheel curve, retargeting a
// running curve; jumps when the host is immediate. For keyboard paging and
// scroll-into-view targets an owner computed itself.
UiScrollDispatch ui_scroll_host_scroll_to(UiScrollHost* host, UiScrollBinding binding, double target,
                                          u64 now_us, double dips_per_pixel = 1.0) noexcept;
// Deepest eligible scroll_view under the point wins; all scroll_view widgets
// share the same input, timing and boundary policy.
UiScrollDispatch ui_scroll_host_wheel_widgets(
    UiScrollHost* host, UiWidgetManager* widgets, UiPoint point, i32 wheel_delta, u32 lines, bool page,
    u64 now_us, double dips_per_pixel = 1.0,
    UiWidgetHandle virtual_owner = ui_widget_invalid_handle()) noexcept;
bool ui_scroll_host_tick(UiScrollHost* host, u64 now_us, bool* changed) noexcept;
bool ui_scroll_host_active(const UiScrollHost& host) noexcept;
void ui_scroll_host_cancel(UiScrollHost* host) noexcept;

} // namespace maicord
