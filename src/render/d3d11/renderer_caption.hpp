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
// repainted only after the caption changes there (or an animated backdrop
// moves under it), and the change is presented as its own dirty rectangle.
// Content draws never reach the caption rows: their viewport starts below.

// The caption must be drawn into the next presented buffer.
bool d3d11_renderer_caption_pending(D3D11Renderer* renderer, const Theme& theme) noexcept;
// Draws when pending, when the content covered its tooltip or when an animated
// backdrop repaints the whole target; builds and uploads its quads again only
// after they changed.
bool d3d11_renderer_caption_draw(D3D11Renderer* renderer, const Theme& theme) noexcept;
// Quad builds (and uploads) and draws so far.
u32 d3d11_renderer_caption_builds(const D3D11Renderer& renderer) noexcept;
u32 d3d11_renderer_caption_draws(const D3D11Renderer& renderer) noexcept;
// The caption rectangle when this frame drew it: returns 1 and fills `rectangle`.
u32 d3d11_renderer_caption_dirty_rect(const D3D11Renderer& renderer, RECT* rectangle) noexcept;
// After Present: the other back buffer still holds the previous caption.
void d3d11_renderer_caption_presented(D3D11Renderer* renderer) noexcept;
// A content draw's full-window scrims (modals, dialogs, the media viewer):
// begun with each content draw, noted with the layer opacity they draw at.
// The caption dims its band under the same colour.
void d3d11_renderer_caption_begin_scrim(D3D11Renderer* renderer) noexcept;
void d3d11_renderer_caption_note_scrim(D3D11Renderer* renderer, Color color, float opacity) noexcept;
// The target lost the caption's pixels (new buffers, clear, height change).
void d3d11_renderer_caption_invalidate(D3D11Renderer* renderer) noexcept;
void d3d11_renderer_caption_release(D3D11Renderer* renderer) noexcept;

} // namespace maicord
