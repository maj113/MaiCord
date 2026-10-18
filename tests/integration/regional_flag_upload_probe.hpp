#pragma once

#include "maicord/media/regional_flag_svg.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/core/win32_error.hpp"

namespace maicord {
// The account module's media worker compiles flag SVG; probes reproduce that
// step so the renderer receives the same borrowed paint program.
inline MediaAtlasGpuResult probe_upload_regional_flag(D3D11Renderer* renderer, MediaAssetKey key,
                                                      ConstSlice<u8> svg) noexcept {
    RegionalFlagSvg image{};
    ColrPaintProgram program{};
    const bool compiled = regional_flag_svg_parse(svg, &image) &&
                          regional_flag_svg_compile(image, &program, kRegionalFlagPixelsPerUnit);
    const auto uploaded = compiled ? renderer_upload_regional_flag(renderer, key, program)
                                   : MediaAtlasGpuResult{{}, E_FAIL, MediaAtlasGpuError::invalid_input};
    arena_release(&program.storage);
    return uploaded;
}
} // namespace maicord
