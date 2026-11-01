#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

struct VideoDisplayExtent {
    u16 width;
    u16 height;
};

// Natural square-pixel size of the cropped picture. Grow width for wide
// pixels and height for narrow pixels, rounding once. This describes layout,
// never the decoder surface, atlas allocation, or upload size. Unrepresentable
// metadata fails at the current 16-bit media-canvas limit.
constexpr VideoDisplayExtent video_display_extent(u32 width, u32 height,
                                                   u32 sar_width, u32 sar_height) noexcept {
    if (!width || !height || width > 0xFFFFu || height > 0xFFFFu ||
        !sar_width || !sar_height) return {};
    const u64 display_width = sar_width >= sar_height
        ? (static_cast<u64>(width) * sar_width + sar_height / 2u) / sar_height : width;
    const u64 display_height = sar_width < sar_height
        ? (static_cast<u64>(height) * sar_height + sar_width / 2u) / sar_width : height;
    if (display_width > 0xFFFFu || display_height > 0xFFFFu) return {};
    return {static_cast<u16>(display_width), static_cast<u16>(display_height)};
}

// The natural extent after the container's clockwise quarter turns; pixel
// aspect applies to the decoded picture first, so odd turns swap the result.
constexpr VideoDisplayExtent video_display_rotated(VideoDisplayExtent extent, u8 rotation) noexcept {
    return (rotation & 1u) ? VideoDisplayExtent{extent.height, extent.width} : extent;
}

} // namespace maicord
