#pragma once

#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/login_text_presentation.hpp"

namespace maicord {
inline constexpr u32 kRendererLoginFormDisplayBytes = 8 * 1024;
// Each admitted confirmation command is at most 2,048 UTF-8 bytes. Keep
// fallback-run capacity for an alternating-script command, but no account
// semantics, interactive hit maps or markdown decoration banks are needed.
inline constexpr u32 kRendererLoginFormGlyphCapacity = 4 * 1024;
inline constexpr u32 kRendererLoginFormUnitCapacity = 2'048 + 1;
struct RendererLoginFormTextStorage {
    Arena arena;
    RendererTextStorage text;
    UiTextCommand commands[kMaximumLoginTextCommands];
    char display[kRendererLoginFormDisplayBytes];
    UiTextCommandStream projection;
    bool valid;
};

inline bool d3d11_renderer_login_form_text(const D3D11Renderer& renderer) noexcept {
    return renderer.login_visible && renderer.login_form_text &&
           renderer.content_text == &renderer.login_form_text->text;
}
bool d3d11_renderer_capture_login_form_text(D3D11Renderer* renderer, const LoginView& view,
                                           const Theme& theme) noexcept;
bool d3d11_renderer_restore_login_form_base(D3D11Renderer* renderer) noexcept;
} // namespace maicord
