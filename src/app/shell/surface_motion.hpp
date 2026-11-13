#pragma once

#include "maicord/render/d3d11/layer_presentation.hpp"
#include "maicord/ui/core/transition.hpp"

namespace maicord::app_detail {

inline constexpr u32 kApplicationSurfaceMotionCount = static_cast<u32>(RendererLayerSurface::count);

// Numeric scene token is compared only, never dereferenced. Gallery owner is
// its process-lifetime account_scope; popup lifetime is its opens counter.
struct ApplicationSurfaceMotionKey {
    u64 scene{}, owner{}, lifetime{};
    u32 kind{};
};
inline bool application_surface_motion_same_key(const ApplicationSurfaceMotionKey& a,
                                                const ApplicationSurfaceMotionKey& b) noexcept {
    return a.scene == b.scene && a.owner == b.owner && a.lifetime == b.lifetime && a.kind == b.kind;
}
struct ApplicationSurfaceMotionSlot {
    // Keep Application's static storage zero-filled; install actual theme
    // defaults lazily, rather than serializing the full host object in .data.
    UiMotionTransition motion{.style = {0, UiMotionCurve::linear}};
    UiMotionStyle style{0, UiMotionCurve::linear};
    ApplicationSurfaceMotionKey key{};
    // The owner's real close while the surface plays its exit (application_surface_motion_exit), else null.
    void (*close)() noexcept{};
};
struct ApplicationSurfaceMotionOwner {
    ApplicationSurfaceMotionSlot slots[kApplicationSurfaceMotionCount]{};
    u64 scope_serial{};
    // The earliest frame a shared-scheduler transition asked for (application_motion_schedule), 0 for none.
    u64 scheduled_ms{};
    bool client_area_animation{}, system_preference_cached{}, styles_initialized{};
};
static_assert(kApplicationSurfaceMotionCount == 6);
static_assert(sizeof(ApplicationSurfaceMotionOwner) == 648);

// Pure controller seam: a zero scene retires the slot. Suppression keeps the
// current identity settled, so uncovering does not replay an entrance. Styles
// are copied values; an invalid style is replaced by the shared safe default.
// Initialize before supplying an explicit copied style (zero duration is valid).
// A slot with a pending close eases to 0 instead of 1; a new key cancels it.
void application_surface_motion_owner_initialize(ApplicationSurfaceMotionOwner*) noexcept;
float application_surface_motion_update(ApplicationSurfaceMotionOwner*, RendererLayerSurface,
                                        ApplicationSurfaceMotionKey, u64 now_ms, bool animate) noexcept;
void application_surface_motion_owner_retire(ApplicationSurfaceMotionOwner*, RendererLayerSurface) noexcept;
u64 application_surface_motion_owner_next_wake(const ApplicationSurfaceMotionOwner&, RendererLayerSurface,
                                               ApplicationSurfaceMotionKey) noexcept;

// The first thing a frame does: consumes the shared scheduler's deadline when it is due. Wakes armed later in the
// frame (every tick re-arms its own next frame) stay armed until the next frame starts, however long this one runs.
void application_surface_motion_begin_frame(u64 now_ms) noexcept;
void application_surface_motion_synchronize(u64 now_ms) noexcept;
u64 application_surface_motion_next_wake() noexcept;
bool application_surface_motion_request_frame_if_due(u64 now_ms, bool frame_available) noexcept;
void application_surface_motion_refresh_system_preference() noexcept;
// Call before closing/releasing an owner, including close/reopen between frames.
void application_surface_motion_retire(RendererLayerSurface) noexcept;

// Modal exit: the surface fades and shrinks to kUiMotionModalScale (kUiMotionModal), then `close`, the owner's
// real close, runs. True when the exit started or one is already pending (the caller does nothing more); false
// when the surface must close at once (reduced motion, a hidden window, a surface not shown): the caller closes it
// now. The surface stays drawn meanwhile; a pointer press, key or wheel event runs every pending close first
// (application_surface_motion_finish_exits), so input never lands on a closing surface.
bool application_surface_motion_exit(RendererLayerSurface, void (*close)() noexcept) noexcept;
void application_surface_motion_finish_exits() noexcept;

// The shared motion scheduler (ui/core/transition.hpp has the vocabulary). Each frame a host draws a moving thing it
// ticks the transition and schedules the next frame it needs; the loop wakes for the earliest deadline and, once
// nothing schedules one, returns to idle (each frame consumes a due deadline first, application_surface_motion_begin_frame).
// No allocation or timer.
void application_motion_schedule(u64 deadline_ms) noexcept;
// Ticks `transition` toward `target` under the application's reduced motion (the user setting or the system's) and
// window visibility, schedules its next frame and returns its value. A bound scope is the caller's
// (ui_motion_transition_reset); an unbound transition is bound to `scope` at `target`, so the first tick shows the
// settled state unless the caller reset it to the start value.
float application_motion_tick(UiMotionTransition*, const UiMotionScope&, float target, u64 now_ms,
                              const UiMotionStyle&) noexcept;
// The same for a hover colour group; true when an amount changed (repaint).
bool application_motion_hover_tick(UiMotionHover*, u64 hovered_id, u64 now_ms,
                                   const UiMotionStyle& = kUiMotionHover) noexcept;

} // namespace maicord::app_detail
