// The pure surface motion controller: each surface's key, transition and wake, without the application host
// (surface_motion.cpp), so tests run it on its own.
#include "maicord/core/memory.hpp"
#include "surface_motion.hpp"

namespace maicord::app_detail {
namespace {
bool valid_surface(RendererLayerSurface surface) noexcept {
    return static_cast<u32>(surface) < kApplicationSurfaceMotionCount;
}
bool valid_key(RendererLayerSurface surface, const ApplicationSurfaceMotionKey& key) noexcept {
    return key.scene && (surface == RendererLayerSurface::gallery ? key.owner != 0 : key.lifetime != 0);
}
bool stale_key(RendererLayerSurface surface, const ApplicationSurfaceMotionKey& previous,
               const ApplicationSurfaceMotionKey& key) noexcept {
    if (!previous.scene)
        return false;
    if (surface == RendererLayerSurface::gallery)
        return key.owner < previous.owner;
    return key.scene == previous.scene && key.lifetime < previous.lifetime;
}
} // namespace

void application_surface_motion_owner_initialize(ApplicationSurfaceMotionOwner* owner) noexcept {
    if (!owner || owner->styles_initialized)
        return;
    for (auto& slot : owner->slots)
        slot.style = kUiDefaultMotionStyle;
    owner->styles_initialized = true;
}

void application_surface_motion_owner_retire(ApplicationSurfaceMotionOwner* owner,
                                             RendererLayerSurface surface) noexcept {
    if (!owner || !valid_surface(surface))
        return;
    auto& slot = owner->slots[static_cast<u32>(surface)];
    ui_motion_transition_cancel(&slot.motion);
    reset_value(&slot.key);
    slot.close = nullptr;
}

float application_surface_motion_update(ApplicationSurfaceMotionOwner* owner, RendererLayerSurface surface,
                                        ApplicationSurfaceMotionKey key, u64 now_ms,
                                        bool animate) noexcept {
    if (!owner || !valid_surface(surface))
        return 1;
    application_surface_motion_owner_initialize(owner);
    auto& slot = owner->slots[static_cast<u32>(surface)];
    if (!valid_key(surface, key)) {
        application_surface_motion_owner_retire(owner, surface);
        return 1;
    }
    if (stale_key(surface, slot.key, key))
        return slot.motion.value;
    if (!ui_motion_style_valid(slot.style))
        slot.style = kUiDefaultMotionStyle;
    if (!application_surface_motion_same_key(slot.key, key)) {
        const bool successor = slot.key.scene != 0;
        slot.key = key;
        slot.close = nullptr;
        // Account/source replacement is not an opening animation. Popup opens
        // supply their own lifetime, so their entrance can start.
        const bool entrance = animate && (!successor || surface != RendererLayerSurface::gallery);
        if (owner->scope_serial == ~u64{0}) {
            ui_motion_transition_cancel(&slot.motion);
            slot.motion.value = 1;
            return 1;
        }
        const UiMotionScope scope{static_cast<u64>(surface) + 1, ++owner->scope_serial};
        ui_motion_transition_reset(&slot.motion, scope, entrance ? 0.0f : 1.0f);
    }
    ui_motion_transition_tick(&slot.motion, slot.motion.scope, slot.close ? 0 : 1, now_ms, slot.style, !animate);
    return slot.motion.value;
}

u64 application_surface_motion_owner_next_wake(const ApplicationSurfaceMotionOwner& owner,
                                               RendererLayerSurface surface,
                                               ApplicationSurfaceMotionKey key) noexcept {
    if (!valid_surface(surface) || !valid_key(surface, key))
        return 0;
    const auto& slot = owner.slots[static_cast<u32>(surface)];
    return application_surface_motion_same_key(slot.key, key)
               ? ui_motion_transition_next_wake(slot.motion, slot.motion.scope, false)
               : 0;
}

} // namespace maicord::app_detail
