#pragma once

#include "maicord/media/image/regional_flag_art.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/core/win32_error.hpp"

namespace maicord {
// The account module's media worker compiles the bundled flag record; probes
// reproduce that step so the renderer receives the same borrowed paint program.
inline MediaAtlasGpuResult probe_upload_regional_flag(D3D11Renderer* renderer, u16 sequence) noexcept {
    RegionalFlagArt art;
    ColrPaintProgram program{};
    const bool compiled =
        regional_flag_record_parse(regional_flag_bundled_record(sequence), &art) &&
        regional_flag_compile(art, &program);
    const auto uploaded = compiled ? renderer_upload_regional_flag(renderer, media_asset_regional_flag(sequence), program)
                                   : MediaAtlasGpuResult{{}, E_FAIL, MediaAtlasGpuError::invalid_input};
    arena_release(&program.storage);
    return uploaded;
}
} // namespace maicord
