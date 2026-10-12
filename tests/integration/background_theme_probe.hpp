#pragma once
#include "maicord/ui/theme.hpp"
namespace maicord {
struct D3D11Renderer;
struct BackgroundThemeProbeContext {
    D3D11Renderer& renderer;
    bool (*rebuild)() noexcept;
    bool (*capture)(const wchar_t*, const Theme&) noexcept = nullptr;
};
// Original fixed-color, in-memory native GPU proof. Optional captures use the
// caller's existing fixture capture owner; no image/asset source is loaded.
bool background_theme_ui_probe(BackgroundThemeProbeContext) noexcept;
} // namespace maicord
