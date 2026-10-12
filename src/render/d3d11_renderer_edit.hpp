#pragma once
#include "maicord/ui/edit_field.hpp"

namespace maicord {

struct D3D11Renderer;
struct LoginView;

bool d3d11_renderer_measure_login_edit(D3D11Renderer* renderer, const LoginView& view) noexcept;
bool d3d11_renderer_measure_composer_edit(D3D11Renderer* renderer) noexcept;
using PopupEditGeometry = UiEditGeometry;
bool d3d11_renderer_measure_profile_edit(D3D11Renderer* renderer) noexcept;
bool d3d11_renderer_measure_settings_edit(D3D11Renderer* renderer) noexcept;
UiEditGeometry d3d11_renderer_profile_edit_geometry(const D3D11Renderer& renderer) noexcept;
bool d3d11_renderer_measure_popup_edit(D3D11Renderer* renderer) noexcept;
PopupEditGeometry d3d11_renderer_popup_edit_geometry(const D3D11Renderer& renderer) noexcept;

} // namespace maicord
