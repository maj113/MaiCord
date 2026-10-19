#pragma once

#include "maicord/render/d3d11_renderer.hpp"

namespace maicord {

RendererPresentationStamp renderer_capture_presentation_stamp(
    const D3D11Renderer& renderer, const Theme& theme, bool structural = false) noexcept;
bool renderer_presentation_stamp_equal(
    const RendererPresentationStamp& left,
    const RendererPresentationStamp& right) noexcept;
// When an animated name effect is due, adds the bounds of every animated
// material to presentation damage and advances the effect deadline.
bool renderer_note_text_effect_damage(D3D11Renderer* renderer, u64 now_ms) noexcept;
// Call after a scene rebuild. Records the new frame and, when only paint or
// media quad contents changed since the last presented build, adds damage for
// the old and new bounds of the differing quads (per batch, after trimming
// the common prefix and suffix). Returns false when the whole frame must be
// treated as damaged.
bool renderer_paint_diff_damage(D3D11Renderer* renderer, const Theme& theme) noexcept;
// The recorded frame is on screen (or provably needs no present).
void renderer_paint_diff_commit(D3D11Renderer* renderer) noexcept;
void renderer_paint_diff_release(D3D11Renderer* renderer) noexcept;
// Typing dots (RendererTypingDots): where the last build painted them, whether
// the composer's phase has moved since, and the step that patches the retained
// stream and damages only the dots. A false step leaves everything as it was
// (the caller rebuilds).
void renderer_record_typing_dots(D3D11Renderer* renderer, const UiTypingDotsPaint& paint) noexcept;
bool renderer_typing_dots_pending(const D3D11Renderer& renderer) noexcept;
bool renderer_step_typing_dots(D3D11Renderer* renderer) noexcept;

} // namespace maicord
