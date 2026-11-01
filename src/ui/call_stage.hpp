#pragma once
#include "maicord/ui/call_scene.hpp"
#include "maicord/ui/scene_kit.hpp"

// Stage-channel parts of the call view (internal to call_scene.cpp).
namespace maicord::call_stage {
// Helpers shared with call_scene.cpp.
// Copies at most capacity bytes without splitting a UTF-8 sequence.
u32 copy_text(char* destination, u32 capacity, Utf8View text) noexcept;
UiRect centered(float x, float y, float width, float height) noexcept;
// A fill darkened by hover and press.
inline Color shaded(Color fill, bool hovered, bool pressed) noexcept {
    const float shade = pressed ? .8f : hovered ? .9f : 1.f;
    fill.red *= shade;
    fill.green *= shade;
    fill.blue *= shade;
    return fill;
}

// Copies content.stage into the scene; clears every stage field when inactive.
void copy(UiCallScene& s, const UiCallStage& stage) noexcept;
// Splits the stage area into banner, hand raises, audience band and the
// speakers' tile area (returned).
UiRect layout(UiCallScene& s, UiRect area, float gap) noexcept;
// Header (start, topic, end) and banner (accept, decline) controls.
void place_controls(UiCallScene& s) noexcept;
// Left edge of the header's stage buttons (header.right when none).
float header_left(const UiCallScene& s) noexcept;
// Bar order for a stage: mute, deafen, stage actions, disconnect.
u32 bar_order(const UiCallScene& s, UiCallControl* output, u32 capacity) noexcept;
UiRect invite_button(const UiCallScene& s, u32 request) noexcept;
// Per-row invite under the point (control none when nothing is hit).
UiCallHit hit(const UiCallScene& s, float x, float y) noexcept;
u64 hash(const UiCallScene& s, u64 seed) noexcept;
Utf8View label(const UiCallScene& s, UiCallControl control) noexcept;
bool paint(PaintStream* out, const UiCallScene& s, const Theme& theme) noexcept;
bool paint_foreground(PaintStream* out, const UiCallScene& s, const Theme& theme) noexcept;
// Stage labels through the call scene's text pen.
void text(UiTextPen<UiTextClip::label>& pen, const UiCallScene& s, const Theme& theme) noexcept;
bool avatar(const UiCallScene& s, u32 index, UiCallTileMedia* output) noexcept;
} // namespace maicord::call_stage
