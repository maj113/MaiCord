#pragma once
#include "maicord/render/text_edit_layout.hpp"
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
// Measures a search-style field over `bounds` into `layout` and its draw
// geometry, whose right edge never falls short of `bounds`. A failure
// invalidates the layout only when `invalidate_on_failure` is set.
bool d3d11_renderer_measure_search_edit(TextEditLayoutOwner* layout, TextEditMetric* selections,
                                        const UiEditView& edit, const UiRect& bounds, const TextSystem& text,
                                        bool invalidate_on_failure, UiEditGeometry* geometry) noexcept;

} // namespace maicord
