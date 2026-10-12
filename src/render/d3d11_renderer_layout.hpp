#pragma once

namespace maicord {

struct D3D11Renderer;
struct LoginView;
struct RendererPresentationStamp;
struct RendererShellSegment;
struct Theme;


bool d3d11_renderer_paint_layout(D3D11Renderer* renderer, const Theme& theme,
                                 const RendererPresentationStamp& current) noexcept;
// Builds the paint stream for the current bindings without presenting it.
bool d3d11_renderer_build_paint(D3D11Renderer* renderer, const Theme& theme) noexcept;
void d3d11_renderer_shell_segment_release(RendererShellSegment* segment) noexcept;
bool d3d11_renderer_rebuild_login_text(D3D11Renderer* renderer, const LoginView& view,
                                       const Theme& theme) noexcept;
bool d3d11_renderer_rebuild_text_mode(D3D11Renderer* renderer, const Theme& theme,
                                      bool include_messages) noexcept;

} // namespace maicord
