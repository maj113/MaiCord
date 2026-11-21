#pragma once

#include "maicord/render/d3d11/renderer.hpp"

namespace maicord {

// The content text builds the renderer finished: each one counts once, as a base build or an overlay build.
inline u32 content_text_builds(const D3D11Renderer& renderer) noexcept {
    return renderer.content_text_prefix.base_builds + renderer.content_text_prefix.overlay_builds;
}

} // namespace maicord
