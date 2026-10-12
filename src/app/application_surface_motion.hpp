#pragma once

#include "maicord/render/layer_presentation.hpp"
#include "maicord/ui/transition.hpp"

namespace maicord::app_detail {

inline constexpr u32 kApplicationSurfaceMotionCount = static_cast<u32>(RendererLayerSurface::count);

// Numeric scene token is compared only, never dereferenced. Gallery owner is
// its process-lifetime account_scope; popup lifetime is its opens counter.
struct ApplicationSurfaceMotionKey {
    u64 scene{}, owner{}, lifetime{};
    u32 kind{};
};
struct ApplicationSurfaceMotionSlot {
    // Keep Application's static storage zero-filled; install actual theme
    // defaults lazily, rather than serializing the full host object in .data.
    UiMotionTransition motion{.style = {0, UiMotionCurve::linear}};
    UiMotionStyle style{0, UiMotionCurve::linear};
    ApplicationSurfaceMotionKey key{};
};
struct ApplicationSurfaceMotionOwner {
    ApplicationSurfaceMotionSlot slots[kApplicationSurfaceMotionCount]{};
    u64 scope_serial{};
    bool client_area_animation{}, system_preference_cached{}, styles_initialized{};
};
static_assert(kApplicationSurfaceMotionCount == 7);
static_assert(sizeof(ApplicationSurfaceMotionOwner) == 688);

// Pure controller seam: a zero scene retires the slot. Suppression keeps the
// current identity settled, so uncovering does not replay an entrance. Styles
// are copied values; an invalid style is replaced by the shared safe default.
// Initialize before supplying an explicit copied style (zero duration is valid).
void application_surface_motion_owner_initialize(ApplicationSurfaceMotionOwner*) noexcept;
float application_surface_motion_update(ApplicationSurfaceMotionOwner*, RendererLayerSurface,
                                        ApplicationSurfaceMotionKey, u64 now_ms, bool animate) noexcept;
void application_surface_motion_owner_retire(ApplicationSurfaceMotionOwner*, RendererLayerSurface) noexcept;
u64 application_surface_motion_owner_next_wake(const ApplicationSurfaceMotionOwner&, RendererLayerSurface,
                                               ApplicationSurfaceMotionKey, u64 now_ms) noexcept;

void application_surface_motion_synchronize(u64 now_ms) noexcept;
u64 application_surface_motion_next_wake(u64 now_ms) noexcept;
bool application_surface_motion_request_frame_if_due(u64 now_ms, bool frame_available) noexcept;
void application_surface_motion_refresh_system_preference() noexcept;
// Call before closing/releasing an owner, including close/reopen between frames.
void application_surface_motion_retire(RendererLayerSurface) noexcept;
void application_surface_motion_retire_all() noexcept;

} // namespace maicord::app_detail
