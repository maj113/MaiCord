#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {

struct D3D11Renderer;
struct Theme;
struct Color;

// The window caption pass (renderer_set_caption_height): a few paint quads and
// one shaped title line in their own instance buffers, drawn after the content
// stream into the caption rows of the swap chain. Each back buffer is
// repainted only after the caption changes there, and the change is presented
// as its own dirty rectangle.

// The caption must be drawn into the next presented buffer.
bool d3d11_renderer_caption_pending(D3D11Renderer* renderer, const Theme& theme) noexcept;
// Draws when pending or when the frame repaints the whole target.
bool d3d11_renderer_caption_draw(D3D11Renderer* renderer, const Theme& theme) noexcept;
// The caption rectangle when this frame drew it: returns 1 and fills `rectangle`.
u32 d3d11_renderer_caption_dirty_rect(const D3D11Renderer& renderer, RECT* rectangle) noexcept;
// After Present: the other back buffer still holds the previous caption.
void d3d11_renderer_caption_presented(D3D11Renderer* renderer) noexcept;
// A content draw's full-window scrims (modals, dialogs, the media viewer):
// begun with each content draw, noted with the layer opacity they draw at.
// The caption dims its band under the same colour.
void d3d11_renderer_caption_begin_scrim(D3D11Renderer* renderer) noexcept;
void d3d11_renderer_caption_note_scrim(D3D11Renderer* renderer, Color color, float opacity) noexcept;
// The target lost the caption's pixels (resize, clear, height change).
void d3d11_renderer_caption_invalidate(D3D11Renderer* renderer) noexcept;
void d3d11_renderer_caption_release(D3D11Renderer* renderer) noexcept;

} // namespace maicord
