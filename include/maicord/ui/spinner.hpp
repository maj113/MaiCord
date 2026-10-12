#pragma once
#include "maicord/ui/paint.hpp"

namespace maicord {
inline constexpr u32 kUiSpinnerPhases = 32;
inline constexpr u32 kUiSpinnerStepMs = 32;
struct UiSpinnerStyle {
    float stroke{2.5f};
    float tail_opacity{.18f};
};
// Stateless, allocation-free geometry. Hosts own visibility and the shared
// frame deadline; the painter never creates a timer or samples an OS clock.
u8 ui_spinner_phase(u64 now_ms, bool reduced_motion) noexcept;
u64 ui_spinner_next_wake(u64 now_ms, bool visible, bool reduced_motion) noexcept;
// Host-owned frame clock shared by every loading spinner on screen. Each
// presented frame samples it once: visible spinners get the current phase and
// a deadline for the next step; hidden ones (or reduced motion) disarm it so
// an idle panel never polls. The phase belongs in each scene's paint stamp.
struct UiSpinnerClock {
    u64 wake_ms;
    u8 phase;
    bool visible;
};
// Returns true when the painted phase changed (the host repaints).
bool ui_spinner_clock_tick(UiSpinnerClock*, u64 now_ms, bool visible, bool reduced_motion) noexcept;
bool ui_spinner_clock_due(const UiSpinnerClock&, u64 now_ms) noexcept;
bool ui_spinner_paint(PaintStream*, UiRect bounds, Color, u8 phase, const UiSpinnerStyle& = {}) noexcept;
} // namespace maicord
