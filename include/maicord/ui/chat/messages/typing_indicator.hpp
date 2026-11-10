#pragma once
#include "maicord/ui/core/paint.hpp"

namespace maicord {
// The three dots before "X is typing…", as the official client draws them:
// 7 DIP dots 8.75 apart, each pulsing between 80% size at 30% opacity and full
// size and opacity once every 1.2 s, the next one 150 ms behind. An inactive
// window or reduced motion holds them at full size. Stateless geometry; the
// host owns the clock.
inline constexpr u32 kUiTypingDotCount = 3;
inline constexpr u32 kUiTypingDotsPulseMs = 1'200;
inline constexpr u32 kUiTypingDotsStaggerMs = 150;
// Frame quantum (20 per second): every dot's pose changes on these steps.
inline constexpr u32 kUiTypingDotsStepMs = 50;
inline constexpr u32 kUiTypingDotsPhases = kUiTypingDotsPulseMs / kUiTypingDotsStepMs;
static_assert(kUiTypingDotsPulseMs % kUiTypingDotsStepMs == 0 && kUiTypingDotsStaggerMs % kUiTypingDotsStepMs == 0);
// The held pose: every dot at full size and opacity.
inline constexpr u16 kUiTypingDotsRest = 0xFFFF;
inline constexpr float kUiTypingDotSize = 7.f, kUiTypingDotPitch = 8.75f;
// The dots' box starts this far into the typing row; the label follows after
// kUiTypingDotsGap (kUiTypingLabelInset in all).
inline constexpr float kUiTypingDotsInset = 8.f, kUiTypingDotsGap = 4.f;
inline constexpr float kUiTypingDotsWidth = kUiTypingDotSize + (kUiTypingDotCount - 1) * kUiTypingDotPitch;
inline constexpr float kUiTypingLabelInset = kUiTypingDotsInset + kUiTypingDotsWidth + kUiTypingDotsGap;
// One name's byte span inside the typing label; the label draws it bold.
struct MessageTypingName {
    u16 offset, bytes;
};
// When one to three names do not fit the row the label collapses, as the official client does: its
// "Multiple people" (the span) replaces the names. The names fit while their width plus this reserve for the
// dots and margin stays within the width of the row from the dots on.
inline constexpr Utf8View kUiTypingCollapsedLabel = utf8_literal("Multiple people are typing...");
inline constexpr MessageTypingName kUiTypingCollapsedName{0, 15};
inline constexpr float kUiTypingOverflowReserve = 56.f;
// The fade that lets messages scrolled under the typing strip dissolve into the chat background (the official
// client's chat gradient): `kUiTypingFadeIdle` DIPs at the list's bottom, or `kUiTypingFadeTyping` DIPs ending
// where the strip begins (a solid band of the background follows it) while someone types; it stops
// `kUiTypingFadeRightInset` short of the list's right edge, which keeps the scrollbar clear.
inline constexpr float kUiTypingFadeIdle = 16.f, kUiTypingFadeTyping = 72.f, kUiTypingFadeRightInset = 16.f;
// The label's line (and the dots) sit this far below the middle of the row: centred 12 DIP above the composer
// like the official client's 24 DIP wrapper, and clear of the list's bottom edge the strip overlaps. The row's
// own clip grows by it, into the gap below the row.
inline constexpr float kUiTypingLabelDrop = 2.f;
// Phase at now, or kUiTypingDotsRest when not animating.
u16 ui_typing_dots_phase(u64 now_ms, bool animate) noexcept;
// Dot `index` at `phase`: scale (of kUiTypingDotSize) and opacity.
void ui_typing_dot_pose(u32 index, u16 phase, float* scale, float* opacity) noexcept;
// The dots' box in a typing row.
UiRect ui_typing_dots_row(UiRect row) noexcept;
// The dots' paint quads, as ui_typing_dots_paint pushes them.
void ui_typing_dots_quads(const UiRect& row, const Color&, u16 phase, PaintQuad output[kUiTypingDotCount]) noexcept;
// Always kUiTypingDotCount quads, whatever the phase.
bool ui_typing_dots_paint(PaintStream*, const UiRect& row, const Color&, u16 phase) noexcept;
// Where a frame painted animated dots, so a step can patch their quads in
// place: first_quad of `quads` under `clip`, painted at `phase`.
struct UiTypingDotsPaint {
    UiRect row, clip;
    Color color;
    u32 first_quad, quads;
    u16 phase;
};
// Host frame clock: each presented frame ticks it once with whether a typing
// line is on screen and may animate. Animating dots get their phase and the
// next step's deadline; anything else rests and disarms it, so idle typing
// state never wakes the loop.
struct UiTypingDotsClock {
    u64 wake_ms;
    u16 phase;
    bool visible;
};
// True when the painted phase changed.
bool ui_typing_dots_clock_tick(UiTypingDotsClock*, u64 now_ms, bool visible, bool animate) noexcept;
bool ui_typing_dots_clock_due(const UiTypingDotsClock&, u64 now_ms) noexcept;
} // namespace maicord
