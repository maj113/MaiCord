#pragma once
#include "maicord/core/base.hpp"

// The data of the background effect pass (see background_fx.hpp): what the
// renderer's effect owner stores, without the policy functions or the theme.
namespace maicord {

inline constexpr u32 kBackgroundFxParameters = 8;
inline constexpr u32 kBackgroundFxPaletteRoles = 8;

// mc_fx_constants (192 bytes, 16-byte rows), then one host-only row: the
// canvas in DIPs, read by the effect vertex shader (and the built-in pixel
// shader). Plugin shaders declare only the first 192 bytes.
struct BackgroundFxConstants {
    float time, delta, intensity, dark;
    float resolution[2], origin[2];
    float palette[kBackgroundFxPaletteRoles][4];
    float parameters[kBackgroundFxParameters];
    float canvas[4];
};
static_assert(sizeof(BackgroundFxConstants) == 208);
static_assert(__builtin_offsetof(BackgroundFxConstants, palette) == 32 &&
              __builtin_offsetof(BackgroundFxConstants, parameters) == 160 &&
              __builtin_offsetof(BackgroundFxConstants, canvas) == 192);

enum class BackgroundFxSource : u8 {
    none,
    builtin,
    plugin
};

// GPU guard: a plugin effect averaging more than 2 ms of GPU time over a
// second, or taking more than 8 ms in three frames, is turned off.
inline constexpr float kBackgroundFxAverageLimitMs = 2;
inline constexpr float kBackgroundFxStallMs = 8;
inline constexpr u32 kBackgroundFxStallLimit = 3;
inline constexpr u64 kBackgroundFxWindowMs = 1000;
struct BackgroundFxGuard {
    u64 window_start_ms;
    float window_total_ms;
    u32 window_frames;
    u32 stalls;
};

} // namespace maicord
