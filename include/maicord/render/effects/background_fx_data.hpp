#pragma once
#include "maicord/core/base.hpp"

// The data of the built-in background effect pass (see background_fx.hpp): what the renderer's
// owner stores, without the policy functions or the theme.
namespace maicord {

inline constexpr u32 kBackgroundFxParameters = 8;
inline constexpr u32 kBackgroundFxPaletteRoles = 8;

// Twelve 16-byte rows (time, delta, intensity and dark; the canvas size and origin in pixels; the
// theme palette; the parameters), then two rows read by the effect vertex shader: the canvas in DIPs, and
// the sprite grid (first star cell, star cells per row, comet lanes).
struct BackgroundFxConstants {
    float time, delta, intensity, dark;
    float resolution[2], origin[2];
    float palette[kBackgroundFxPaletteRoles][4];
    float parameters[kBackgroundFxParameters];
    float canvas[4];
    float grid[4];
};
static_assert(sizeof(BackgroundFxConstants) == 224);
static_assert(__builtin_offsetof(BackgroundFxConstants, palette) == 32 &&
              __builtin_offsetof(BackgroundFxConstants, parameters) == 160 &&
              __builtin_offsetof(BackgroundFxConstants, canvas) == 192 &&
              __builtin_offsetof(BackgroundFxConstants, grid) == 208);

enum class BackgroundFxSource : u8 {
    none,
    builtin
};

} // namespace maicord
