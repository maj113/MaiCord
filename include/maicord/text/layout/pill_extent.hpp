#pragma once

#include "maicord/core/base.hpp"

namespace maicord {
// The nearest whole device pixel, also for the negative origins of text scrolled above its clip. Glyph text draws its
// block origin and every baseline this way, so a pill laid against the text's baseline rounds the same.
inline float text_whole_pixels(float pixels) noexcept {
    const float shifted = pixels + .5f;
    const i32 truncated = static_cast<i32>(shifted);
    return static_cast<float>(static_cast<float>(truncated) > shifted ? truncated - 1 : truncated);
}

// Where a pill sits on its line. The font's ascent and descent box is not the text's visual centre: ascent reserves room
// for accents above the capitals, so a pill drawn on the line box lands high (Segoe UI's box centre is .41 em above the
// baseline, the ink of "@game" .24 em: its capitals reach .71 em and its descenders .24 em below). Pills are centred
// .24 em above the baseline instead, which sits within a few hundredths of an em of the ink centre of Latin UI faces
// (half their x-height, .25 to .27 em), and sized in ems so every font size and UI scale gets the same proportions.
inline constexpr float kTextPillCentre = .24f;
// A mention in running text (it must not touch the pill of the next wrapped line, whose line pitch is about 1.33 em)
// and a command option or game chip in the composer (a bordered box with room around the text).
inline constexpr float kTextInlinePillHeight = 1.4f;
inline constexpr float kTextChipPillHeight = 1.6f;

struct TextPillExtent {
    float top, bottom;
};
// `baseline` is the line's baseline and `em` the text's size, in the same units as the result.
constexpr TextPillExtent text_pill_extent(float baseline, float em, float height_in_ems) noexcept {
    const float middle = baseline - kTextPillCentre * em, half = height_in_ems * em * .5f;
    return {middle - half, middle + half};
}
} // namespace maicord
